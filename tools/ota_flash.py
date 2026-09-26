#!/usr/bin/env python3
"""PicoDeck OTA firmware flasher — stage, reboot, verify.

Pushes a raw firmware image to a USB-connected device over the SD-staged
OTA path: signs it (ECDSA P-256, tools/sign_update.py — the TEST key by
default, which dev firmware trusts; --key for another), uploads
/system/update.sha256, /system/update.sig and /system/update.bin via the
firmware's putb64 serial command, then sends `reboot-ota`, which validates
the staged image (checksum + signature) and sets the one-shot OTA token
before rebooting.  On boot the firmware (token present) pre-reads the image
into PSRAM, re-checks the checksum and signature over those bytes, reflashes
itself and resets (see src/os/ota_update.c).  On success the device renames
the staged files to *.flashed; a refused image is renamed to *.rejected and
logged to /system/error.log.  A staged image WITHOUT the token is never
flashed: the boot renames it to *.stale.  Firmware older than `reboot-ota`
answers "Unknown command"; the tool then falls back to a plain `reboot`
(those builds flash any staged image at boot and ignore the .sig).

Reuses the serial transfer helpers from picodeck_mcp.py (same directory) so
chunk/ACK pacing, integrity checks and transfer retries live in one place.

Usage:
    python3 tools/ota_flash.py [build/picodeck.bin] [--device /dev/ttyACM0]
                               [--key tests/keys/picodeck-update-TEST-private.pem]

Notes:
  - Hardware only.  The simulator has no flash; this tool never falls back
    to the simulator transport.
  - The device must be at the launcher: `reboot-ota` is dropped while an app
    is running, so a running app is asked to exit first.
  - Do not run this while another process (for example the picodeck MCP server
    with an active hardware log monitor) is reading the same serial port.
  - Recovery if an update bricks the device: hold BOOTSEL, connect USB and
    copy build/picodeck.uf2 to the mounted drive.
"""

import argparse
import hashlib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import picodeck_mcp as pm  # noqa: E402
import sign_update  # noqa: E402

OTA_MAX_SIZE = 2 * 1024 * 1024  # must match OTA_MAX_SIZE in ota_verify.h
OTA_MIN_SIZE = 256


def fail(msg: str) -> "NoReturn":  # noqa: F821
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def get_ver(port: str) -> str | None:
    """Return the device's 'PicoDeck build <date> <time>' string, or None."""
    try:
        lines = pm.do_command_hardware("ver", port, timeout=2.5)
    except Exception:
        return None
    for line in lines:
        if "PicoDeck build" in line:
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
    """Exit to the launcher first: the final `reboot-ota` is dropped while an
    app runs (plain `reboot`/`reboot-flash` are honoured mid-app, but without
    the OTA token the boot renames the image to update.bin.stale)."""
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


def request_ota_reboot(port: str) -> None:
    """Send `reboot-ota` (sets the OTA token, then reboots).  Falls back to
    `reboot` on firmware that predates the command."""
    lines: list[str] = []
    try:
        ser = pm.open_serial(port, 1)
        try:
            ser.write(b"reboot-ota\n")
            ser.flush()
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                lines.append(line)
                if "Unknown command" in line or "reboot-ota failed" in line \
                        or "reboot-ota ignored" in line \
                        or "Triggering update" in line:
                    break
        finally:
            ser.close()
    except Exception:
        pass  # the port vanishes as the device reboots
    text = "\n".join(lines)
    if "Unknown command" in text:
        print("  (firmware predates reboot-ota — using plain reboot)")
        pm.do_command_hardware("reboot", port, timeout=2)
    elif "reboot-ota failed" in text or "reboot-ota ignored" in text:
        fail("device refused the staged update:\n" + text)


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
            # Success signal: the staged image was renamed to *.flashed (old
        # leftovers were deleted before staging).  *.rejected (checksum or
        # signature refused at boot), *.stale (no OTA token) or update.bin
        # still present mean the device booted WITHOUT applying it.
        for leftover, why in (("/system/update.bin.rejected", "rejected "
                               "(checksum/signature refused — wrong --key "
                               "for this firmware?)"),
                              ("/system/update.bin", "not applied"),
                              ("/system/update.bin.stale", "ignored: the "
                               "boot saw no OTA request token")):
            try:
                pm.do_get_file_b64(port, leftover)
                fail(f"device rebooted but the update was {why} — check the "
                     "device screen, serial log and /system/error.log")
            except FileNotFoundError:
                pass
            except Exception as e:
                print(f"  (could not check {leftover}: {e})")
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
        description="Flash PicoDeck firmware to a USB device via the OTA path")
    ap.add_argument("firmware", nargs="?", default="build/picodeck.bin",
                    help="raw firmware image (default: build/picodeck.bin)")
    ap.add_argument("--device", help="serial port (default: auto-detect)")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="seconds to wait for reflash + reboot (default 240)")
    ap.add_argument("--no-verify", action="store_true",
                    help="stage and reboot, but do not wait for the device")
    ap.add_argument("--key", type=Path, default=sign_update.TEST_PRIVATE,
                    help="P-256 private key to sign with (default: the TEST "
                         "key that local/dev builds embed)")
    args = ap.parse_args()

    fw = Path(args.firmware)
    if not fw.is_file():
        fail(f"{fw} not found — run 'make build' first")
    data = fw.read_bytes()
    if data[:4] in (b"UF2\n", b"UF2\x0a"):
        fail("that is a UF2 file — the OTA path needs the raw .bin "
             "(build/picodeck.bin)")
    if len(data) < OTA_MIN_SIZE:
        fail(f"{fw} is too small ({len(data)} bytes) to be firmware")
    if len(data) > OTA_MAX_SIZE:
        fail(f"{fw} is {len(data)} bytes — exceeds the {OTA_MAX_SIZE} byte "
             "OTA limit in ota_update.c")

    port = args.device or pm.find_usb_device()
    if not port:
        fail("no PicoDeck device detected on USB (OTA flash targets hardware "
             "only); pass --device if detection failed")

    print(f"Device:   {port}")
    print(f"Firmware: {fw} ({len(data) / 1024:.0f} KB)")

    ensure_launcher(port)
    old_ver = get_ver(port)
    if old_ver:
        print(f"Current:  {old_ver}")

    try:
        sig = sign_update.sign_bytes(data, args.key)
    except RuntimeError as e:
        fail(f"signing with {args.key} failed: {e}")
    print(f"Signed:   {args.key.name} ({len(sig)}-byte ECDSA P-256 signature)")

    # Clear leftovers so the post-reboot check means this run.
    for base in ("update.bin", "update.sha256", "update.sig"):
        for suffix in (".flashed", ".stale", ".rejected"):
            try:
                pm.do_command_hardware(f"rm /system/{base}{suffix}", port,
                                       timeout=2)
            except Exception:
                pass

    sha = hashlib.sha256(data).hexdigest()
    print(f"SHA-256:  {sha[:16]}…")
    pm.do_put_file_b64(port, (sha + "\n").encode(), "/system/update.sha256")
    pm.do_put_file_b64(port, sig, "/system/update.sig")

    print(f"Uploading {len(data) / 1024:.0f} KB to /system/update.bin...")
    start = time.monotonic()

    def progress(sent: int, total: int) -> None:
        pct = sent * 100 // total
        print(f"\r  {pct:3d}%  ({sent // 1024}K / {total // 1024}K)",
              end="", flush=True)

    pm.do_put_file_b64(port, data, "/system/update.bin", progress=progress)
    print(f"\n  Uploaded in {time.monotonic() - start:.0f}s (fnv1a verified)")

    print("Rebooting — the device verifies the SHA-256 and signature and "
          "reflashes on boot. DO NOT power off.")
    request_ota_reboot(port)

    if args.no_verify:
        print("Staged and rebooted (--no-verify: not waiting for the device).")
        return
    wait_for_reflash(port, old_ver, args.timeout)


if __name__ == "__main__":
    main()
