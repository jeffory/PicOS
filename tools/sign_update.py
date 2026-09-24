#!/usr/bin/env python3
"""Sign, verify and key-gen for PicOS OTA firmware images.

Scheme: ECDSA P-256 over SHA-256 of the raw image (build/picocalc_os.bin).
The signature file is the DER ECDSA-Sig-Value that `openssl dgst -sha256
-sign` writes, so plain openssl can check it too:

    openssl dgst -sha256 -verify pub.pem -signature image.sig image.bin

The firmware embeds the PUBLIC key at build time (CMake option
PICOS_UPDATE_PUBKEY_PEM, default tests/keys/picos-update-TEST-public.pem) and
refuses to flash /system/update.bin unless /system/update.sig verifies against
it (src/os/ota_verify.c).  Everything here shells out to the openssl CLI, so
the tool needs no Python packages.

Usage:
    tools/sign_update.py sign   IMAGE [--key PRIV.pem] [--out IMAGE.sig]
    tools/sign_update.py verify IMAGE [--pub PUB.pem]  [--sig IMAGE.sig]
    tools/sign_update.py genkey PRIV.pem PUB.pem

`sign` defaults to the TEST key (tests/keys/picos-update-TEST-private.pem),
which only local/dev builds trust.  Release builds embed the real public key
and are signed in CI from the UPDATE_SIGNING_KEY secret.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_PRIVATE = ROOT / "tests" / "keys" / "picos-update-TEST-private.pem"
TEST_PUBLIC = ROOT / "tests" / "keys" / "picos-update-TEST-public.pem"


def _openssl(*args: str, input_bytes: bytes | None = None) -> bytes:
    try:
        res = subprocess.run(["openssl", *args], input=input_bytes,
                             capture_output=True, check=False)
    except FileNotFoundError:
        sys.exit("ERROR: the openssl command-line tool is required")
    if res.returncode != 0:
        raise RuntimeError(res.stderr.decode(errors="replace").strip()
                           or f"openssl {args[0]} failed")
    return res.stdout


def check_p256_private(key: Path) -> None:
    text = _openssl("pkey", "-in", str(key), "-noout", "-text").decode()
    if "prime256v1" not in text and "P-256" not in text:
        raise RuntimeError(f"{key} is not an EC P-256 private key")


def sign_bytes(data: bytes, key: Path) -> bytes:
    """DER ECDSA-SHA256 signature of `data` with the P-256 key `key`."""
    check_p256_private(key)
    return _openssl("dgst", "-sha256", "-sign", str(key), input_bytes=data)


def sign_file(image: Path, key: Path, out: Path) -> bytes:
    sig = sign_bytes(image.read_bytes(), key)
    out.write_bytes(sig)
    return sig


def verify_file(image: Path, pub: Path, sig: Path) -> bool:
    try:
        _openssl("dgst", "-sha256", "-verify", str(pub), "-signature",
                 str(sig), str(image))
        return True
    except RuntimeError:
        return False


def genkey(priv: Path, pub: Path) -> None:
    if priv.exists():
        sys.exit(f"ERROR: {priv} exists; refusing to overwrite a key")
    old = os.umask(0o077)
    try:
        _openssl("genpkey", "-algorithm", "EC", "-pkeyopt",
                 "ec_paramgen_curve:P-256", "-out", str(priv))
    finally:
        os.umask(old)
    _openssl("pkey", "-in", str(priv), "-pubout", "-out", str(pub))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("sign", help="write IMAGE.sig")
    s.add_argument("image", type=Path)
    s.add_argument("--key", type=Path, default=TEST_PRIVATE,
                   help="P-256 private key PEM (default: the TEST key)")
    s.add_argument("--out", type=Path, help="default: IMAGE with .sig suffix")

    v = sub.add_parser("verify", help="check IMAGE.sig")
    v.add_argument("image", type=Path)
    v.add_argument("--pub", type=Path, default=TEST_PUBLIC)
    v.add_argument("--sig", type=Path)

    g = sub.add_parser("genkey", help="create a new P-256 key pair")
    g.add_argument("private", type=Path)
    g.add_argument("public", type=Path)

    args = ap.parse_args()
    try:
        if args.cmd == "sign":
            out = args.out or args.image.with_suffix(".sig")
            sig = sign_file(args.image, args.key, out)
            if args.key.resolve() == TEST_PRIVATE.resolve():
                print("NOTE: signed with the TEST key (dev builds only)",
                      file=sys.stderr)
            print(f"{out} ({len(sig)} bytes)")
        elif args.cmd == "verify":
            sig = args.sig or args.image.with_suffix(".sig")
            ok = verify_file(args.image, args.pub, sig)
            print("Verified OK" if ok else "Verification FAILED")
            return 0 if ok else 1
        else:
            genkey(args.private, args.public)
            print(f"private: {args.private} (keep secret)\npublic:  {args.public}")
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
