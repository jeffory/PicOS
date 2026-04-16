"""PicOS Simulator control wrapper for E2E testing.

Uses JSON-RPC 2.0 over TCP to communicate with the simulator, matching
the protocol used by the MCP server (tools/picos_mcp.py).

Usage:
    with PicosSimulator(headless=True) as sim:
        sim.launch_app("hello")
        screenshot = sim.screenshot()
        sim.exit_app()
"""

import base64
import io
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import threading
from pathlib import Path
from typing import Any, Optional, List


class PicosSimulator:
    """Controls PicOS Simulator process for E2E testing via JSON-RPC 2.0."""

    # Project root (two levels up from tests/e2e/)
    PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
    DEFAULT_BINARY = PROJECT_ROOT / "build_sim" / "picos_simulator"
    DEFAULT_SD_CARD = PROJECT_ROOT / "simulator" / "assets" / "sd_card"

    def __init__(
        self,
        binary_path: Optional[str] = None,
        sd_card_path: Optional[str] = None,
        headless: bool = True,
        tcp_port: int = 0,
        timeout: float = 10.0,
    ):
        self.binary_path = binary_path or str(self.DEFAULT_BINARY)
        self.sd_card_path = sd_card_path or str(self.DEFAULT_SD_CARD)
        self.headless = headless
        self.requested_port = tcp_port
        self.tcp_port: Optional[int] = None  # actual port after start
        self.timeout = timeout
        self.process: Optional[subprocess.Popen] = None
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._reader_thread: Optional[threading.Thread] = None
        self._reader_done = threading.Event()
        self._notifications: list[dict] = []
        self._notif_lock = threading.Lock()
        self._pending: dict[int, dict] = {}
        self._pending_lock = threading.Lock()
        self._id_counter = 0
        self._connected = False
        self._recv_buf = ""

    # ── Context Manager ──────────────────────────────────────────────────────

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def start(self):
        """Start the simulator process and connect."""
        cmd = [
            self.binary_path,
            "--sd-card", self.sd_card_path,
            "--port", str(self.requested_port),
        ]

        env = os.environ.copy()
        if self.headless:
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIODRIVER"] = "dummy"

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )

        # Parse the actual TCP port from simulator stdout
        self.tcp_port = self._parse_port()

        # Connect and start reader
        self._connect()

    def stop(self):
        """Gracefully stop the simulator."""
        # Try RPC shutdown first
        if self._connected:
            try:
                self.call("shutdown", timeout=1.0)
            except Exception:
                pass

        self._disconnect()

        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
            self.process = None

    def _parse_port(self) -> int:
        """Read stdout lines until we find '[Socket] TCP port: N'."""
        deadline = time.time() + self.timeout
        port_re = re.compile(r"\[Socket\] TCP port: (\d+)")

        while time.time() < deadline:
            if self.process.poll() is not None:
                stdout = self.process.stdout.read().decode() if self.process.stdout else ""
                stderr = self.process.stderr.read().decode() if self.process.stderr else ""
                raise RuntimeError(
                    f"Simulator exited early (code {self.process.returncode})\n"
                    f"stdout: {stdout}\nstderr: {stderr}"
                )

            # Non-blocking read from stdout
            line = self.process.stdout.readline().decode("utf-8", errors="replace")
            if not line:
                time.sleep(0.05)
                continue

            m = port_re.search(line)
            if m:
                return int(m.group(1))

        # Fallback: if requested_port != 0, try that
        if self.requested_port > 0:
            return self.requested_port
        raise TimeoutError("Could not detect simulator TCP port")

    def _connect(self):
        """Connect TCP and start reader thread."""
        delays = [0.1, 0.2, 0.4, 0.8, 1.6]
        last_err = None
        for i, delay in enumerate(delays):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(self.timeout)
                sock.connect(("127.0.0.1", self.tcp_port))
                self._sock = sock
                self._connected = True
                break
            except (socket.error, ConnectionRefusedError) as e:
                last_err = e
                sock.close()
                if i < len(delays) - 1:
                    time.sleep(delay)

        if not self._connected:
            raise RuntimeError(
                f"Cannot connect to simulator port {self.tcp_port}: {last_err}"
            )

        self._reader_done.clear()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def _disconnect(self):
        """Close socket and reader thread."""
        self._connected = False
        self._reader_done.set()
        if self._reader_thread and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=2.0)
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        with self._pending_lock:
            for entry in self._pending.values():
                entry["event"].set()
            self._pending.clear()

    # ── Reader Thread ─────────────────────────────────────────────────────────

    def _reader_loop(self):
        """Single reader thread routing all incoming JSON messages."""
        buf = ""
        while not self._reader_done.is_set():
            if not self._sock:
                break
            try:
                self._sock.settimeout(0.2)
                chunk = self._sock.recv(65536)
                if not chunk:
                    break
                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if "id" in msg and msg["id"] is not None:
                        with self._pending_lock:
                            entry = self._pending.get(msg["id"])
                            if entry:
                                entry["result"] = msg
                                entry["event"].set()
                    else:
                        with self._notif_lock:
                            self._notifications.append(msg)
            except socket.timeout:
                continue
            except OSError:
                break
        self._connected = False

    # ── JSON-RPC ──────────────────────────────────────────────────────────────

    def call(self, method: str, params: Optional[dict] = None,
             timeout: Optional[float] = None) -> Any:
        """Send a JSON-RPC 2.0 request and return the result."""
        if not self._connected or not self._sock:
            raise RuntimeError("Not connected to simulator")

        self._id_counter += 1
        req_id = self._id_counter

        event = threading.Event()
        with self._pending_lock:
            self._pending[req_id] = {"event": event, "result": None}

        payload = json.dumps({
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params or {},
        }) + "\n"

        with self._lock:
            self._sock.sendall(payload.encode())

        t = timeout or self.timeout
        if not event.wait(timeout=t):
            with self._pending_lock:
                self._pending.pop(req_id, None)
            raise TimeoutError(f"Timeout calling {method} (>{t}s)")

        with self._pending_lock:
            entry = self._pending.pop(req_id, None)

        if not entry or not entry["result"]:
            raise RuntimeError(f"No response for {method}")

        msg = entry["result"]
        if "error" in msg:
            err = msg["error"]
            raise RuntimeError(f"RPC error [{err.get('code')}]: {err.get('message')}")
        return msg.get("result", {})

    # ── Notifications ─────────────────────────────────────────────────────────

    def get_notifications(self) -> list[dict]:
        """Return and clear accumulated notifications."""
        with self._notif_lock:
            notifs = self._notifications.copy()
            self._notifications.clear()
        return notifs

    def wait_for_notification(self, method: str, timeout: Optional[float] = None) -> dict:
        """Wait until a notification with the given method arrives."""
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            with self._notif_lock:
                for i, n in enumerate(self._notifications):
                    if n.get("method") == method:
                        self._notifications.pop(i)
                        return n
            time.sleep(0.05)
        raise TimeoutError(f"Notification '{method}' not received within timeout")

    # ── High-Level Helpers ────────────────────────────────────────────────────

    def ping(self) -> bool:
        """Check if simulator is responsive."""
        try:
            result = self.call("ping", timeout=2.0)
            # Ping returns {"version":"...", "uptime_ms":N}
            return "version" in result or "uptime_ms" in result
        except Exception:
            return False

    def get_status(self) -> dict:
        """Get simulator status (running app, heap, wifi)."""
        app = self.call("get_running_app")
        heap = self.call("get_heap_info")
        wifi = self.call("get_wifi_state")
        return {"app": app, "heap": heap, "wifi": wifi}

    def list_apps(self) -> list[str]:
        """List available apps on the SD card."""
        result = self.call("list_dir", {"path": "/apps"})
        entries = result.get("entries", [])
        return [e["name"] for e in entries if e.get("is_dir")]

    def launch_app(self, name: str) -> dict:
        """Launch an app by name. Returns result dict."""
        return self.call("launch_app", {"name": name})

    def exit_app(self) -> dict:
        """Exit the currently running app."""
        return self.call("exit_app")

    def wait_for_exit(self, timeout: Optional[float] = None) -> dict:
        """Wait for the current app to exit."""
        return self.call("wait_for_exit", timeout=timeout or 30.0)

    # Named buttons recognized by inject_button
    _BUTTONS = {
        "up", "down", "left", "right", "enter", "esc", "escape",
        "menu", "sym", "backspace", "tab", "del", "shift",
        "f1", "f2", "f3", "f4", "f5",
    }

    def keypress(self, key: str) -> dict:
        """Send a keypress to the simulator.

        Named keys: up, down, left, right, enter, esc, menu, f1-f5,
        backspace, tab, del, shift.
        Character keys: a-z, A-Z, 0-9, space, punctuation — sent via inject_char.
        """
        if key.lower() in self._BUTTONS:
            return self.call("inject_button", {"button": key, "action": "click"})
        elif len(key) == 1:
            return self.call("inject_char", {"char": key})
        else:
            return self.call("inject_button", {"button": key, "action": "click"})

    def keypress_sequence(self, keys: list[str], delay_ms: int = 100):
        """Send a sequence of keypresses with delays."""
        for key in keys:
            self.keypress(key)
            time.sleep(delay_ms / 1000.0)

    def screenshot(self) -> bytes:
        """Take a screenshot and return PNG bytes."""
        result = self.call("screenshot")
        png_b64 = result.get("data", "") or result.get("png_base64", "")
        return base64.b64decode(png_b64)

    def screenshot_pil(self):
        """Take a screenshot and return a PIL Image (requires Pillow)."""
        from PIL import Image
        png_data = self.screenshot()
        return Image.open(io.BytesIO(png_data))

    def get_log_buffer(self, since_seq: int = 0) -> dict:
        """Get the simulator log buffer. Returns {lines: [...], next_seq: int}."""
        return self.call("get_log_buffer", {"since_seq": since_seq})

    def clear_log(self) -> dict:
        """Clear the simulator log buffer."""
        return self.call("clear_log_buffer")

    def wait_for_log(self, pattern: str, timeout: Optional[float] = None,
                     since_seq: int = 0) -> str:
        """Wait until a log line matching the regex pattern appears.

        Returns the matching log line.
        """
        regex = re.compile(pattern)
        deadline = time.time() + (timeout or self.timeout)
        seq = since_seq
        while time.time() < deadline:
            result = self.get_log_buffer(since_seq=seq)
            for line in result.get("lines", []):
                text = line if isinstance(line, str) else line.get("text", "")
                if regex.search(text):
                    return text
            seq = result.get("next_seq", seq)
            time.sleep(0.1)
        raise TimeoutError(f"Log pattern '{pattern}' not found within timeout")

    def get_terminal_buffer(self) -> dict:
        """Get the active terminal's text buffer."""
        return self.call("get_terminal_buffer")

    def get_heap_info(self) -> dict:
        """Get heap usage info."""
        return self.call("get_heap_info")

    def set_time_multiplier(self, multiplier: float) -> dict:
        """Set time multiplier for simulation speed."""
        return self.call("set_time_multiplier", {"multiplier": multiplier})

    # ── Build Helper ──────────────────────────────────────────────────────────

    @classmethod
    def build(cls, jobs: int = 4) -> Path:
        """Build the simulator, returning the binary path."""
        build_dir = cls.PROJECT_ROOT / "build_sim"
        build_dir.mkdir(exist_ok=True)

        subprocess.run(
            ["cmake", "-B", str(build_dir), str(cls.PROJECT_ROOT / "simulator")],
            cwd=str(cls.PROJECT_ROOT),
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["make", "-C", str(build_dir), f"-j{jobs}"],
            check=True,
            capture_output=True,
        )
        return build_dir / "picos_simulator"
