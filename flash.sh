#!/bin/bash
# Build and flash Clawdmeter firmware on Linux.
# Usage:
#   ./flash.sh                  # default port /dev/ttyACM0
#   ./flash.sh /dev/ttyACM1     # explicit USB serial port
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/ttyACM0}"

echo "=== Flashing Clawdmeter ==="
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
~/.platformio/penv/bin/pio run -e waveshare_amoled_216 -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="
