# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

import struct

from app_cmd import caps_names, describe, parse_manifest, retired_format

SIG = bytes(range(64))


def manifest(hdr_len=160, magic=0x34464D41, fmt=4, name=b'weather', key_id=0):
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
        key_id,
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


def test_manifest_key_id():
    got = parse_manifest(manifest(key_id=0x12345678) + SIG)
    assert got['key_id'] == 0x12345678


def test_retired_formats():
    for fmt in (1, 2, 3):
        mf = manifest(magic=0x00464D41 | (ord(str(fmt)) << 24), hdr_len=124)
        assert retired_format(mf) == fmt
        assert parse_manifest(mf + SIG) is None
    assert retired_format(manifest()) == 0
    assert retired_format(manifest(magic=0x30464D41)) == 0
    assert retired_format(manifest(magic=0x35464D41)) == 0
    assert retired_format(b'AMF') == 0


def test_caps_names(tmp_path):
    caps_h = tmp_path / 'app_caps.h'
    caps_h.write_text(
        '#define APP_CAP_LOG     APP_CAP_BIT(0)\n'
        '#define APP_CAP_RANDOM  APP_CAP_BIT(2)\n'
        '#define APP_CAP_TIME    APP_CAP_BIT(3)\n'
    )
    assert caps_names(0, caps_h) == 'none'
    assert caps_names(0x0D, caps_h) == 'LOG,RANDOM,TIME'
    assert caps_names(0x1 | 1 << 40, caps_h) == 'LOG,bit40'


def test_describe_names_key_and_caps(tmp_path):
    caps_h = tmp_path / 'app_caps.h'
    caps_h.write_text(
        '#define APP_CAP_LOG     APP_CAP_BIT(0)\n'
        '#define APP_CAP_CRYPTO  APP_CAP_BIT(1)\n'
        '#define APP_CAP_RANDOM  APP_CAP_BIT(2)\n'
        '#define APP_CAP_TIME    APP_CAP_BIT(3)\n'
    )
    lines = describe(parse_manifest(manifest(key_id=7) + SIG), caps_h)
    assert 'signing key id: 7' in lines
    assert 'requested caps: LOG,CRYPTO,RANDOM,TIME (0xf)' in lines
    assert 'required caps:  LOG,CRYPTO (0x3)' in lines
    lines = describe(parse_manifest(manifest() + SIG), caps_h)
    assert 'signing key id: 0 (development key)' in lines
