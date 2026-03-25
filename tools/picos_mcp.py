#!/usr/bin/env python3
"""PicOS MCP Server — exposes PicOS simulator to coding agents via JSON-RPC 2.0.

Supports both PicOS Simulator (default, via TCP socket) and hardware devices
via USB serial: ping, screenshot, list_apps, launch_app, exit_app, reboot,
send_command, and more.

Usage:
    Registered in .mcp.json as an MCP server.
    Run with simulator (default): python3 tools/picos_mcp.py
    Run with hardware device: python3 tools/picos_mcp.py --hardware
"""

import argparse
import asyncio
import base64
import json
import os
import platform
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any, Optional

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False
if TYPE_CHECKING:
    import serial

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("picos")

DEFAULT_TIMEOUT = 5
DEFAULT_TCP_PORT = 7878
HARDWARE_MODE = False

SCRN_MAGIC = b"SCRN"
SCRN_HEADER_SIZE = 12


# ── JSON-RPC 2.0 Client ─────────────────────────────────────────────────────────

class JRpcError(Exception):
    def __init__(self, code: int, message: str, data: Any = None):
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"[{code}] {message}")


class SimulatorConnection:
    """Manages a connection to the PicOS simulator via JSON-RPC 2.0 over TCP.

    Uses a single reader thread that routes ALL incoming messages:
    - Messages with an "id" → delivered to the corresponding pending call via Event
    - Messages without "id" → appended to notifications list
    This eliminates the race condition between call() and notification reading.
    """

    def __init__(self, tcp_port: int | None = None):
        self.tcp_port = tcp_port or DEFAULT_TCP_PORT
        self._sock: socket.socket | None = None
        self._lock = threading.Lock()  # protects _sock writes
        self._reader_thread: threading.Thread | None = None
        self._reader_done = threading.Event()
        self._notifications: list[dict] = []
        self._notif_lock = threading.Lock()
        self._id_counter = 1
        # Pending responses: id → {"event": Event, "result": dict|None}
        self._pending: dict[int, dict] = {}
        self._pending_lock = threading.Lock()
        self._connected = False

    def connect(self) -> None:
        if self._connected:
            return

        # Retry with exponential backoff
        delays = [0.1, 0.2, 0.4, 0.8, 1.6]
        last_error = None
        for attempt, delay in enumerate(delays):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(DEFAULT_TIMEOUT)
                sock.connect(("127.0.0.1", self.tcp_port))
                self._sock = sock
                self._connected = True
                break
            except (socket.error, ConnectionRefusedError) as e:
                last_error = e
                if sock:
                    sock.close()
                if attempt < len(delays) - 1:
                    time.sleep(delay)

        if not self._connected:
            raise RuntimeError(
                f"Cannot connect to simulator on port {self.tcp_port} "
                f"after {len(delays)} attempts. Is the simulator running? "
                f"Error: {last_error}"
            )

        # Start single reader thread
        self._reader_done.clear()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def close(self) -> None:
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
        # Wake any pending calls
        with self._pending_lock:
            for entry in self._pending.values():
                entry["event"].set()
            self._pending.clear()

    def _reader_loop(self) -> None:
        """Single reader thread: routes ALL incoming JSON messages."""
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
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    if "id" in obj:
                        # Response to a call — route to pending future
                        msg_id = obj["id"]
                        with self._pending_lock:
                            entry = self._pending.get(msg_id)
                            if entry:
                                entry["result"] = obj
                                entry["event"].set()
                    else:
                        # Notification
                        with self._notif_lock:
                            self._notifications.append(obj)
            except socket.timeout:
                continue
            except OSError:
                break

        self._connected = False

    def _send_raw(self, data: str) -> None:
        with self._lock:
            if not self._sock:
                raise RuntimeError("Not connected to simulator")
            self._sock.sendall((data + "\n").encode("utf-8"))

    def call(self, method: str, params: dict | None = None, timeout: float = DEFAULT_TIMEOUT) -> dict:
        """Send a JSON-RPC 2.0 request and return the result."""
        if not self._connected:
            self.connect()

        jid = self._id_counter
        self._id_counter += 1

        event = threading.Event()
        with self._pending_lock:
            self._pending[jid] = {"event": event, "result": None}

        request = {
            "jsonrpc": "2.0",
            "id": jid,
            "method": method,
            "params": params or {},
        }
        try:
            self._send_raw(json.dumps(request))
        except RuntimeError:
            with self._pending_lock:
                self._pending.pop(jid, None)
            raise

        if not event.wait(timeout):
            with self._pending_lock:
                self._pending.pop(jid, None)
            raise JRpcError(-32000, f"Timeout calling {method} after {timeout}s")

        with self._pending_lock:
            entry = self._pending.pop(jid, None)

        if not entry or not entry["result"]:
            raise JRpcError(-32000, f"Connection lost while calling {method}")

        result = entry["result"]
        if "error" in result:
            err = result["error"]
            raise JRpcError(
                err.get("code", -32603),
                err.get("message", "Unknown error"),
                err.get("data"),
            )
        return result.get("result", {})

    def get_notifications(self) -> list[dict]:
        with self._notif_lock:
            notes = list(self._notifications)
            self._notifications.clear()
        return notes

    def wait_for_notification(self, method: str | None = None, timeout: float = 30.0) -> dict | None:
        """Wait for a specific notification (or any if method=None)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._notif_lock:
                for i, n in enumerate(self._notifications):
                    if method is None or n.get("method") == method:
                        del self._notifications[i]
                        return n
            time.sleep(0.05)
        return None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()


# ── Global connection (lazy) ──────────────────────────────────────────────────────

_conn: SimulatorConnection | None = None
_conn_lock = threading.Lock()
_configured_port = DEFAULT_TCP_PORT


def get_connection() -> SimulatorConnection:
    global _conn
    with _conn_lock:
        if _conn is None:
            _conn = SimulatorConnection(tcp_port=_configured_port)
        # Try to reconnect if connection was lost
        if not _conn._connected:
            try:
                _conn.close()
            except Exception:
                pass
            _conn = SimulatorConnection(tcp_port=_configured_port)
        return _conn


# ── Simulator Lifecycle Management ────────────────────────────────────────────

class SimulatorManager:
    """Manages simulator process lifecycle."""

    def __init__(self):
        self.process: subprocess.Popen | None = None
        self.port: int = 0
        self.project_root = Path(__file__).parent.parent

    def ensure_running(self, port: int = 0, sd_card_path: str | None = None,
                       headless: bool = False) -> int:
        """Ensure a simulator is running. Returns the TCP port.

        If port=0, auto-assigns a port.
        """
        # Check if already running on requested port
        if port > 0 and self._probe_port(port):
            self.port = port
            return port

        # Check if our managed process is still alive
        if self.process and self.process.poll() is None and self.port > 0:
            if self._probe_port(self.port):
                return self.port

        # Build if needed
        binary = self.project_root / "build_sim" / "picos_simulator"
        if not binary.exists():
            self._build()

        # Start simulator
        cmd = [str(binary)]
        if port >= 0:
            cmd += ["--port", str(port)]
        if sd_card_path:
            cmd += ["--sd-card", sd_card_path]
        else:
            cmd += ["--sd-card", str(self.project_root)]

        env = os.environ.copy()
        if headless:
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIODRIVER"] = "dummy"

        self.process = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
            text=True,
        )

        # Read stdout to find the assigned port
        actual_port = port if port > 0 else 0
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                stdout = self.process.stdout.read() if self.process.stdout else ""
                stderr = self.process.stderr.read() if self.process.stderr else ""
                raise RuntimeError(
                    f"Simulator exited early (code {self.process.returncode})\n"
                    f"stdout: {stdout}\nstderr: {stderr}"
                )
            line = self.process.stdout.readline() if self.process.stdout else ""
            if "[Socket] TCP port:" in line:
                actual_port = int(line.split(":")[-1].strip())
                break
            if actual_port > 0 and self._probe_port(actual_port):
                break
            time.sleep(0.1)

        if actual_port <= 0:
            raise RuntimeError("Failed to determine simulator TCP port")

        # Wait for port to be connectable
        start = time.monotonic()
        while time.monotonic() - start < 5.0:
            if self._probe_port(actual_port):
                break
            time.sleep(0.1)

        self.port = actual_port
        return actual_port

    def stop(self):
        """Stop managed simulator."""
        if self.process:
            # Try graceful shutdown via RPC
            if self.port > 0:
                try:
                    conn = SimulatorConnection(tcp_port=self.port)
                    conn.connect()
                    conn.call("shutdown", timeout=2)
                    conn.close()
                except Exception:
                    pass

            # Wait briefly, then terminate
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.process = None
            self.port = 0

    def _build(self):
        """Build the simulator."""
        build_dir = self.project_root / "build_sim"
        sim_dir = self.project_root / "simulator"
        subprocess.run(
            ["cmake", "-B", str(build_dir), str(sim_dir)],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["make", "-C", str(build_dir), "-j4"],
            check=True, capture_output=True,
        )

    @staticmethod
    def _probe_port(port: int) -> bool:
        """Check if a TCP port is accepting connections."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(("127.0.0.1", port))
            s.close()
            return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            return False


_sim_manager: SimulatorManager | None = None


# ── Hardware connection helpers ─────────────────────────────────────────────────

def find_usb_device() -> str | None:
    import glob
    for pattern in ["/dev/ttyACM*", "/dev/tty.usbmodem*"]:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_serial(port: str, timeout: float = DEFAULT_TIMEOUT):
    if not HAS_SERIAL:
        raise RuntimeError("pyserial not installed. Run: pip install pyserial")
    ser = serial.Serial(port, baudrate=115200, timeout=timeout)
    ser.reset_input_buffer()
    time.sleep(0.05)
    ser.reset_input_buffer()
    return ser


# ── Simulator helpers ────────────────────────────────────────────────────────────

def do_screenshot_simulator(timeout: float = DEFAULT_TIMEOUT) -> bytes:
    conn = get_connection()
    result = conn.call("screenshot", {"format": "png"}, timeout=timeout)
    png_b64 = result.get("data", "")
    return base64.b64decode(png_b64)


def rgb565be_to_png(data: bytes, width: int, height: int) -> bytes:
    import io
    from PIL import Image
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        hi = data[i * 2]
        lo = data[i * 2 + 1]
        pixel = (hi << 8) | lo
        r = ((pixel >> 11) & 0x1F) * 255 // 31
        g = ((pixel >> 5) & 0x3F) * 255 // 63
        b = (pixel & 0x1F) * 255 // 31
        pixels[i * 3] = r
        pixels[i * 3 + 1] = g
        pixels[i * 3 + 2] = b
    img = Image.frombytes("RGB", (width, height), bytes(pixels))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


# ── Hardware helpers ────────────────────────────────────────────────────────────

def do_command_hardware(cmd: str, port: str, timeout: float = DEFAULT_TIMEOUT) -> list[str]:
    ser = open_serial(port, timeout)
    try:
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        lines = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            if line.startswith("[DEV] ") and any(
                kw in line for kw in ["pong", "Total:", "Error:", "Launching", "Rebooting", "Unknown"]
            ):
                break
        return lines
    finally:
        ser.close()


def do_screenshot_hardware(port: str, timeout: float = DEFAULT_TIMEOUT) -> bytes:
    ser = open_serial(port, timeout)
    try:
        ser.write(b"screenshot\n")
        ser.flush()
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            buf += chunk
            idx = buf.find(SCRN_MAGIC)
            if idx >= 0:
                buf = buf[idx:]
                break
        else:
            raise TimeoutError("Timed out waiting for SCRN header")
        while len(buf) < SCRN_HEADER_SIZE:
            chunk = ser.read(SCRN_HEADER_SIZE - len(buf))
            if not chunk:
                raise TimeoutError("Timed out reading header")
            buf += chunk
        width, height, fmt = struct.unpack_from("<HHH", buf, 4)
        pixel_bytes = width * height * 2
        data = buf[SCRN_HEADER_SIZE:]
        while len(data) < pixel_bytes:
            remaining = pixel_bytes - len(data)
            chunk = ser.read(min(remaining, 4096))
            if not chunk:
                raise TimeoutError(f"Timed out reading pixels ({len(data)}/{pixel_bytes})")
            data += chunk
        return rgb565be_to_png(data[:pixel_bytes], width, height)
    finally:
        ser.close()


# ── Unified dispatch ───────────────────────────────────────────────────────────

def resolve_port(device: str | None = None) -> str | None:
    if HARDWARE_MODE:
        if device:
            return device
        port = find_usb_device()
        if not port:
            raise RuntimeError(
                "No PicOS hardware device found. Connect via USB or use --simulator flag."
            )
        return port
    return None


# ── MCP Tools ──────────────────────────────────────────────────────────────────


@mcp.tool()
async def ping(device: str | None = None) -> str:
    """Ping the PicOS device or simulator. Returns 'pong' on success."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "ping", port)
            for line in lines:
                if "pong" in line.lower():
                    return "pong"
            return f"No pong. Response: {lines}"
        except Exception as e:
            return str(e)
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "ping", timeout=5)
            return f"pong (uptime: {result.get('uptime_ms', '?')}ms)"
        except Exception as e:
            return f"Cannot connect: {e}"


@mcp.tool()
async def screenshot(device: str | None = None) -> list:
    """Take a screenshot of the PicOS display. Returns PNG image."""
    port = resolve_port(device)
    if port:
        try:
            png_bytes = await asyncio.to_thread(do_screenshot_hardware, port)
        except ImportError:
            return [{"type": "text", "text": "Pillow not installed: pip install Pillow"}]
        except Exception as e:
            return [{"type": "text", "text": f"Error: {e}"}]
    else:
        try:
            png_bytes = await asyncio.to_thread(do_screenshot_simulator)
        except Exception as e:
            return [{"type": "text", "text": f"Error: {e}"}]

    png_b64 = base64.b64encode(png_bytes).decode("ascii")
    return [
        {"type": "image", "data": png_b64, "mimeType": "image/png"},
        {"type": "text", "text": f"Screenshot (320x320 PNG)"},
    ]


@mcp.tool()
async def list_apps(device: str | None = None) -> str:
    """List all apps installed on the PicOS device or simulator."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "list", port)
            apps = []
            for line in lines:
                if "[DEV] Available apps:" in line:
                    continue
                if "[DEV] Total:" in line:
                    break
                if line.strip().startswith("  ") or line.strip().startswith("-"):
                    apps.append(line.strip())
            return "\n".join(apps) if apps else "\n".join(lines)
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "list_dir", {"path": "/apps"})
            entries = result.get("entries", [])
            if not entries:
                return "(no apps found)"
            lines = []
            for e in entries:
                if e.get("is_dir"):
                    lines.append(f"  {e['name']}/")
                else:
                    lines.append(f"  {e['name']}  ({e.get('size', 0)} bytes)")
            return "\n".join(lines)
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def launch_app(app_name: str, device: str | None = None) -> str:
    """Launch an app by name on the simulator. Non-blocking — returns immediately.

    Use wait_for_exit() to wait for the app to finish.
    """
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, f"launch {app_name}", port)
            for line in lines:
                if "Launching" in line:
                    return f"Launching: {line}"
            return f"Sent launch command. Response: {lines}"
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(
                conn.call, "launch_app", {"name": app_name}, timeout=5
            )
            return f"App launch initiated: {result}"
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def wait_for_exit(app_name: str = "", timeout: float = 60.0, device: str | None = None) -> str:
    """Wait for the currently running app to exit. Returns when app exits or timeout."""
    port = resolve_port(device)
    if port:
        return "(wait_for_exit not supported in hardware mode)"
    try:
        conn = get_connection()
        notif = await asyncio.to_thread(conn.wait_for_notification, "app.exited", timeout=timeout)
        if notif:
            params = notif.get("params", {})
            name = params.get("name", "?")
            ok = params.get("ok", False)
            return f"App '{name}' exited (ok={ok})"
        return f"Timeout after {timeout}s — app still running"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def exit_app(device: str | None = None) -> str:
    """Send exit signal to the currently running app."""
    port = resolve_port(device)
    if port:
        try:
            await asyncio.to_thread(do_command_hardware, "exit", port, timeout=2)
            return "Exit signal sent."
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "exit_app", timeout=5)
            return f"Exit signal sent: {result}"
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def keypress(key: str, device: str | None = None) -> str:
    """Inject a keypress on the PicOS simulator.

    Valid keys: up, down, left, right, enter, esc, menu, f1-f10,
    backspace, tab, del, shift, a-z, A-Z, 0-9.
    """
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, f"keypress {key}", port)
            for line in lines:
                if "injected" in line.lower():
                    return f"Key '{key}' injected."
            return "\n".join(lines)
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(
                conn.call, "inject_button", {"button": key, "action": "click"}, timeout=5
            )
            return f"Key '{key}' injected: {result}"
        except JRpcError as e:
            return f"Unknown key '{key}': {e.message}" if e.code == -32602 else str(e)
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def send_command(command: str, timeout: float = 5.0, device: str | None = None) -> str:
    """Send a raw dev command to PicOS and return the response."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, command, port, timeout=timeout)
            return "\n".join(lines) if lines else "(no response)"
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            parts = command.split(maxsplit=1)
            method = parts[0]
            params = {}
            if len(parts) > 1:
                try:
                    params = json.loads(parts[1])
                except json.JSONDecodeError:
                    pass
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, method, params, timeout=timeout)
            return json.dumps(result, indent=2)
        except JRpcError as e:
            return f"[{e.code}] {e.message}"
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def get_status(device: str | None = None) -> str:
    """Get simulator status: running app, heap info, WiFi state."""
    port = resolve_port(device)
    if port:
        return "(status not available in hardware mode)"
    try:
        conn = get_connection()
        app = await asyncio.to_thread(conn.call, "get_running_app", timeout=5)
        heap = await asyncio.to_thread(conn.call, "get_heap_info", timeout=5)
        wifi = await asyncio.to_thread(conn.call, "get_wifi_state", timeout=5)

        app_name = app.get("name") if app else None
        if app_name:
            status = f"Running: {app_name}"
        else:
            status = "Status: Launcher (no app running)"

        return (
            f"{status}\n"
            f"Heap: {heap.get('lua_heap_used_kb', '?')}KB used / "
            f"{heap.get('lua_heap_free_kb', '?')}KB free\n"
            f"PSRAM: {heap.get('psram_total_kb', '?')}KB total\n"
            f"WiFi: {wifi.get('status', '?')} ({wifi.get('ssid', '')}) "
            f"IP: {wifi.get('ip', '?')}"
        )
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_log_buffer(lines: int = 100, device: str | None = None) -> str:
    """Retrieve recent log lines from the simulator."""
    port = resolve_port(device)
    if port:
        return "(log buffer not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_log_buffer", timeout=5)
        log_lines = result.get("lines", [])
        if isinstance(log_lines, str) and log_lines:
            log_lines = log_lines.strip().split("\n")
        if not log_lines:
            return "(no log lines)"
        shown = log_lines[-lines:] if len(log_lines) > lines else log_lines
        return "\n".join(shown)
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def clear_log_buffer(device: str | None = None) -> str:
    """Clear the simulator log buffer."""
    port = resolve_port(device)
    if port:
        return "(not available in hardware mode)"
    try:
        conn = get_connection()
        await asyncio.to_thread(conn.call, "clear_log_buffer", timeout=5)
        return "Log buffer cleared."
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def set_wifi_state(
    status: str = "connected",
    error_code: int = 0,
    error_str: str = "",
    device: str | None = None,
) -> str:
    """Set WiFi state in the simulator (for testing error paths)."""
    port = resolve_port(device)
    if port:
        return "(WiFi control not available in hardware mode)"
    try:
        params = {"status": status}
        if error_code:
            params["error_code"] = str(error_code)
        if error_str:
            params["error_str"] = error_str
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "set_wifi_state", params, timeout=5)
        return f"WiFi state set: {result}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def set_time_multiplier(multiplier: float = 1.0, device: str | None = None) -> str:
    """Set time multiplier in simulator (0=pause, 1=realtime, >1=fast-forward)."""
    port = resolve_port(device)
    if port:
        return "(Time control not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(
            conn.call, "set_time_multiplier", {"multiplier": multiplier}, timeout=5
        )
        return f"Time multiplier set: {result}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_terminal_buffer(device: str | None = None) -> str:
    """Get the active terminal's text buffer (simulator only).

    Returns JSON with cols, rows, cursor position, and text lines.
    Only works when a Lua app has created a picocalc.terminal instance.
    """
    port = resolve_port(device)
    if port:
        return "(terminal buffer not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_terminal_buffer", timeout=5)
        return json.dumps(result, indent=2)
    except JRpcError as e:
        return f"[{e.code}] {e.message}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_heap_info(device: str | None = None) -> str:
    """Get memory usage: Lua heap and PSRAM."""
    port = resolve_port(device)
    if port:
        return "(heap info not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_heap_info", timeout=5)
        return (
            f"Lua heap: {result.get('lua_heap_used_kb', '?')}KB used, "
            f"{result.get('lua_heap_free_kb', '?')}KB free\n"
            f"PSRAM total: {result.get('psram_total_kb', '?')}KB"
        )
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_crash_log(device: str | None = None) -> str:
    """Get the simulator crash log (if any). Useful after a simulator crash."""
    port = resolve_port(device)
    if port:
        return "(crash log not available in hardware mode)"

    # Try reading via RPC first
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_crash_log", timeout=5)
        crash = result.get("crash_log")
        if crash:
            return f"Crash log:\n{crash}"
        return "No crash log found."
    except Exception:
        pass

    # If simulator is unreachable, try reading the file directly
    crash_log_path = Path("/tmp/picos_sim_crash.log")
    if crash_log_path.exists():
        try:
            return f"Crash log (from file):\n{crash_log_path.read_text()}"
        except Exception as e:
            return f"Error reading crash log: {e}"
    return "No crash log found and simulator is unreachable."


@mcp.tool()
async def shutdown_simulator(device: str | None = None) -> str:
    """Gracefully shut down the PicOS simulator."""
    port = resolve_port(device)
    if port:
        return "(shutdown not available in hardware mode)"
    try:
        conn = get_connection()
        await asyncio.to_thread(conn.call, "shutdown", timeout=3)
        return "Shutdown signal sent."
    except Exception as e:
        return f"Error (simulator may already be stopped): {e}"


@mcp.tool()
async def start_simulator(
    port: int = 0,
    sd_card_path: str = "",
    headless: bool = True,
) -> str:
    """Build (if needed) and start a PicOS simulator instance.

    Args:
        port: TCP port (0=auto-assign)
        sd_card_path: Path to SD card directory (empty=default)
        headless: Run without display (for CI/testing)

    Returns the TCP port of the started simulator.
    """
    global _sim_manager, _configured_port, _conn
    if _sim_manager is None:
        _sim_manager = SimulatorManager()
    try:
        actual_port = await asyncio.to_thread(
            _sim_manager.ensure_running,
            port=port,
            sd_card_path=sd_card_path or None,
            headless=headless,
        )
        # Reset connection to use new port (atomic update under lock)
        with _conn_lock:
            _configured_port = actual_port
            if _conn:
                try:
                    _conn.close()
                except Exception:
                    pass
                _conn = None
        return f"Simulator running on port {actual_port}"
    except Exception as e:
        return f"Error starting simulator: {e}"


# ── Hardware-only Tools ────────────────────────────────────────────────────────


@mcp.tool()
async def reboot(mode: str = "normal", device: str | None = None) -> str:
    """Reboot the PicOS hardware device (not available in simulator)."""
    if not HARDWARE_MODE:
        return "Reboot is only available in hardware mode. Start MCP with --hardware flag."
    if mode not in ("normal", "flash"):
        return f"Unknown mode '{mode}'. Use 'normal' or 'flash'."
    try:
        port = resolve_port(device)
        assert port is not None
        cmd = "reboot-flash" if mode == "flash" else "reboot"
        ser = open_serial(port, timeout=1)
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        time.sleep(0.1)
        ser.close()
        if mode == "flash":
            return "Reboot-to-flash sent. Device will appear as USB drive."
        return "Reboot sent."
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def flash(file: str, device: str | None = None) -> str:
    """Flash a UF2 file to the PicOS hardware device (not available in simulator)."""
    if not HARDWARE_MODE:
        return "Flash is only available in hardware mode."
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    return "Flash not yet implemented in this version."


@mcp.tool()
async def put_file(local_path: str, remote_path: str, device: str | None = None) -> str:
    """Upload a file to the PicOS SD card (simulator: copies to host filesystem)."""
    if not HARDWARE_MODE:
        try:
            sim_apps = os.environ.get("PICOS_SIMULATOR_SD", "./assets/sd_card")
            dest = Path(sim_apps) / remote_path.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(local_path, dest)
            return f"Copied to simulator: {dest}"
        except Exception as e:
            return f"Error: {e}"
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    return "Hardware put_file not yet implemented."


@mcp.tool()
async def get_file(remote_path: str, local_path: str, device: str | None = None) -> str:
    """Download a file from the PicOS SD card (simulator: copies from host filesystem)."""
    if not HARDWARE_MODE:
        try:
            sim_apps = os.environ.get("PICOS_SIMULATOR_SD", "./assets/sd_card")
            src = Path(sim_apps) / remote_path.lstrip("/")
            shutil.copy(src, local_path)
            return f"Copied from simulator: {src} -> {local_path}"
        except Exception as e:
            return f"Error: {e}"
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    return "Hardware get_file not yet implemented."


# ── Main Entry Point ──────────────────────────────────────────────────────────


def main():
    global HARDWARE_MODE, _configured_port

    parser = argparse.ArgumentParser(
        description="PicOS MCP Server - Control PicOS simulator (default) or hardware device"
    )
    parser.add_argument("--hardware", action="store_true", help="Use hardware device via USB serial")
    parser.add_argument("--simulator", action="store_true", help="Use simulator (default)")
    parser.add_argument("--port", type=int, default=DEFAULT_TCP_PORT, help="Simulator TCP port (default: 7878)")
    parser.add_argument("--device", help="Device endpoint (serial port or socket path)")
    parser.add_argument(
        "--transport", choices=["stdio", "sse"], default="stdio",
        help="MCP transport type (default: stdio)"
    )
    args = parser.parse_args()

    if args.hardware:
        HARDWARE_MODE = True
        if not HAS_SERIAL:
            print("Warning: pyserial not installed. Hardware mode requires: pip install pyserial", file=sys.stderr)

    _configured_port = args.port

    mode_str = "hardware" if HARDWARE_MODE else "simulator"
    print(f"PicOS MCP Server starting in {mode_str} mode (port {_configured_port})...", file=sys.stderr)
    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
