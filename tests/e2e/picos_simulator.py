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
from collections import deque
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
        test_mode: bool = False,
    ):
        self.binary_path = binary_path or str(self.DEFAULT_BINARY)
        self.sd_card_path = sd_card_path or str(self.DEFAULT_SD_CARD)
        self.headless = headless
        self.requested_port = tcp_port
        self.tcp_port: Optional[int] = None  # actual port after start
        self.timeout = timeout
        # --test-mode: Lua error screens and launch refusals return at once
        # (their text goes to the log's "err" source) and idle dim is off.
        self.test_mode = test_mode
        self.process: Optional[subprocess.Popen] = None
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._reader_thread: Optional[threading.Thread] = None
        self._reader_done = threading.Event()
        # Notifications other than `log`, in arrival order, each tagged with
        # a receive index ("_rx") so waits can ignore ones that predate a
        # launch. `log` notifications go to _log_events.
        self._notifications: list[dict] = []
        self._notif_lock = threading.Lock()
        self._notif_cond = threading.Condition(self._notif_lock)
        self._rx_counter = 0
        self._launch_rx = 0
        self._log_events: deque = deque(maxlen=50000)
        self._logs_subscribed = False
        self._pending: dict[int, dict] = {}
        self._pending_lock = threading.Lock()
        self._id_counter = 0
        self._connected = False
        self._recv_buf = ""
        # Bounded tails of the child's stdout/stderr. The pipes MUST be drained
        # continuously (see _start_pipe_drains) or the simulator deadlocks once
        # the 64KB kernel pipe buffer fills; these keep the output available for
        # diagnostics without growing without bound.
        self._stdout_tail: deque = deque(maxlen=2000)
        self._stderr_tail: deque = deque(maxlen=2000)
        self._drain_threads: list[threading.Thread] = []

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
        if self.test_mode:
            cmd.append("--test-mode")

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

        # From here on nothing else reads these pipes, so they must be drained
        # continuously — see _start_pipe_drains for why.
        self._start_pipe_drains()

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

    def _start_pipe_drains(self):
        """Continuously drain the child's stdout/stderr.

        The simulator is spawned with stdout=PIPE and stderr=PIPE, but after
        _parse_port() nothing reads them again. Once the kernel pipe buffer
        (64KB) fills, the simulator blocks in write(). That is not a clean
        stall: the blocking write happens inside fflush() on the socket thread
        while it holds the stdio FILE lock, so the main thread then blocks in
        printf() waiting for that lock and the whole simulator deadlocks —
        RPCs stop being answered and every subsequent call times out.

        This showed up as 'App hung on cycle 5/10' in the stress tests: it took
        about five app launches' worth of output to fill the pipe.
        """
        def drain(stream, tail):
            try:
                for raw in iter(stream.readline, b""):
                    tail.append(raw.decode("utf-8", errors="replace").rstrip("\n"))
            except (ValueError, OSError):
                pass  # stream closed during shutdown
            finally:
                try:
                    stream.close()
                except Exception:
                    pass

        for stream, tail in ((self.process.stdout, self._stdout_tail),
                             (self.process.stderr, self._stderr_tail)):
            if stream is None:
                continue
            t = threading.Thread(target=drain, args=(stream, tail), daemon=True)
            t.start()
            self._drain_threads.append(t)

    def get_output(self) -> dict:
        """Recent simulator stdout/stderr, for diagnostics."""
        return {
            "stdout": "\n".join(self._stdout_tail),
            "stderr": "\n".join(self._stderr_tail),
        }

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
                        with self._notif_cond:
                            self._rx_counter += 1
                            msg["_rx"] = self._rx_counter
                            if msg.get("method") == "log":
                                self._log_events.append(msg.get("params", {}))
                            else:
                                self._notifications.append(msg)
                            self._notif_cond.notify_all()
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

    def wait_for_notification(self, method: str, timeout: Optional[float] = None,
                              after_rx: int = 0) -> dict:
        """Wait for (and consume) the first `method` notification received
        after receive index `after_rx`."""
        deadline = time.time() + (timeout or self.timeout)
        with self._notif_cond:
            while True:
                for i, n in enumerate(self._notifications):
                    if n.get("method") == method and n.get("_rx", 0) > after_rx:
                        return self._notifications.pop(i)
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"Notification '{method}' not received within timeout")
                self._notif_cond.wait(remaining)

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
        """Queue an app launch. Returns {"queued": True, "busy": bool}.

        The launch runs when the launcher loop next runs (after any running
        app exits). An app staged after boot is found by the sim's rescan-on-
        miss. Use wait_for_exit() for the outcome.
        """
        with self._notif_cond:
            self._launch_rx = self._rx_counter
        return self.call("launch_app", {"name": name})

    def rescan_apps(self) -> dict:
        """Ask the launcher to rescan /apps (e.g. after changing an app.json)."""
        return self.call("rescan_apps")

    def exit_app(self) -> dict:
        """Exit the currently running app."""
        return self.call("exit_app")

    def wait_for_exit(self, timeout: Optional[float] = None) -> dict:
        """Wait for the app from the last launch_app() to finish.

        Returns the app.exited params:
        {name, id, found, result, error, runtime_ms} where result is
        "returned" | "error" | "exit_sentinel" | "load_failed".
        """
        notif = self.wait_for_notification(
            "app.exited", timeout=timeout or 30.0, after_rx=self._launch_rx)
        return notif.get("params", {})

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

    def get_input_state(self) -> dict:
        """{issued_seq, consumed_seq}: every injection with seq <= consumed_seq
        has been read by the OS input layer."""
        return self.call("get_input_state")

    def wait_input_consumed(self, seq: int, timeout: Optional[float] = None) -> dict:
        """Wait until the injection with `seq` (the input_seq returned by
        keypress/inject_*) has been read by the OS."""
        deadline = time.time() + (timeout or self.timeout)
        while True:
            state = self.get_input_state()
            if state.get("consumed_seq", 0) >= seq:
                return state
            if time.time() >= deadline:
                raise TimeoutError(f"input seq {seq} not consumed: {state}")
            time.sleep(0.01)

    def present_count(self) -> int:
        """Frames presented to the (simulated) panel since boot."""
        return self.call("display_stats").get("present_count", 0)

    def wait_frames(self, n: int = 1, timeout: Optional[float] = None) -> int:
        """Wait until `n` more frames have been presented. Returns the new
        present_count."""
        target = self.present_count() + n
        deadline = time.time() + (timeout or self.timeout)
        while True:
            count = self.present_count()
            if count >= target:
                return count
            if time.time() >= deadline:
                raise TimeoutError(f"only {count - target + n} of {n} frames presented")
            time.sleep(0.01)

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

    def get_log_buffer(self, since_seq: int = 0, tail: int = 0) -> dict:
        """Log entries with seq >= since_seq (0 = everything still held).

        Returns {lines: [{seq, t_ms, src, text}], next_seq, dropped, more}.
        src is "lua" | "native" | "os" | "err". Pass next_seq back as
        since_seq to read only newer entries; "more" means the response was
        paged. tail > 0 returns only the newest `tail` entries.
        """
        params = {"since_seq": since_seq}
        if tail:
            params["tail"] = tail
        return self.call("get_log_buffer", params)

    def get_log_lines(self, since_seq: int = 0) -> list[dict]:
        """Every held entry with seq >= since_seq, following pages."""
        return self._read_log(since_seq)[0]

    def _read_log(self, since_seq: int) -> tuple[list[dict], int]:
        out: list[dict] = []
        seq = since_seq
        while True:
            page = self.get_log_buffer(since_seq=seq)
            out.extend(page.get("lines", []))
            seq = page.get("next_seq", seq)
            if not page.get("more"):
                return out, seq

    def clear_log(self) -> dict:
        """Clear the simulator log buffer (sequence numbers keep counting)."""
        return self.call("clear_log_buffer")

    def subscribe_logs(self, on: bool = True) -> dict:
        """Receive every new log entry as a `log` notification."""
        result = self.call("subscribe", {"logs": on})
        self._logs_subscribed = on
        return result

    def wait_for_log(self, pattern: str, timeout: Optional[float] = None,
                     since_seq: int = 0, src: Optional[str] = None) -> str:
        """Wait until a log entry matching the regex appears (optionally only
        from source `src`) and return its text.

        Event-driven: subscribes to log notifications, then checks the entries
        already held from since_seq on (0 = all), then waits for new ones.
        """
        regex = re.compile(pattern)

        def match(entry: dict) -> bool:
            if src is not None and entry.get("src") != src:
                return False
            return bool(regex.search(entry.get("text", "")))

        if not self._logs_subscribed:
            self.subscribe_logs(True)
        deadline = time.time() + (timeout or self.timeout)
        # Held entries first; everything later arrives as a notification
        # (the subscription is already active, so nothing falls in between).
        held, cursor = self._read_log(since_seq)
        for entry in held:
            if match(entry):
                return entry.get("text", "")
        with self._notif_cond:
            while True:
                for entry in list(self._log_events):
                    if entry.get("seq", 0) >= cursor:
                        cursor = entry["seq"] + 1
                        if match(entry):
                            return entry.get("text", "")
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"Log pattern '{pattern}' not found within timeout")
                self._notif_cond.wait(remaining)

    def get_terminal_buffer(self) -> dict:
        """Get the active terminal's text buffer."""
        return self.call("get_terminal_buffer")

    def get_heap_info(self) -> dict:
        """Get heap usage info."""
        return self.call("get_heap_info")

    def set_time_multiplier(self, multiplier: float) -> dict:
        """Scale hal_sleep_ms() delays only. It does NOT change the clock Lua
        sees (sys.sleep, getTimeMs use real wall time), so it cannot speed up
        or slow down app timing."""
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
