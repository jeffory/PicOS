#!/usr/bin/env python3
"""PicOS MCP Server — exposes dev commands to coding agents.

Wraps USB serial communication with PicOS devices as MCP tools:
  ping, screenshot, list_apps, launch_app, exit_app, reboot, send_command

Usage:
    Registered in .claude/settings.local.json as an MCP server.
    Can also be run standalone: python3 tools/picos_mcp.py
"""

import asyncio
import base64
import glob
import io
import platform
import shutil
import struct
import time
from pathlib import Path

try:
    import serial
except ImportError:
    raise ImportError("pyserial required: pip install pyserial")

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("picos")

# ── Serial helpers ────────────────────────────────────────────────────────────

SCRN_MAGIC = b"SCRN"
SCRN_HEADER_SIZE = 12
DEFAULT_TIMEOUT = 5


def find_device():
    """Auto-detect PicOS serial port."""
    for pattern in ["/dev/ttyACM*", "/dev/tty.usbmodem*", "COM*"]:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_serial(port, timeout=DEFAULT_TIMEOUT):
    """Open serial port and drain pending output."""
    ser = serial.Serial(port, baudrate=115200, timeout=timeout)
    ser.reset_input_buffer()
    time.sleep(0.05)
    ser.reset_input_buffer()
    return ser


def do_command(cmd, port, timeout=DEFAULT_TIMEOUT):
    """Send a text command, return all response lines."""
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
            # Stop reading once we've consumed the response
            if line.startswith("[DEV] ") and len(lines) > 1:
                # Check if this is a terminal line (pong, Total:, Error:, etc.)
                if any(kw in line for kw in ["pong", "Total:", "Error:", "Launching", "Rebooting", "BOOTSEL", "Unknown"]):
                    break
        return lines
    finally:
        ser.close()


def do_screenshot(port, timeout=DEFAULT_TIMEOUT):
    """Send screenshot command, return PNG bytes."""
    ser = open_serial(port, timeout)
    try:
        ser.write(b"screenshot\n")
        ser.flush()

        # Read until SCRN magic (skip printf chatter)
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

        # Read rest of header
        while len(buf) < SCRN_HEADER_SIZE:
            chunk = ser.read(SCRN_HEADER_SIZE - len(buf))
            if not chunk:
                raise TimeoutError("Timed out reading header")
            buf += chunk

        width, height, fmt = struct.unpack_from("<HHH", buf, 4)
        pixel_bytes = width * height * 2

        # Read pixel data
        data = buf[SCRN_HEADER_SIZE:]
        while len(data) < pixel_bytes:
            remaining = pixel_bytes - len(data)
            chunk = ser.read(min(remaining, 4096))
            if not chunk:
                raise TimeoutError(
                    f"Timed out reading pixels ({len(data)}/{pixel_bytes})"
                )
            data += chunk

        return rgb565be_to_png(data[:pixel_bytes], width, height)
    finally:
        ser.close()


def rgb565be_to_png(data, width, height):
    """Convert byte-swapped RGB565 framebuffer to PNG bytes in memory."""
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


def resolve_device(device=None):
    """Resolve device path, raising a helpful error if not found."""
    port = device or find_device()
    if not port:
        raise RuntimeError(
            "No PicOS device found. Connect via USB and specify device= if needed."
        )
    return port


# ── MCP Tools ─────────────────────────────────────────────────────────────────


@mcp.tool()
async def ping(device: str | None = None) -> str:
    """Ping the PicOS device to check connectivity. Returns 'pong' on success."""
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(do_command, "ping", port, DEFAULT_TIMEOUT)
        for line in lines:
            if "pong" in line:
                return "pong"
        return f"No pong response. Got: {lines}"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def screenshot(device: str | None = None) -> list:
    """Take a screenshot of the PicOS display. Returns the screen as a PNG image."""
    port = resolve_device(device)
    try:
        png_bytes = await asyncio.to_thread(do_screenshot, port, DEFAULT_TIMEOUT)
        png_b64 = base64.standard_b64encode(png_bytes).decode("ascii")
        return [
            {"type": "image", "data": png_b64, "mimeType": "image/png"},
            {"type": "text", "text": f"Screenshot captured from {port} (320x320 PNG)"},
        ]
    except serial.SerialException as e:
        return [{"type": "text", "text": f"Serial error: {e}. Is another program using {port}?"}]
    except ImportError:
        return [{"type": "text", "text": "Pillow not installed. Run: pip install Pillow"}]
    except Exception as e:
        return [{"type": "text", "text": f"Error: {e}"}]


@mcp.tool()
async def list_apps(device: str | None = None) -> str:
    """List all apps installed on the PicOS device's SD card."""
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(do_command, "list", port, DEFAULT_TIMEOUT)
        apps = []
        in_list = False
        for line in lines:
            if "[DEV] Available apps:" in line:
                in_list = True
                continue
            if "[DEV] Total:" in line:
                apps.append(line.replace("[DEV] ", ""))
                break
            if in_list and line.startswith("  "):
                apps.append(line.strip())
        if apps:
            return "\n".join(apps)
        return f"No apps found. Raw response: {lines}"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def launch_app(app_name: str, device: str | None = None) -> str:
    """Launch an app by name on the PicOS device. Blocks until the app exits."""
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(
            do_command, f"launch {app_name}", port, DEFAULT_TIMEOUT
        )
        for line in lines:
            if "Launching app:" in line:
                launched_name = line.split("Launching app:")[1].strip()
                return f"Launched '{launched_name}'. The app is now running."
            if "Error:" in line:
                return line.replace("[DEV] ", "")
        return f"WARNING: No launch confirmation received. Response: {lines}"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def exit_app(device: str | None = None) -> str:
    """Send exit signal to the currently running app on PicOS."""
    port = resolve_device(device)
    try:
        await asyncio.to_thread(do_command, "exit", port, 1)
        return "Exit signal sent. App will return to launcher."
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def reboot(mode: str = "normal", device: str | None = None) -> str:
    """Reboot the PicOS device.

    Args:
        mode: "normal" (default) restarts firmware, "flash" enters BOOTSEL/UF2 mode for flashing.
        device: Serial port path, or auto-detect if omitted.
    """
    if mode not in ("normal", "flash"):
        return f"Unknown mode '{mode}'. Use 'normal' or 'flash'."
    port = resolve_device(device)
    cmd = "reboot-flash" if mode == "flash" else "reboot"
    try:
        # Fire and forget — device disconnects immediately
        ser = open_serial(port, timeout=1)
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        time.sleep(0.1)
        ser.close()
        if mode == "flash":
            return "Reboot-to-flash command sent. Device will appear as a USB drive (RPI-RP2) for UF2 flashing."
        return "Reboot command sent. Device will disconnect and reconnect."
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


def find_mount_dir(timeout: float = 30.0) -> str | None:
    """Find the RP2350 USB mass storage mount point."""
    poll_interval = 0.5
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        sys = platform.system()
        if sys == "Linux":
            # Try multiple common mount locations and labels
            for base in ["/run/media", "/media", "/mnt"]:
                if not Path(base).exists():
                    continue
                try:
                    users = glob.glob(f"{base}/*")
                    for user in users:
                        for label in ["RP2350", "RPI-RP2"]:
                            path = f"{user}/{label}"
                            if Path(path).is_dir():
                                return path
                except PermissionError:
                    continue
        elif sys == "Darwin":
            for name in ["RPI-RP2", "RP2350"]:
                path = f"/Volumes/{name}"
                if Path(path).is_dir():
                    return path

        time.sleep(poll_interval)

    return None


def do_flash(file: str, port: str) -> str:
    """Reboot to flash mode and copy a file to the device."""
    local_path = Path(file)
    if not local_path.exists():
        return f"File not found: {file}"

    filename = local_path.name

    # Check if already in flash mode (mounted as USB drive)
    mount_dir = find_mount_dir(timeout=2.0)

    if not mount_dir:
        # Not in flash mode - reboot to flash mode first
        try:
            ser = open_serial(port, timeout=1)
            ser.write(b"reboot-flash\n")
            ser.flush()
            time.sleep(0.1)
            ser.close()
        except Exception as e:
            return f"Failed to send reboot command: {e}"

        # Wait for mount
        mount_dir = find_mount_dir(timeout=30.0)
        if not mount_dir:
            return "Device did not mount as USB drive. Is the BOOTSEL button held?"

    # Verify mount is ready (some systems need a moment)
    for _ in range(5):
        try:
            test_file = Path(mount_dir) / ".write_test"
            test_file.write_text("")
            test_file.unlink()
            break
        except (PermissionError, FileNotFoundError, OSError):
            time.sleep(0.2)
    else:
        return f"Mount point {mount_dir} is not writable. Try mounting manually."

    # Copy file
    try:
        dest_path = Path(mount_dir) / filename
        shutil.copy(local_path, dest_path)
        return f"Flashed {filename} to device. Safely remove the device before reconnecting."
    except Exception as e:
        return f"Failed to copy file: {e}"


@mcp.tool()
async def flash(file: str, device: str | None = None) -> str:
    """Flash a UF2 file to the PicOS device.

    Reboots the device into BOOTSEL mode, waits for it to appear as a USB
    drive, and copies the specified file to it.

    Args:
        file: Path to the UF2 file to flash.
        device: Serial port path, or auto-detect if omitted.
    """
    port = resolve_device(device)
    try:
        return await asyncio.to_thread(do_flash, file, port)
    except Exception as e:
        return f"Error: {e}"


VALID_KEYS = (
    "up, down, left, right, enter, esc, menu, f1-f10, backspace, tab, del, shift, "
    "a-z, A-Z, 0-9, and other printable ASCII"
)


@mcp.tool()
async def keypress(key: str, device: str | None = None) -> str:
    """Inject a keypress on the PicOS device.

    Args:
        key: Key name or character to inject.
             Valid keys: up, down, left, right, enter, esc, menu, f1-f10,
             backspace, tab, del, shift, a-z, A-Z, 0-9, punctuation.
        device: Serial port path, or auto-detect if omitted.
    """
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(do_command, f"keypress {key}", port, DEFAULT_TIMEOUT)
        for line in lines:
            if "Key injected:" in line:
                return f"Key '{key}' injected successfully."
            if "Unknown key:" in line:
                return f"Unknown key: '{key}'. Valid keys: {VALID_KEYS}"
        return "\n".join(lines) if lines else "(no response)"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def help(device: str | None = None) -> str:
    """Show available PicOS dev commands and valid key names."""
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(do_command, "help", port, DEFAULT_TIMEOUT)
        return "\n".join(lines) if lines else "(no response)"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


def do_put_file(local_path: str, remote_path: str, port: str, timeout: int = 30) -> str:
    """Send a file to the PicOS device via put command."""
    try:
        with open(local_path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return f"File not found: {local_path}"
    except IOError as e:
        return f"Error reading file: {e}"

    file_size = len(data)
    ser = open_serial(port, timeout)
    try:
        ser.write(f"put {remote_path} {file_size}\n".encode())
        ser.flush()

        lines = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            if "Ready to receive" in line:
                break

        if not any("Ready to receive" in l for l in lines):
            return f"Device not ready. Response: {lines}"

        sent = 0
        while sent < file_size:
            chunk = data[sent:sent + 4096]
            ser.write(chunk)
            ser.flush()
            sent += len(chunk)

        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if "File received:" in line:
                return f"File uploaded: {remote_path} ({file_size} bytes)"
            if "Error" in line:
                return f"Upload failed: {line}"
        return f"Upload timed out. Sent {sent}/{file_size} bytes."
    finally:
        ser.close()


def do_get_file(remote_path: str, local_path: str, port: str, timeout: int = 30) -> str:
    """Receive a file from the PicOS device via get command."""
    ser = open_serial(port, timeout)
    try:
        ser.write(f"get {remote_path}\n".encode())
        ser.flush()

        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            avail = ser.in_waiting
            if avail > 0:
                chunk = ser.read(avail)
                buf += chunk
                if b"FILE_DATA:" in buf:
                    break
            else:
                time.sleep(0.01)

        header_pos = buf.find(b"FILE_DATA:")
        if header_pos < 0:
            return f"Invalid response. Got: {buf[:200]}"

        header = buf[:header_pos].decode("utf-8", errors="replace")
        size = 0
        for line in header.split("\n"):
            if line.startswith("SIZE:"):
                try:
                    size = int(line.split(":")[1])
                except (ValueError, IndexError):
                    pass

        data_start = header_pos + len(b"FILE_DATA:")
        data = buf[data_start:]

        while len(data) < size and time.monotonic() < deadline:
            avail = ser.in_waiting
            if avail > 0:
                chunk = ser.read(min(avail, size - len(data)))
                data += chunk
            else:
                time.sleep(0.01)

        with open(local_path, "wb") as f:
            f.write(data[:size])

        return f"File downloaded: {local_path} ({size} bytes)"
    except FileNotFoundError:
        return f"Cannot write to: {local_path}"
    except IOError as e:
        return f"Error writing file: {e}"
    finally:
        ser.close()


def do_list_dir(path: str, port: str, timeout: int = 10) -> str:
    """List directory contents on the PicOS device."""
    ser = open_serial(port, timeout)
    try:
        ser.write(f"ls {path}\n".encode())
        ser.flush()

        lines = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            if line.startswith("[DEV] ") and "items in" in line:
                break

        output = []
        for line in lines:
            if line.startswith("[DEV] "):
                continue
            if line:
                output.append(line)
        return "\n".join(output) if output else "(empty directory)"
    finally:
        ser.close()


@mcp.tool()
async def put_file(local_path: str, remote_path: str, device: str | None = None) -> str:
    """Upload a file to the PicOS SD card.

    Args:
        local_path: Path to the local file to upload.
        remote_path: Destination path on the SD card (e.g., '/apps/myapp/main.lua').
        device: Serial port path, or auto-detect if omitted.
    """
    port = resolve_device(device)
    try:
        return await asyncio.to_thread(do_put_file, local_path, remote_path, port)
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_file(remote_path: str, local_path: str, device: str | None = None) -> str:
    """Download a file from the PicOS SD card.

    Args:
        remote_path: Path to the file on the SD card (e.g., '/apps/myapp/main.lua').
        local_path: Destination path for the downloaded file.
        device: Serial port path, or auto-detect if omitted.
    """
    port = resolve_device(device)
    try:
        return await asyncio.to_thread(do_get_file, remote_path, local_path, port)
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def list_dir(path: str, device: str | None = None) -> str:
    """List directory contents on the PicOS SD card.

    Args:
        path: Directory path to list (e.g., '/apps', '/data').
        device: Serial port path, or auto-detect if omitted.
    """
    port = resolve_device(device)
    try:
        return await asyncio.to_thread(do_list_dir, path, port)
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def send_command(
    command: str, timeout: float = 5.0, device: str | None = None
) -> str:
    """Send a raw dev command to PicOS and return the response."""
    port = resolve_device(device)
    try:
        lines = await asyncio.to_thread(do_command, command, port, timeout)
        return "\n".join(lines) if lines else "(no response)"
    except serial.SerialException as e:
        return f"Serial error: {e}. Is another program using {port}?"
    except Exception as e:
        return f"Error: {e}"


if __name__ == "__main__":
    mcp.run()
