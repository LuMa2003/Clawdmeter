"""Windows host-lock detector — feeds the firmware's light-sleep trigger.

Listens to `WM_WTSSESSION_CHANGE` (0x02B1) via `WTSRegisterSessionNotification`
and invokes a callback whenever the host PC transitions between locked and
unlocked. The daemon stuffs the latest lock state into every BLE payload so
the firmware can immediately fade to its light-sleep state when the user
walks away (`idle_set_host_locked()` in firmware/src/idle.cpp).

Why a hidden window? `WTSRegisterSessionNotification` only delivers events
via a window message queue. The listener creates a zero-area `MessageOnly`
window in a background thread and pumps its messages — no UI, no taskbar
entry, no impact on the tray icon's own message loop.
"""

from __future__ import annotations

import logging
import threading

# WM_WTSSESSION_CHANGE and its wParam subcodes are not exposed in pywin32's
# win32con / win32ts — define inline. Values per Microsoft Learn:
# learn.microsoft.com/en-us/windows/win32/termserv/wm-wtssession-change
_WM_WTSSESSION_CHANGE = 0x02B1
_WTS_SESSION_LOCK     = 0x07
_WTS_SESSION_UNLOCK   = 0x08


class HostLockListener:
    """Background thread that calls `on_change(locked: bool)` on lock edges.

    Start with `.start()`; the thread runs until the process exits (the
    message pump is daemonized so process shutdown tears it down for free).
    Stop the registration cleanly with `.stop()` if needed.

    `on_change` is invoked from the listener thread, not the main thread —
    callbacks should do only quick atomic state writes (e.g. update a
    `TrayState` bool). No locks needed for simple scalar writes in CPython.
    """

    def __init__(self, on_change) -> None:
        self._on_change = on_change
        self._thread: threading.Thread | None = None
        self._hwnd = None
        self._atom = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self._run, name="host-lock-listener", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        # Post WM_CLOSE to drop the message pump; the thread exits when
        # PumpMessages returns. Best-effort: if anything's mid-init, the
        # daemon=True flag handles teardown at process exit anyway.
        if self._hwnd is not None:
            try:
                import win32gui
                win32gui.PostMessage(self._hwnd, 0x0010, 0, 0)  # WM_CLOSE
            except Exception:
                pass

    def _run(self) -> None:
        try:
            import win32api
            import win32con
            import win32gui
            import win32ts
        except ImportError as e:
            logging.getLogger(__name__).warning(
                "pywin32 not available — host-lock detection disabled: %s", e
            )
            return

        def wnd_proc(hwnd, msg, wparam, lparam):
            if msg == _WM_WTSSESSION_CHANGE:
                if wparam == _WTS_SESSION_LOCK:
                    try: self._on_change(True)
                    except Exception: logging.getLogger(__name__).exception("on_change(True) raised")
                elif wparam == _WTS_SESSION_UNLOCK:
                    try: self._on_change(False)
                    except Exception: logging.getLogger(__name__).exception("on_change(False) raised")
                return 0
            if msg == win32con.WM_CLOSE:
                try: win32ts.WTSUnRegisterSessionNotification(hwnd)
                except Exception: pass
                win32gui.DestroyWindow(hwnd)
                return 0
            if msg == win32con.WM_DESTROY:
                win32gui.PostQuitMessage(0)
                return 0
            return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

        wc = win32gui.WNDCLASS()
        wc.lpfnWndProc = wnd_proc
        wc.lpszClassName = "ClawdmeterHostLockListener"
        wc.hInstance = win32api.GetModuleHandle(None)
        self._atom = win32gui.RegisterClass(wc)

        # HWND_MESSAGE = -3 — a "message-only" window: invisible, not enumerated,
        # only receives messages. The right kind of window for a pure listener.
        self._hwnd = win32gui.CreateWindowEx(
            0, self._atom, "Clawdmeter Lock Listener",
            0, 0, 0, 0, 0,
            -3,  # HWND_MESSAGE
            0, wc.hInstance, None,
        )

        win32ts.WTSRegisterSessionNotification(self._hwnd, win32ts.NOTIFY_FOR_THIS_SESSION)
        logging.getLogger(__name__).info("host-lock listener started")

        # Pumps until WM_QUIT is posted (via WM_CLOSE → DestroyWindow → WM_DESTROY).
        win32gui.PumpMessages()
        self._hwnd = None
