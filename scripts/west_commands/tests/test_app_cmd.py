# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

import struct

from app_cmd import parse_manifest

SIG = bytes(range(64))


def manifest(hdr_len=160, magic=0x34464D41, fmt=4, name=b'weather'):
    body = struct.pack(
        '<II32s32s16sIHHIIQQI28s',
        7,
        4096,
        name,
        b'\x11' * 32,
        b'\x22' * 16,
        0xCAFE,
        1,
        0,
        2048,
        0,
        0x0F,
        0x03,
        0,
        b'',
    )
    mf = struct.pack('<IHH', magic, hdr_len, fmt) + body
    mf += b'\xa5' * max(0, hdr_len - len(mf))
    return mf[:hdr_len]


def test_manifest_fields():
    got = parse_manifest(manifest() + SIG)
    assert got == {
        'name': 'weather',
        'version': 7,
        'module_len': 4096,
        'target': 0xCAFE,
        'abi_major': 1,
        'heap_size': 2048,
        'key_id': 0,
        'requested_caps': 0x0F,
        'required_caps': 0x03,
        'flags': 0,
    }


def test_manifest_with_unknown_fields():
    got = parse_manifest(manifest(hdr_len=200, fmt=5) + SIG)
    assert got is not None
    assert got['name'] == 'weather'
    assert got['requested_caps'] == 0x0F


def test_manifest_refused():
    assert parse_manifest(manifest(hdr_len=100) + SIG) is None
    assert parse_manifest(manifest(hdr_len=257) + SIG) is None
    assert parse_manifest(manifest(magic=0x33464D41) + SIG) is None
    assert parse_manifest(manifest(fmt=3) + SIG) is None
    assert parse_manifest(manifest() + SIG[:-1]) is None
    assert parse_manifest(manifest() + SIG + b'\0') is None
    assert parse_manifest(b'') is None
