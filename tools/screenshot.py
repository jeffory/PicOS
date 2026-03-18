#!/usr/bin/env python3
"""Capture a screenshot from a PicOS device over USB serial.

Usage:
    python3 tools/screenshot.py                     # auto-detect, save screenshot.png
    python3 tools/screenshot.py -o screen.png       # save to specific file
    python3 tools/screenshot.py -d /dev/ttyACM1     # specify serial device
    python3 tools/screenshot.py --raw frame.bin     # save raw RGB565 (no Pillow needed)
    python3 tools/screenshot.py ping                # test serial connectivity

Requires: pyserial (pip install pyserial)
Optional: Pillow  (pip install Pillow) — for PNG output
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


MAGIC = b"SCRN"
HEADER_SIZE = 12
TIMEOUT = 5  # seconds


def find_device():
    """Try common PicOS serial device paths."""
    import glob
    for pattern in ["/dev/ttyACM*", "/dev/tty.usbmodem*", "COM*"]:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_serial(port, timeout=TIMEOUT):
    """Open serial port and drain any pending output."""
    ser = serial.Serial(port, baudrate=115200, timeout=timeout)
    ser.reset_input_buffer()
    time.sleep(0.05)
    ser.reset_input_buffer()
    return ser


def ping(port, timeout=TIMEOUT):
    """Send ping command and check for pong response."""
    ser = open_serial(port, timeout)
    ser.write(b"ping\n")
    ser.flush()

    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if not chunk:
            continue
        buf += chunk
        # Look for the pong response from printf("[DEV] pong\n")
        if b"pong" in buf:
            ser.close()
            return True
    ser.close()
    return False


def capture(port, timeout=TIMEOUT):
    """Send screenshot command and receive framebuffer data.

    Returns (width, height, format, pixel_data) or raises on error.
    """
    ser = open_serial(port, timeout)

    # Send command
    ser.write(b"screenshot\n")
    ser.flush()

    # Read until we find the SCRN magic (skip any printf output before it)
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if not chunk:
            continue
        buf += chunk
        idx = buf.find(MAGIC)
        if idx >= 0:
            buf = buf[idx:]  # discard anything before magic
            break
    else:
        ser.close()
        raise TimeoutError("Timed out waiting for SCRN header")

    # Read rest of header
    while len(buf) < HEADER_SIZE:
        remaining = HEADER_SIZE - len(buf)
        chunk = ser.read(remaining)
        if not chunk:
            raise TimeoutError("Timed out reading header")
        buf += chunk

    width, height, fmt = struct.unpack_from("<HHH", buf, 4)
    pixel_bytes = width * height * 2

    # Read pixel data
    data = buf[HEADER_SIZE:]
    while len(data) < pixel_bytes:
        remaining = pixel_bytes - len(data)
        chunk = ser.read(min(remaining, 4096))
        if not chunk:
            raise TimeoutError(
                f"Timed out reading pixels ({len(data)}/{pixel_bytes} bytes)"
            )
        data += chunk

    ser.close()
    return width, height, fmt, data[:pixel_bytes]


def rgb565be_to_rgb(data, width, height):
    """Convert byte-swapped (big-endian) RGB565 framebuffer to RGB888 bytes."""
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        # Framebuffer stores big-endian RGB565 (byte-swapped for DMA)
        hi = data[i * 2]
        lo = data[i * 2 + 1]
        pixel = (hi << 8) | lo
        r = ((pixel >> 11) & 0x1F) * 255 // 31
        g = ((pixel >> 5) & 0x3F) * 255 // 63
        b = (pixel & 0x1F) * 255 // 31
        pixels[i * 3] = r
        pixels[i * 3 + 1] = g
        pixels[i * 3 + 2] = b
    return bytes(pixels)


def save_png(width, height, rgb_data, path):
    """Save RGB888 data as PNG using Pillow."""
    try:
        from PIL import Image
    except ImportError:
        print(
            "Error: Pillow not installed. Run: pip install Pillow\n"
            "       Or use --raw to save raw RGB565 data.",
            file=sys.stderr,
        )
        sys.exit(1)

    img = Image.frombytes("RGB", (width, height), rgb_data)
    img.save(path)


def main():
    parser = argparse.ArgumentParser(description="Capture PicOS screenshot")
    parser.add_argument(
        "command", nargs="?", default=None,
        help="'ping' to test connectivity (omit for screenshot)"
    )
    parser.add_argument(
        "-d", "--device", default=None, help="Serial device (auto-detect if omitted)"
    )
    parser.add_argument(
        "-o", "--output", default="screenshot.png", help="Output file (default: screenshot.png)"
    )
    parser.add_argument(
        "--raw", action="store_true", help="Save raw RGB565 binary instead of PNG"
    )
    parser.add_argument(
        "-t", "--timeout", type=float, default=TIMEOUT, help="Timeout in seconds"
    )
    args = parser.parse_args()

    device = args.device or find_device()
    if not device:
        print("Error: No serial device found. Specify with -d.", file=sys.stderr)
        sys.exit(1)

    print(f"Connecting to {device}...", file=sys.stderr)

    if args.command == "ping":
        try:
            ok = ping(device, timeout=args.timeout)
        except serial.SerialException as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
        if ok:
            print("pong", file=sys.stderr)
            sys.exit(0)
        else:
            print("Error: No response from device", file=sys.stderr)
            sys.exit(1)

    if args.command and args.command != "screenshot":
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1)

    try:
        width, height, fmt, data = capture(device, timeout=args.timeout)
    except (TimeoutError, serial.SerialException) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"Received {width}x{height} RGB{fmt} ({len(data)} bytes)", file=sys.stderr)

    if args.raw:
        with open(args.output, "wb") as f:
            f.write(data)
    else:
        rgb = rgb565be_to_rgb(data, width, height)
        save_png(width, height, rgb, args.output)

    print(f"Saved to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
