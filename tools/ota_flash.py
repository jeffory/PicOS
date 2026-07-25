#!/usr/bin/env python3
"""PicOS OTA firmware flasher — stage, reboot, verify.

Pushes a raw firmware image to a USB-connected device over the SD-staged
OTA path: uploads /system/update.sha256 and /system/update.bin via the
firmware's putb64 serial command, then reboots.  On boot the firmware
verifies the hash, pre-reads the image into PSRAM, reflashes itself and
resets (see src/os/ota_update.c).  On success the device renames the
staged files to *.flashed, so a later boot does not re-apply them.

Reuses the serial transfer helpers from picos_mcp.py (same directory) so
chunk/ACK pacing, integrity checks and transfer retries live in one place.

Usage:
    python3 tools/ota_flash.py [build/picocalc_os.bin] [--device /dev/ttyACM0]

Notes:
  - Hardware only.  The simulator has no flash; this tool never falls back
    to the simulator transport.
  - The device must be at the launcher: reboot is ignored while an app is
    running, so a running app is asked to exit first.
  - Do not run this while another process (for example the picos MCP server
    with an active hardware log monitor) is reading the same serial port.
  - Recovery if an update bricks the device: hold BOOTSEL, connect USB and
    copy build/picocalc_os.uf2 to the mounted drive.
"""

import argparse
import hashlib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import picos_mcp as pm  # noqa: E402

OTA_MAX_SIZE = 2 * 1024 * 1024  # must match OTA_MAX_SIZE in ota_update.c
OTA_MIN_SIZE = 256


def fail(msg: str) -> "NoReturn":  # noqa: F821
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def get_ver(port: str) -> str | None:
    """Return the device's 'PicOS build <date> <time>' string, or None."""
    try:
        lines = pm.do_command_hardware("ver", port, timeout=2.5)
    except Exception:
        return None
    for line in lines:
        if "PicOS build" in line:
            return line.split("[DEV] ", 1)[-1]
    return None


def get_running_app(port: str) -> str | None:
    """Return the running app name, 'launcher', or None if status failed."""
    try:
        lines = pm.do_command_hardware("status", port, timeout=3)
    except Exception:
        return None
    for line in lines:
        m = re.search(r"\[DEV\] Status: app=(.*?) app_uptime_ms=", line)
        if m:
            return m.group(1)
    return None


def ensure_launcher(port: str) -> None:
    """Reboot is ignored while an app runs — exit to the launcher first."""
    app = get_running_app(port)
    if app is None:
        print("  (firmware predates the `status` command — assuming launcher)")
        return
    if app == "launcher":
        return
    print(f"  App '{app}' is running — sending exit...")
    pm.do_command_hardware("exit", port, timeout=2)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        time.sleep(1.0)
        if get_running_app(port) == "launcher":
            return
    fail(f"app '{app}' did not exit within 15s; exit it on-device and retry")


def wait_for_reflash(old_port: str, old_ver: str | None, timeout: float) -> None:
    """Wait for reboot, OTA apply and the second reboot into new firmware."""
    # Phase 1: the serial port disappears when the device reboots.
    print("  Waiting for device to reboot...")
    vanish_deadline = time.monotonic() + 30
    vanished = False
    while time.monotonic() < vanish_deadline:
        if not os.path.exists(old_port):
            vanished = True
            break
        time.sleep(0.2)
    if not vanished:
        print("  WARNING: serial port never dropped — reboot may have been "
              "ignored. Continuing to poll anyway.")

    # Phase 2: the flash write runs with USB dead; the port returns (possibly
    # renumbered) once the new firmware boots to the launcher.
    print("  Waiting for OTA apply + reboot (watch the device screen)...")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(2.0)
        port = pm.find_usb_device()
        if not port:
            continue
        ver = get_ver(port)
        if not ver:
            continue
        # Success signal: the staged image was consumed (renamed to
        # *.flashed).  If update.bin is still present the device booted
        # WITHOUT applying it (hash mismatch or validation failure).
        try:
            pm.do_get_file_b64(port, "/system/update.bin")
            fail("device rebooted but /system/update.bin was not consumed — "
                 "the update was rejected (check the device screen/serial "
                 "log for the OTA error)")
        except FileNotFoundError:
            pass  # consumed — flashed OK
        except Exception as e:
            print(f"  (could not confirm staging cleanup: {e})")
        print(f"\n✓ Device is back on {port}")
        if old_ver:
            print(f"  before: {old_ver}")
        print(f"  after:  {ver}")
        if old_ver and old_ver == ver:
            print("  NOTE: identical build timestamp — the version string "
                  "comes from main.c, which only rebuilds when touched.")
        return
    fail(f"device did not come back within {timeout:.0f}s — check its "
         "screen; do NOT power off while the progress bar is moving")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Flash PicOS firmware to a USB device via the OTA path")
    ap.add_argument("firmware", nargs="?", default="build/picocalc_os.bin",
                    help="raw firmware image (default: build/picocalc_os.bin)")
    ap.add_argument("--device", help="serial port (default: auto-detect)")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="seconds to wait for reflash + reboot (default 240)")
    ap.add_argument("--no-verify", action="store_true",
                    help="stage and reboot, but do not wait for the device")
    args = ap.parse_args()

    fw = Path(args.firmware)
    if not fw.is_file():
        fail(f"{fw} not found — run 'make build' first")
    data = fw.read_bytes()
    if data[:4] in (b"UF2\n", b"UF2\x0a"):
        fail("that is a UF2 file — the OTA path needs the raw .bin "
             "(build/picocalc_os.bin)")
    if len(data) < OTA_MIN_SIZE:
        fail(f"{fw} is too small ({len(data)} bytes) to be firmware")
    if len(data) > OTA_MAX_SIZE:
        fail(f"{fw} is {len(data)} bytes — exceeds the {OTA_MAX_SIZE} byte "
             "OTA limit in ota_update.c")

    port = args.device or pm.find_usb_device()
    if not port:
        fail("no PicOS device detected on USB (OTA flash targets hardware "
             "only); pass --device if detection failed")

    print(f"Device:   {port}")
    print(f"Firmware: {fw} ({len(data) / 1024:.0f} KB)")

    ensure_launcher(port)
    old_ver = get_ver(port)
    if old_ver:
        print(f"Current:  {old_ver}")

    sha = hashlib.sha256(data).hexdigest()
    print(f"SHA-256:  {sha[:16]}…")
    pm.do_put_file_b64(port, (sha + "\n").encode(), "/system/update.sha256")

    print(f"Uploading {len(data) / 1024:.0f} KB to /system/update.bin...")
    start = time.monotonic()

    def progress(sent: int, total: int) -> None:
        pct = sent * 100 // total
        print(f"\r  {pct:3d}%  ({sent // 1024}K / {total // 1024}K)",
              end="", flush=True)

    pm.do_put_file_b64(port, data, "/system/update.bin", progress=progress)
    print(f"\n  Uploaded in {time.monotonic() - start:.0f}s (fnv1a verified)")

    print("Rebooting — the device verifies the SHA-256 and reflashes on "
          "boot. DO NOT power off.")
    pm.do_command_hardware("reboot", port, timeout=2)

    if args.no_verify:
        print("Staged and rebooted (--no-verify: not waiting for the device).")
        return
    wait_for_reflash(port, old_ver, args.timeout)


if __name__ == "__main__":
    main()
