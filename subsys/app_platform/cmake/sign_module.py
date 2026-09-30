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
(app_platform_abi.txt), one key=value per line.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric import ec, utils

MAGIC = 0x34464D41
FORMAT = 4
NAME_MAX = 32
HEADER = struct.Struct("<IHH")
FIELDS = struct.Struct("<II32s32s16sIHHIIQQI28s")
MANIFEST_LEN = HEADER.size + FIELDS.size
MAX_LEN = 256


def read_abi_file(path):
    facts = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            facts[key.strip()] = value.strip()
    return facts


def build_manifest(module, args, facts):
    name = args.name.encode("ascii")
    if len(name) >= NAME_MAX:
        sys.exit(f"application name '{args.name}' is longer than {NAME_MAX - 1} bytes")
    abi_id = bytes.fromhex(facts["abi_id"])
    abi_major = int(args.abi_major if args.abi_major is not None else facts["abi_major"], 0)
    target = int(args.target if args.target is not None else facts["target"], 0)
    body = FIELDS.pack(
        args.version,
        len(module),
        name,
        hashlib.sha256(module).digest(),
        abi_id,
        target,
        abi_major,
        0,
        args.heap_size,
        args.key_id,
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


def load_key(key_path):
    if not key_path.exists():
        key_path.parent.mkdir(parents=True, exist_ok=True)
        key = ec.generate_private_key(ec.SECP256R1())
        key_path.write_bytes(
            key.private_bytes(ser.Encoding.PEM, ser.PrivateFormat.PKCS8, ser.NoEncryption())
        )
    return ser.load_pem_private_key(key_path.read_bytes(), None)


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
    parser.add_argument("--key", required=True, help="private key, generated when missing")
    parser.add_argument("--abi-file", required=True, help="firmware facts")
    parser.add_argument("--name", required=True, help="application name")
    parser.add_argument("--version", type=int, required=True, help="application version")
    parser.add_argument("--heap-size", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--requested-caps", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--required-caps", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--key-id", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--abi-major", help="interface major version, overrides the firmware's")
    parser.add_argument("--target", help="target fingerprint, overrides the firmware's")
    parser.add_argument(
        "--manifest-len",
        type=int,
        help="signed length, cutting the manifest short or appending unknown fields",
    )
    parser.add_argument("--out-mf-inc", required=True, help="manifest as C byte literals")
    parser.add_argument("--out-sig-inc", required=True, help="signature as C byte literals")
    parser.add_argument("--out-pub-inc", help="public key as C byte literals")
    parser.add_argument("--out-manifest", help="manifest followed by its signature")
    args = parser.parse_args()

    for caps in (args.requested_caps, args.required_caps):
        if not 0 <= caps < 1 << 64:
            sys.exit("capabilities must fit 64 bits")
    if args.required_caps & ~args.requested_caps:
        sys.exit("required capabilities must be a subset of the requested ones")

    module = Path(args.module).read_bytes()
    key = load_key(Path(args.key))
    mf = build_manifest(module, args, read_abi_file(args.abi_file))
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
