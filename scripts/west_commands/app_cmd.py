# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

'''west "app" command: inspect a signed application and install it on a device.'''

import argparse
import base64
import binascii
import re
import struct
import time
from pathlib import Path
from textwrap import dedent

from west.commands import WestCommand

from build_helpers import FIND_BUILD_DIR_DESCRIPTION, find_build_dir

try:
    import cbor
    import serial

    MISSING_REQUIREMENT = None
except ImportError as e:
    MISSING_REQUIREMENT = e.name

# struct app_manifest (include/zephyr/app_platform/app_manifest.h): a header of
# magic, signed length and format, then the fields, followed by the ECDSA P-256
# signature. Bytes past the fields known here are signed but not interpreted.
MANIFEST_HDR_FMT = '<IHH'
MANIFEST_FIELDS_FMT = '<II32s32s16sIHHIIQQI'
MANIFEST_MAGIC = 0x34464D41
MANIFEST_MAGIC_PREFIX = 0x00464D41
MANIFEST_FORMAT = 4
MANIFEST_MIN_LEN = 160
MANIFEST_MAX_LEN = 256
MANIFEST_SIG_LEN = 64

SMP_OP_WRITE = 2
SMP_OP_WRITE_RSP = 3
SMP_GROUP_FS = 8
SMP_GROUP_SHELL = 9
SMP_ID_FS_FILE = 0
SMP_ID_SHELL_EXEC = 0
SMP_ERR_ENOTSUP = 8

FRAME_START = b'\x06\x09'
FRAME_CONT = b'\x04\x14'
FRAME_B64_MAX = 124
UPLOAD_CHUNK = 128
UPLOAD_RETRIES = 3
REQUEST_TIMEOUT = 5.0
INSTALL_TIMEOUT = 60.0

ANSI_ESCAPE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')

CAPS_H = Path(__file__).resolve().parents[2] / 'include/zephyr/app_platform/app_caps.h'


def retired_format(blob):
    '''The format number of a manifest of a retired format, 'A' 'M' 'F' and a
    digit below the current format, or 0.'''
    if len(blob) < 4:
        return 0
    (magic,) = struct.unpack_from('<I', blob)
    digit = magic >> 24
    if (
        magic & 0xFFFFFF != MANIFEST_MAGIC_PREFIX
        or not ord('1') <= digit < ord('0') + MANIFEST_FORMAT
    ):
        return 0
    return digit - ord('0')


def caps_names(caps, caps_h=CAPS_H):
    '''Capability names without the APP_CAP_ prefix, comma-separated, for the
    bits set in caps; a bit app_caps.h does not name is shown as bitN.'''
    bits = {}
    if caps_h.is_file():
        for m in re.finditer(
            r'#define\s+APP_CAP_(\w+)\s+APP_CAP_BIT\((\d+)\)', caps_h.read_text(encoding='utf-8')
        ):
            bits[int(m.group(2))] = m.group(1)
    names = [bits.get(bit, f'bit{bit}') for bit in range(64) if caps >> bit & 1]
    return ','.join(names) if names else 'none'


def describe(manifest, caps_h=CAPS_H):
    '''The fields of a parsed manifest as lines of text.'''
    key_id = manifest['key_id']
    key = f'{key_id} (development key)' if key_id == 0 else str(key_id)
    return [
        f'name:           {manifest["name"]}',
        f'version:        {manifest["version"]}',
        f'module length:  {manifest["module_len"]}',
        f'signing key id: {key}',
        f'requested caps: {caps_names(manifest["requested_caps"], caps_h)} '
        f'(0x{manifest["requested_caps"]:x})',
        f'required caps:  {caps_names(manifest["required_caps"], caps_h)} '
        f'(0x{manifest["required_caps"]:x})',
        f'heap size:      {manifest["heap_size"]}',
        f'abi major:      {manifest["abi_major"]}',
        f'target:         0x{manifest["target"]:08x}',
    ]


def parse_manifest(blob):
    '''Return the fields of a manifest file, the manifest followed by its
    signature, or None when it is not one this tool understands.'''
    if len(blob) < struct.calcsize(MANIFEST_HDR_FMT):
        return None
    magic, hdr_len, fmt = struct.unpack_from(MANIFEST_HDR_FMT, blob)
    if (
        magic != MANIFEST_MAGIC
        or fmt < MANIFEST_FORMAT
        or not MANIFEST_MIN_LEN <= hdr_len <= MANIFEST_MAX_LEN
        or len(blob) != hdr_len + MANIFEST_SIG_LEN
    ):
        return None
    (
        version,
        module_len,
        name,
        _,
        _,
        target,
        abi_major,
        _,
        heap_size,
        key_id,
        requested_caps,
        required_caps,
        flags,
    ) = struct.unpack_from(MANIFEST_FIELDS_FMT, blob, struct.calcsize(MANIFEST_HDR_FMT))
    return {
        'name': name.split(b'\0', 1)[0].decode('ascii', errors='replace'),
        'version': version,
        'module_len': module_len,
        'target': target,
        'abi_major': abi_major,
        'heap_size': heap_size,
        'key_id': key_id,
        'requested_caps': requested_caps,
        'required_caps': required_caps,
        'flags': flags,
    }


class SmpError(Exception):
    '''An SMP request failed (rc set) or timed out (rc None).'''

    def __init__(self, msg, rc=None):
        super().__init__(msg)
        self.rc = rc


class SmpSerial:
    '''SMP client over a serial console that also carries a shell.'''

    def __init__(self, port, baud):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.open()
        self.ser.reset_input_buffer()
        self.seq = 0
        self.rx = b''

    def close(self):
        self.ser.close()

    def request(self, group, cmd, payload, timeout=REQUEST_TIMEOUT):
        body = cbor.dumps(payload)
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFF
        pkt = struct.pack('>BBHHBB', SMP_OP_WRITE, 0, len(body), group, seq, cmd) + body
        raw = struct.pack('>H', len(pkt) + 2) + pkt + struct.pack('>H', binascii.crc_hqx(pkt, 0))
        b64 = base64.b64encode(raw)
        for i in range(0, len(b64), FRAME_B64_MAX):
            marker = FRAME_START if i == 0 else FRAME_CONT
            self.ser.write(marker + b64[i : i + FRAME_B64_MAX] + b'\n')
        self.ser.flush()

        rsp = self._response(group, cmd, seq, time.monotonic() + timeout)
        if rsp is None:
            raise SmpError(f'no response to SMP group {group} command {cmd}')
        rc = rsp.get('rc', 0)
        if 'err' in rsp:
            rc = rsp['err'].get('rc', 0)
        if rc != 0:
            raise SmpError(f'SMP group {group} command {cmd} failed (rc {rc})', rc)
        return rsp

    def _readline(self, deadline):
        while b'\n' not in self.rx:
            if time.monotonic() > deadline:
                return None
            self.rx += self.ser.read(self.ser.in_waiting or 1)
        line, self.rx = self.rx.split(b'\n', 1)
        return line

    def _response(self, group, cmd, seq, deadline):
        data = None
        while (line := self._readline(deadline)) is not None:
            start = line.find(FRAME_START)
            cont = line.find(FRAME_CONT)
            if start >= 0:
                data = b''
                frag = line[start + 2 :]
            elif cont >= 0 and data is not None:
                frag = line[cont + 2 :]
            else:
                continue
            try:
                data += base64.b64decode(frag.strip(), validate=True)
            except binascii.Error:
                data = None
                continue
            if len(data) < 2:
                continue
            (total,) = struct.unpack('>H', data[:2])
            if len(data) < 2 + total:
                continue
            pkt, crc = data[2:total], data[total : total + 2]
            data = None
            if len(pkt) < 8 or struct.pack('>H', binascii.crc_hqx(pkt, 0)) != crc:
                continue
            op, _, length, rgroup, rseq, rcmd = struct.unpack('>BBHHBB', pkt[:8])
            if (op & 0x7) == SMP_OP_WRITE_RSP and (rgroup, rseq, rcmd) == (group, seq, cmd):
                return cbor.loads(pkt[8 : 8 + length])
        return None

    def upload(self, content, path):
        off = 0
        while off < len(content):
            req = {'off': off, 'data': content[off : off + UPLOAD_CHUNK], 'name': path}
            if off == 0:
                req['len'] = len(content)
            off = self._request_retry(SMP_GROUP_FS, SMP_ID_FS_FILE, req)['off']

    def _request_retry(self, group, cmd, payload):
        for _ in range(UPLOAD_RETRIES - 1):
            try:
                return self.request(group, cmd, payload)
            except SmpError as e:
                if e.rc is not None:
                    raise
        return self.request(group, cmd, payload)


class App(WestCommand):
    def __init__(self):
        super().__init__(
            'app',
            'manage applications on a running device',
            dedent('''
            Manage app_platform applications on a running device.

            The "info" subcommand prints the signed manifest of a bundle: the
            application name and version, the id of the key that signed it,
            and the capabilities it requests and requires.

            The "install" subcommand uploads a signed application bundle
            (<name>.llext and <name>.manifest) over the SMP transport of the
            device's serial shell into its storage, then runs
            "store install <name>" on the device through the SMP shell
            management group. The device verifies the signature, the ABI
            identifier and the version before it installs and launches the
            application; this command only reports its verdict.'''),
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        subparsers = parser.add_subparsers(dest='subcommand', metavar='<subcommand>', required=True)

        info = subparsers.add_parser(
            'info',
            help='show the signed manifest of an application bundle',
            formatter_class=argparse.RawDescriptionHelpFormatter,
            description=dedent('''
            Show the signed manifest of an application bundle.

            The bundle is searched for below the build directory: a
            <name>.manifest holding the signed manifest followed by its
            signature, next to the <name>.llext module it covers.

            ''')
            + FIND_BUILD_DIR_DESCRIPTION,
        )
        info.add_argument(
            '-d',
            '--build-dir',
            metavar='DIR',
            help='build directory, or any directory holding a bundle',
        )
        info.add_argument(
            '-n', '--name', help='application to show when the directory holds several'
        )

        install = subparsers.add_parser(
            'install',
            help='upload a signed application and install it',
            formatter_class=argparse.RawDescriptionHelpFormatter,
            description=dedent('''
            Upload a signed application bundle and install it on the device.

            The bundle is searched for below the build directory: a
            <name>.manifest holding the signed manifest followed by its
            signature, next to the <name>.llext module it covers.

            ''')
            + FIND_BUILD_DIR_DESCRIPTION,
        )
        install.add_argument(
            '-d',
            '--build-dir',
            metavar='DIR',
            help='build directory, or any directory holding a bundle',
        )
        install.add_argument('-p', '--port', required=True, help='serial port of the device shell')
        install.add_argument(
            '-b', '--baud', type=int, default=115200, help='baud rate (default: %(default)s)'
        )
        install.add_argument(
            '-n', '--name', help='application to install when the directory holds several'
        )
        install.add_argument(
            '--mount-point',
            default='/lfs',
            help='device storage mount point (default: %(default)s)',
        )
        return parser

    def do_run(self, args, unknown_args):
        if args.subcommand == 'info':
            self.info(args)
            return
        if MISSING_REQUIREMENT is not None:
            self.die(
                f'missing Python module "{MISSING_REQUIREMENT}"; install scripts/requirements.txt'
            )
        self.install(args)

    def find_bundle(self, directory, name):
        bundles = []
        for mf_path in sorted(directory.rglob('*.manifest')):
            llext_path = mf_path.with_suffix('.llext')
            if not llext_path.is_file():
                continue
            manifest = self.parse_manifest(mf_path, llext_path)
            if manifest is not None and name in (None, manifest['name']):
                bundles.append((manifest, llext_path, mf_path))

        if not bundles:
            what = f'bundle for application "{name}"' if name else 'signed application bundle'
            self.die(f'no {what} below {directory}')
        if len(bundles) > 1:
            found = ', '.join(f'{m["name"]} ({mf.parent})' for m, _, mf in bundles)
            self.die(f'several bundles found: {found}; select one with --name or --build-dir')
        return bundles[0]

    def parse_manifest(self, mf_path, llext_path):
        blob = mf_path.read_bytes()
        manifest = parse_manifest(blob)
        if manifest is None:
            fmt = retired_format(blob)
            if fmt != 0:
                self.wrn(f'{mf_path}: manifest format {fmt} is retired; rebuild it, skipped')
            return None
        if manifest['module_len'] != llext_path.stat().st_size:
            self.wrn(
                f'{llext_path}: {llext_path.stat().st_size} bytes, '
                f'manifest says {manifest["module_len"]}; skipped'
            )
            return None
        return manifest

    def bundle_dir(self, args):
        directory = Path(find_build_dir(args.build_dir))
        if not directory.is_dir():
            self.die(f'{directory} is not a directory')
        return directory

    def info(self, args):
        manifest, llext_path, mf_path = self.find_bundle(self.bundle_dir(args), args.name)
        self.inf(f'{mf_path}:')
        for line in describe(manifest):
            self.inf(f'  {line}')

    def install(self, args):
        manifest, llext_path, mf_path = self.find_bundle(self.bundle_dir(args), args.name)
        name = manifest['name']
        mount = args.mount_point.rstrip('/')
        self.inf(
            f'installing "{name}" v{manifest["version"]} from {llext_path.parent}, '
            f'signed with key {manifest["key_id"]}, '
            f'requesting {caps_names(manifest["requested_caps"])}'
        )

        try:
            smp = SmpSerial(args.port, args.baud)
        except serial.SerialException as e:
            self.die(f'cannot open {args.port}: {e}')

        try:
            smp.upload(mf_path.read_bytes(), f'{mount}/{name}.manifest.new')
            smp.upload(llext_path.read_bytes(), f'{mount}/{name}.llext.new')
            self.inf(
                f'uploaded {llext_path.name} ({llext_path.stat().st_size} bytes) and {mf_path.name}'
            )
            rsp = smp.request(
                SMP_GROUP_SHELL,
                SMP_ID_SHELL_EXEC,
                {'argv': ['store', 'install', name]},
                timeout=INSTALL_TIMEOUT,
            )
        except SmpError as e:
            if e.rc == SMP_ERR_ENOTSUP:
                self.die(
                    f'{e}: the device does not support this SMP group '
                    '(needs CONFIG_MCUMGR_GRP_FS and CONFIG_MCUMGR_GRP_SHELL)'
                )
            self.die(str(e))
        finally:
            smp.close()

        for line in ANSI_ESCAPE.sub('', rsp.get('o', '')).splitlines():
            if line.strip():
                self.inf(f'device: {line.strip()}')
        ret = rsp.get('ret', 0)
        if ret != 0:
            self.die(f'device refused to install "{name}" (error {ret})')
