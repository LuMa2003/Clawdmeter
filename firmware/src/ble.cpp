#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

#define DEVICE_NAME "Clawdmeter"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request

#define BLE_BUF_SIZE 512

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static char mac_str[18];

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet: name + the 128-bit custom data-service UUID.
    // No HID service is exposed (device is display-only), no special
    // appearance code — Windows BLE scanners pick the device up by name and
    // by the custom UUID, which is all the daemon needs.
    adv->setName(DEVICE_NAME);
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: connected from %s (active=%u)\n",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());

        // Request low-duty-cycle connection parameters. Default NimBLE
        // intervals are ~30-50 ms which keeps the radio chirping constantly.
        // We use slave latency instead: keep a short interval (37.5 ms) so
        // HID keystrokes can fire on the very next interval when we have
        // data to send, but let the peripheral skip up to 8 intervals when
        // idle. Effective idle radio activity: ~300-450 ms between wake-ups,
        // ~5-10x less than the default. The central may negotiate down,
        // but most stacks (incl. Windows BT) honor reasonable requests.
        //
        // Units: min/max in 1.25 ms; timeout in 10 ms.
        //   min=30 → 37.5 ms, max=60 → 75 ms, latency=8, timeout=500 → 5 s
        // Supervision timeout must satisfy: timeout > (1 + latency) * max
        //   (1+8) * 75 ms = 675 ms < 5000 ms ✓
        s->updateConnParams(info.getConnHandle(), 30, 60, 8, 500);

        // Only one connection slot (daemon). NimBLE auto-stops advertising on
        // accept; no need to restart it until the daemon disconnects.
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
};

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the daemon's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_bonds(void) {
    return NimBLEDevice::getNumBonds() > 0;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_disconnect_all(void) {
    // Stop advertising first so no new peer races in while we're closing
    // out the existing ones.
    NimBLEDevice::stopAdvertising();

    if (!server) return;

    // Single connection slot — there's at most one peer (the daemon), but
    // the loop is harmless in case a future code path connects more.
    const uint8_t n = server->getConnectedCount();
    for (uint8_t i = 0; i < n; i++) {
        NimBLEConnInfo info = server->getPeerInfo(i);
        const uint16_t handle = info.getConnHandle();
        if (handle != 0xFFFF) {
            server->disconnect(handle);
        }
    }
    Serial.printf("BLE: disconnected %u peer(s)\n", n);
}
