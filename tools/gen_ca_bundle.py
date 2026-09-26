#!/usr/bin/env python3
"""Generate src/drivers/ca_bundle.{c,h}: the root CAs PicOS trusts for TLS.

The firmware verifies every HTTPS / tls:// server certificate against this
bundle (src/drivers/wifi.c passes it to mg_tls_init as opts.ca).  It is kept
small on purpose: each TLS connection parses the whole bundle into PSRAM.

Inputs are the PEM files in tools/ca/ (one root each, named <slug>.pem).
Regenerate after adding/removing a PEM there:

    python3 tools/gen_ca_bundle.py            # rewrite src/drivers/ca_bundle.*
    python3 tools/gen_ca_bundle.py --check    # exit 1 if they are out of date

Refresh tools/ca/ from a Mozilla-derived system store (Fedora:
/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem, Debian/Ubuntu:
/etc/ssl/certs/ca-certificates.crt needs --by-subject) with:

    python3 tools/gen_ca_bundle.py --extract /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem

To decide what a host needs, look at the chain it serves:

    openssl s_client -connect HOST:443 -servername HOST -showcerts </dev/null

and make sure the last certificate's issuer (or any certificate in the chain)
is one of the roots below.  Needs the openssl CLI.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CA_DIR = ROOT / "tools" / "ca"
OUT_C = ROOT / "src" / "drivers" / "ca_bundle.c"
OUT_H = ROOT / "src" / "drivers" / "ca_bundle.h"

# slug -> (store label / subject CN, why it is here).  Chains observed
# 2026-09-24 with openssl s_client.
ROOTS = {
    "isrg-root-x1": ("ISRG Root X1",
                     "Let's Encrypt RSA; raw/objects/release-assets."
                     "githubusercontent.com (YR1 -> Root YR -> X1)"),
    "isrg-root-x2": ("ISRG Root X2", "Let's Encrypt ECDSA"),
    "gts-root-r1": ("GTS Root R1", "Google Trust Services RSA"),
    "gts-root-r4": ("GTS Root R4",
                    "Google Trust Services ECDSA (Cloudflare Universal SSL, "
                    "e.g. store.picodeck.net)"),
    "sectigo-public-server-authentication-root-e46": (
        "Sectigo Public Server Authentication Root E46",
        "github.com, api.github.com, codeload.github.com (DV E36 -> E46)"),
    "sectigo-public-server-authentication-root-r46": (
        "Sectigo Public Server Authentication Root R46",
        "RSA twin of E46 (GitHub RSA certificates)"),
    "usertrust-ecc-certification-authority": (
        "USERTrust ECC Certification Authority",
        "cross-signs E46 (older GitHub chains)"),
    "usertrust-rsa-certification-authority": (
        "USERTrust RSA Certification Authority", "cross-signs R46"),
    "digicert-global-root-g2": ("DigiCert Global Root G2",
                                "DigiCert-issued sites and CDNs"),
    "ssl-com-tls-ecc-root-ca-2022": ("SSL.com TLS ECC Root CA 2022",
                                     "Cloudflare's third CA option"),
    "ssl-com-tls-rsa-root-ca-2022": ("SSL.com TLS RSA Root CA 2022",
                                     "Cloudflare's third CA option (RSA)"),
}

PEM_RE = re.compile(
    rb"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+?-----END CERTIFICATE-----")


def openssl(*args: str, data: bytes) -> str:
    res = subprocess.run(["openssl", *args], input=data, capture_output=True)
    if res.returncode != 0:
        sys.exit(f"openssl {' '.join(args)} failed: "
                 f"{res.stderr.decode(errors='replace').strip()}")
    return res.stdout.decode()


def describe(pem: bytes) -> dict:
    out = openssl("x509", "-noout", "-subject", "-issuer", "-enddate",
                  "-fingerprint", "-sha256", "-nameopt", "utf8,sep_comma_plus",
                  data=pem)
    info = {}
    for line in out.splitlines():
        key, _, val = line.partition("=")
        info[key.strip().lower()] = val.strip()
    if info.get("subject") != info.get("issuer"):
        sys.exit(f"not a self-signed root: {info.get('subject')}")
    cn = re.search(r"CN=([^,]+)", info["subject"])
    return {"cn": cn.group(1) if cn else info["subject"],
            "not_after": info["notafter"],
            "sha256": info["sha256 fingerprint"]}


def extract(bundle: Path) -> None:
    """Copy the ROOTS out of a system PEM bundle into tools/ca/."""
    text = bundle.read_bytes()
    found = {}
    for m in PEM_RE.finditer(text):
        pem = m.group(0).replace(b"\r\n", b"\n") + b"\n"
        cn = describe(pem)["cn"]
        for slug, (label, _why) in ROOTS.items():
            if cn == label:
                found[slug] = pem
    missing = sorted(set(ROOTS) - set(found))
    if missing:
        sys.exit(f"not in {bundle}: {', '.join(missing)}")
    CA_DIR.mkdir(parents=True, exist_ok=True)
    for slug, pem in found.items():
        (CA_DIR / f"{slug}.pem").write_bytes(pem)
    print(f"wrote {len(found)} roots to {CA_DIR}")


def render() -> tuple[str, str]:
    files = sorted(CA_DIR.glob("*.pem"))
    if not files:
        sys.exit(f"no PEM files in {CA_DIR}")
    unknown = [f.stem for f in files if f.stem not in ROOTS]
    if unknown:
        sys.exit(f"add {', '.join(unknown)} to ROOTS in {__file__} "
                 "(with the reason it is trusted)")
    parts, notes = [], []
    for f in files:
        pem = f.read_bytes()
        certs = PEM_RE.findall(pem)
        if len(certs) != 1:
            sys.exit(f"{f}: expected exactly one certificate")
        info = describe(certs[0])
        label, why = ROOTS[f.stem]
        if info["cn"] != label:
            sys.exit(f"{f}: CN {info['cn']!r} != expected {label!r}")
        parts.append(certs[0].decode().replace("\r\n", "\n").strip() + "\n")
        notes.append(f"//   {info['cn']}\n"
                     f"//     expires {info['not_after']}\n"
                     f"//     SHA-256 {info['sha256']}\n"
                     f"//     {why}\n")
    bundle = "".join(parts)
    lines = []
    for line in bundle.splitlines():
        lines.append(f'    "{line}\\n"')
    c = (
        "// GENERATED by tools/gen_ca_bundle.py from tools/ca/*.pem — do not edit.\n"
        "// Root CAs trusted for TLS server verification (wifi.c: opts.ca).\n"
        f"// {len(files)} roots:\n//\n" + "".join(notes) +
        "\n#include \"ca_bundle.h\"\n\n"
        "const char g_ca_bundle_pem[] =\n" + "\n".join(lines) + ";\n\n"
        "const unsigned g_ca_bundle_pem_len = sizeof(g_ca_bundle_pem) - 1;\n")
    h = (
        "// GENERATED by tools/gen_ca_bundle.py — do not edit.\n"
        "#pragma once\n\n"
        f"// {len(files)} PEM root certificates, concatenated, NUL-terminated.\n"
        "// Regenerate: python3 tools/gen_ca_bundle.py (see its docstring).\n"
        "extern const char g_ca_bundle_pem[];\n"
        "extern const unsigned g_ca_bundle_pem_len;  // strlen(g_ca_bundle_pem)\n")
    return c, h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--extract", type=Path, metavar="BUNDLE.pem",
                    help="refresh tools/ca/ from a system PEM bundle first")
    ap.add_argument("--check", action="store_true",
                    help="fail if src/drivers/ca_bundle.* are out of date")
    args = ap.parse_args()
    if args.extract:
        extract(args.extract)
    c, h = render()
    if args.check:
        stale = [p for p, txt in ((OUT_C, c), (OUT_H, h))
                 if not p.exists() or p.read_text() != txt]
        if stale:
            print("out of date: " + ", ".join(str(p) for p in stale))
            return 1
        print("ca_bundle is up to date")
        return 0
    OUT_C.write_text(c)
    OUT_H.write_text(h)
    print(f"wrote {OUT_C} ({len(c)} bytes) and {OUT_H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
