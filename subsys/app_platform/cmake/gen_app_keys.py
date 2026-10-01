#!/usr/bin/env python3
#
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

"""Build the table of keys trusted to sign application modules.

The key list is a YAML file with one entry per publisher key:

  keys:
    - id: 1                  # non-zero 32-bit key id, unique
      key: publisher.pem     # PEM public or private key, or the hex SEC1 point
      caps: [LOG, TIME]      # capability ceiling: names, a number or ALL
      prefix: "acme_"        # application names the key may sign, "" for any
      revoked: false         # a revoked key signs nothing; key is optional

A key path is relative to the directory of the list. An entry may set
"generate: true" to create a missing private key file, for test lists only.
Key id 0 is the development key, given with --dev-key and never listed.

The table is written as C initializers for the loader and as JSON for the
signer, and the files the table was built from are printed, separated by
semicolons, for the build to depend on.
"""

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

import yaml
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric import ec

NAME_MAX = 32
PUBKEY_LEN = 65
ENTRY_FIELDS = {"id", "key", "caps", "prefix", "revoked", "generate"}


def cap_bits(caps_h):
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(
            r"#define\s+APP_CAP_(\w+)\s+APP_CAP_BIT\((\d+)\)",
            Path(caps_h).read_text(encoding="utf-8"),
        )
    }


def parse_caps(value, bits):
    """Capabilities as a number, ALL, or names without the APP_CAP_ prefix."""
    if isinstance(value, int):
        return value
    names = value.split(",") if isinstance(value, str) else value
    caps = 0
    for name in names:
        name = str(name).strip().upper().removeprefix("APP_CAP_")
        if name == "ALL":
            caps |= (1 << (max(bits.values()) + 1)) - 1
        elif name in bits:
            caps |= 1 << bits[name]
        else:
            raise ValueError(f"unknown capability '{name}'")
    return caps


def create_private_key(path):
    """Write a new P-256 private key to path unless a key is already there."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(ser.Encoding.PEM, ser.PrivateFormat.PKCS8, ser.NoEncryption())
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        os.write(fd, pem)
        os.close(fd)
        os.link(tmp, path)
    except FileExistsError:
        pass
    finally:
        os.unlink(tmp)


def public_point(key):
    return key.public_bytes(ser.Encoding.X962, ser.PublicFormat.UncompressedPoint)


def load_public(value, base, generate):
    """SEC1 uncompressed point of a key given in hex or as a PEM file."""
    text = str(value).strip()
    hex_digits = text.lower().removeprefix("0x")
    if re.fullmatch(r"[0-9a-f]+", hex_digits) and len(hex_digits) == 2 * PUBKEY_LEN:
        point = bytes.fromhex(hex_digits)
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
        return point, None
    path = Path(text)
    if not path.is_absolute():
        path = base / path
    if not path.exists() and generate:
        create_private_key(path)
    data = path.read_bytes()
    if b"PRIVATE KEY" in data:
        key = ser.load_pem_private_key(data, None).public_key()
    else:
        key = ser.load_pem_public_key(data)
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        raise ValueError(f"{path} is not a P-256 key")
    return public_point(key), path


def read_list(path, bits):
    path = Path(path)
    doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if not isinstance(doc, dict) or set(doc) - {"keys"}:
        raise ValueError("the list must be a mapping with a 'keys' sequence")
    entries = doc.get("keys") or []
    if not isinstance(entries, list):
        raise ValueError("'keys' must be a sequence")

    keys = []
    deps = []
    for n, entry in enumerate(entries, 1):
        where = f"entry {n}"
        if not isinstance(entry, dict):
            raise ValueError(f"{where}: not a mapping")
        unknown = set(entry) - ENTRY_FIELDS
        if unknown:
            raise ValueError(f"{where}: unknown field(s) {', '.join(sorted(unknown))}")
        key_id = entry.get("id")
        if not isinstance(key_id, int) or isinstance(key_id, bool):
            raise ValueError(f"{where}: 'id' must be an integer")
        where = f"key id {key_id}"
        if key_id == 0:
            raise ValueError(f"{where}: id 0 is the development key and cannot be listed")
        if not 0 < key_id <= 0xFFFFFFFF:
            raise ValueError(f"{where}: id must fit 32 bits")
        revoked = entry.get("revoked", False)
        if not isinstance(revoked, bool):
            raise ValueError(f"{where}: 'revoked' must be true or false")
        prefix = entry.get("prefix", "")
        if not isinstance(prefix, str) or not re.fullmatch(r"[\x21-\x7e]*", prefix):
            raise ValueError(f"{where}: 'prefix' must be printable ASCII without spaces")
        if len(prefix) >= NAME_MAX:
            raise ValueError(f"{where}: 'prefix' must be shorter than {NAME_MAX} characters")

        point = None
        if "key" in entry:
            point, key_path = load_public(entry["key"], path.parent, entry.get("generate", False))
            if key_path is not None:
                deps.append(key_path)
        elif not revoked:
            raise ValueError(f"{where}: 'key' is missing")
        if "caps" in entry:
            caps = parse_caps(entry["caps"], bits)
        elif revoked:
            caps = 0
        else:
            raise ValueError(f"{where}: 'caps' is missing, list the capability ceiling")
        if not 0 <= caps < 1 << 64:
            raise ValueError(f"{where}: capabilities must fit 64 bits")

        keys.append(
            {
                "id": key_id,
                "state": "revoked" if revoked else "active",
                "caps": caps,
                "prefix": prefix,
                "pubkey": point.hex() if point is not None else None,
            }
        )
    return keys, deps


def check(keys):
    ids = set()
    points = {}
    for key in keys:
        if key["id"] in ids:
            raise ValueError(f"key id {key['id']} is listed twice")
        ids.add(key["id"])
        if key["pubkey"] is not None:
            other = points.setdefault(key["pubkey"], key["id"])
            if other != key["id"]:
                raise ValueError(f"key ids {other} and {key['id']} hold the same public key")


def c_entry(key):
    state = {"active": "KEY_ACTIVE", "revoked": "KEY_REVOKED", "disabled": "KEY_DISABLED"}
    point = bytes.fromhex(key["pubkey"]) if key["pubkey"] is not None else b""
    body = ", ".join(f"0x{b:02x}" for b in point) or "0"
    prefix = json.dumps(key["prefix"])
    return (
        f"\t{{.id = {key['id']}U, .state = {state[key['state']]}, "
        f".caps = 0x{key['caps']:x}ULL, .prefix = {prefix}, .pubkey = {{{body}}}}},\n"
    )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--keys", help="key list (YAML)")
    parser.add_argument("--dev-key", help="development private key, generated when missing")
    parser.add_argument("--caps", required=True, help="app_caps.h")
    parser.add_argument("--out-inc", required=True, help="C initializers of the table")
    parser.add_argument("--out-json", required=True, help="the table for the signer")
    args = parser.parse_args()

    bits = cap_bits(args.caps)
    deps = []
    keys = []
    try:
        if args.dev_key:
            dev = Path(args.dev_key)
            if not dev.exists():
                create_private_key(dev)
            point, _ = load_public(dev, Path.cwd(), False)
            keys.append(
                {
                    "id": 0,
                    "state": "active",
                    "caps": parse_caps("ALL", bits),
                    "prefix": "",
                    "pubkey": point.hex(),
                }
            )
            deps.append(dev)
        else:
            keys.append({"id": 0, "state": "disabled", "caps": 0, "prefix": "", "pubkey": None})
        if args.keys:
            listed, listed_deps = read_list(args.keys, bits)
            keys += listed
            deps += [Path(args.keys), *listed_deps]
        check(keys)
    except (OSError, ValueError, yaml.YAMLError) as e:
        where = args.keys if args.keys else args.dev_key
        sys.exit(f"{where}: {e}")

    inc = "/* Generated by subsys/app_platform/cmake/gen_app_keys.py, do not edit. */\n\n"
    inc += "".join(c_entry(key) for key in keys)
    Path(args.out_inc).write_text(inc, encoding="ascii")
    Path(args.out_json).write_text(json.dumps({"keys": keys}, indent=1) + "\n", encoding="ascii")
    print(";".join(str(p.resolve()) for p in deps), end="")


if __name__ == "__main__":
    main()
