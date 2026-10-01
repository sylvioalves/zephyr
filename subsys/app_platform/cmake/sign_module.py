#!/usr/bin/env python3
#
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

"""Build and sign the manifest of an application module.

The manifest binds the application name, version, heap size and capabilities
to the SHA-256 of the module and to the firmware it was built for, and is
signed with an ECDSA P-256 key. The layout matches struct app_manifest in
include/zephyr/app_platform/app_manifest.h: a header of magic, length and
format, then the fields, packed and little-endian.

The firmware facts come from the file the firmware build writes
(app_platform_abi.txt), or the one of a module development kit, one key=value
per line; a relative path in it is taken from the directory of the file. A
module that imports a symbol outside the list of importable symbols it names is
not signed.

The manifest is signed with --key under --key-id, or with the key the firmware
build was given (APP_SIGNING_KEY and APP_SIGNING_KEY_ID), or else with the
development key, key id 0, at --dev-key, generated when missing. The key must
be the one the firmware lists under that id, not revoked, and allowed to sign
the application name and its required capabilities.
"""

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric import ec, utils
from gen_app_keys import create_private_key, public_point

MAGIC = 0x34464D41
FORMAT = 4
NAME_MAX = 32
HEADER = struct.Struct("<IHH")
FIELDS = struct.Struct("<II32s32s16sIHHIIQQI28s")
MANIFEST_LEN = HEADER.size + FIELDS.size
MAX_LEN = 256
CAPS_H = "include/zephyr/app_platform/app_caps.h"


def cap_bits(caps_h):
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(
            r"#define\s+APP_CAP_(\w+)\s+APP_CAP_BIT\((\d+)\)",
            Path(caps_h).read_text(encoding="utf-8"),
        )
    }


def cap_names(caps, caps_h):
    bits = sorted(cap_bits(caps_h).items(), key=lambda b: b[1])
    return ",".join(name for name, bit in bits if caps >> bit & 1) or "none"


def parse_caps(value, caps_h):
    """Capabilities as a number or as comma-separated names from app_caps.h."""
    value = value.strip()
    if not value or value[0].isdigit():
        return int(value or "0", 0)
    bits = cap_bits(caps_h)
    caps = 0
    for name in value.split(","):
        name = name.strip().upper().removeprefix("APP_CAP_")
        if name not in bits:
            sys.exit(f"unknown capability '{name}'")
        caps |= 1 << bits[name]
    return caps


def read_abi_file(path):
    facts = {}
    base = Path(path).resolve().parent
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            facts[key.strip()] = value.strip()
    for key in ("imports", "keys", "caps", "signing_key"):
        if facts.get(key):
            facts[key] = str(base / facts[key])
    if not facts.get("caps"):
        facts["caps"] = str(Path(__file__).resolve().parents[3] / CAPS_H)
    return facts


def build_manifest(module, args, facts, key_id):
    name = args.name.encode("ascii")
    if len(name) >= NAME_MAX:
        sys.exit(f"application name '{args.name}' is longer than {NAME_MAX - 1} bytes")
    abi_major = int(args.abi_major if args.abi_major is not None else facts["abi_major"], 0)
    target = int(args.target if args.target is not None else facts["target"], 0)
    body = FIELDS.pack(
        args.version,
        len(module),
        name,
        hashlib.sha256(module).digest(),
        b"",
        target,
        abi_major,
        0,
        args.heap_size,
        key_id,
        args.requested_caps,
        args.required_caps,
        0,
        b"",
    )
    length = args.manifest_len if args.manifest_len is not None else MANIFEST_LEN
    mf = HEADER.pack(MAGIC, length, FORMAT) + body
    if length > len(mf):
        mf += bytes((0xA5 + i) & 0xFF for i in range(length - len(mf)))
    return mf[:length]


def forbidden_imports(module_path, facts, nm):
    out = subprocess.run([nm, "-u", module_path], capture_output=True, text=True, check=True).stdout
    imported = {line.split()[-1] for line in out.splitlines() if line.strip()}
    allowed = set(Path(facts["imports"]).read_text(encoding="utf-8").split())
    return sorted(imported - allowed)


def signing_key(args, facts):
    """The private key path and key id the manifest is signed with."""
    if args.key:
        if args.key_id is None:
            sys.exit("a signing key needs its key id: set APP_SIGNING_KEY_ID")
        return Path(args.key), args.key_id
    if facts.get("signing_key"):
        key_id = args.key_id if args.key_id is not None else int(facts["signing_key_id"], 0)
        return Path(facts["signing_key"]), key_id
    if not args.dev_key:
        sys.exit("no signing key: set KEY_DIR for the development key, or APP_SIGNING_KEY")
    dev_key = Path(args.dev_key)
    if not dev_key.exists():
        create_private_key(dev_key)
    return dev_key, args.key_id if args.key_id is not None else 0


def load_key(key_path):
    try:
        key = ser.load_pem_private_key(key_path.read_bytes(), None)
    except (OSError, ValueError, TypeError) as e:
        sys.exit(f"cannot read the signing key {key_path}: {e}")
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{key_path} is not a P-256 private key")
    return key


def check_key(key, key_path, key_id, args, facts):
    """Refuse a key the firmware would refuse; warn of capabilities it would drop."""
    keys_path = facts["keys"]
    keys = json.loads(Path(keys_path).read_text(encoding="utf-8"))["keys"]
    entry = next((k for k in keys if k["id"] == key_id), None)
    where = f"key id {key_id}"
    if entry is None:
        sys.exit(f"{where} is not in the firmware key list ({keys_path})")
    if entry["state"] == "disabled":
        sys.exit(
            "key id 0 is the development key, which this firmware does not accept "
            "(CONFIG_APP_PLATFORM_DEV_KEY is disabled): sign with APP_SIGNING_KEY and "
            "APP_SIGNING_KEY_ID"
        )
    if entry["state"] == "revoked":
        sys.exit(f"{where} is revoked in the firmware key list")
    if public_point(key.public_key()).hex() != entry["pubkey"]:
        sys.exit(f"{key_path} is not the key the firmware lists as {where}")
    if not args.name.startswith(entry["prefix"]):
        sys.exit(f"{where} signs only names starting with '{entry['prefix']}', not '{args.name}'")
    missing = args.required_caps & ~entry["caps"]
    if missing:
        sys.exit(
            f"required capabilities {cap_names(missing, facts['caps'])} are outside the "
            f"ceiling of {where}"
        )
    dropped = args.requested_caps & ~entry["caps"]
    if dropped:
        print(
            f"warning: requested capabilities {cap_names(dropped, facts['caps'])} are outside "
            f"the ceiling of {where} and are not granted",
            file=sys.stderr,
        )


def sign(key, data):
    digest = hashlib.sha256(data).digest()
    der = key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def emit(path, data):
    Path(path).write_text(",".join(f"0x{b:02x}" for b in data) + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--module", required=True, help="module to sign")
    parser.add_argument("--key", help="private key of a listed signing key")
    parser.add_argument("--dev-key", help="development private key, generated when missing")
    parser.add_argument("--abi-file", required=True, help="firmware facts")
    parser.add_argument("--nm", help="nm tool of the toolchain, overrides the firmware's")
    parser.add_argument("--name", required=True, help="application name")
    parser.add_argument("--version", type=int, required=True, help="application version")
    parser.add_argument("--heap-size", type=lambda v: int(v, 0), default=0)
    parser.add_argument(
        "--requested-caps",
        required=True,
        help="capabilities the application asks for, a number or names such as LOG,TIME",
    )
    parser.add_argument(
        "--required-caps",
        default="0",
        help="capabilities it cannot run without, a subset of the requested ones",
    )
    parser.add_argument(
        "--key-id", type=lambda v: int(v, 0), help="signing key id, 0 for the development key"
    )
    parser.add_argument("--abi-major", help="interface major version, overrides the firmware's")
    parser.add_argument("--target", help="target fingerprint, overrides the firmware's")
    parser.add_argument(
        "--manifest-len",
        type=int,
        help="signed length, cutting the manifest short or appending unknown fields",
    )
    parser.add_argument(
        "--skip-import-check",
        action="store_true",
        help="sign a module importing symbols the loader refuses, for tests",
    )
    parser.add_argument(
        "--skip-key-check",
        action="store_true",
        help="sign with a key the firmware refuses for this module, for tests",
    )
    parser.add_argument("--out-mf-inc", required=True, help="manifest as C byte literals")
    parser.add_argument("--out-sig-inc", required=True, help="signature as C byte literals")
    parser.add_argument("--out-pub-inc", help="public key as C byte literals")
    parser.add_argument("--out-manifest", help="manifest followed by its signature")
    args = parser.parse_args()

    facts = read_abi_file(args.abi_file)
    args.requested_caps = parse_caps(args.requested_caps, facts["caps"])
    args.required_caps = parse_caps(args.required_caps, facts["caps"])
    for caps in (args.requested_caps, args.required_caps):
        if not 0 <= caps < 1 << 64:
            sys.exit("capabilities must fit 64 bits")
    if args.required_caps & ~args.requested_caps:
        sys.exit("required capabilities must be a subset of the requested ones")

    if not args.skip_import_check:
        nm = args.nm or facts.get("nm")
        if not nm:
            sys.exit("no nm tool to list the module imports: give --nm")
        forbidden = forbidden_imports(args.module, facts, nm)
        if forbidden:
            sys.exit(
                f"{args.module} imports symbols a module may not use: {', '.join(forbidden)}\n"
                f"A module may import only the symbols listed in {facts['imports']}."
            )

    key_path, key_id = signing_key(args, facts)
    if not 0 <= key_id <= 0xFFFFFFFF:
        sys.exit("the key id must fit 32 bits")
    key = load_key(key_path)
    if not args.skip_key_check and facts.get("keys"):
        check_key(key, key_path, key_id, args, facts)

    module = Path(args.module).read_bytes()
    mf = build_manifest(module, args, facts, key_id)
    sig = sign(key, mf)

    emit(args.out_mf_inc, mf)
    emit(args.out_sig_inc, sig)
    if args.out_pub_inc:
        pub = key.public_key().public_bytes(ser.Encoding.X962, ser.PublicFormat.UncompressedPoint)
        emit(args.out_pub_inc, pub)
    if args.out_manifest:
        Path(args.out_manifest).write_bytes(mf + sig)


if __name__ == "__main__":
    main()
