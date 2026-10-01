#!/usr/bin/env python3
#
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

"""Pack the development kit of application modules for one firmware.

The kit holds what a module is built, checked and signed with on a host that
has neither the Zephyr tree nor the firmware build:

- the headers a module includes, the module SDK header app_sdk.h and every
  header it includes from the tree or the build, found by preprocessing it with
  the module compiler flags; the kit refuses to hold a kernel, syscall,
  devicetree or driver header,
- a configuration header with the few architecture options those headers test,
  in place of the firmware configuration,
- the module compiler and link flags: those the firmware build compiles an
  application module with, without the include paths, the firmware
  configuration, the definitions of other subsystems and the host paths,
- the toolchain the firmware was built with: its variant, target triple,
  compiler version and Zephyr SDK version, but not the toolchain itself,
- the interface lock file, the symbols a module may import, the firmware facts
  the signer puts in a manifest, and the public part of the table of trusted
  keys,
- the signer, the CMake helper that builds, checks and signs a module, and a
  README.

The compiler flags are taken from the compile command of a probe module built
with add_llext_target(), which applies the LLEXT lists of flags to remove and to
append, and are filtered further by the LLEXT EDK list of flags to remove.
"""

import argparse
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

CONFIG_H = "app_platform_edk_config.h"

# The options the module headers test that depend on the target only.
CONFIG_KEEP = re.compile(
    r"CONFIG_(ARCH|ARM|ARM64|RISCV|XTENSA|X86|X86_64|ARC|MIPS|SPARC|RX|OPENRISC|"
    r"TRICORE|HEXAGON|64BIT|LITTLE_ENDIAN|BIG_ENDIAN|ENFORCE_ZEPHYR_STDINT|LLEXT|"
    r"ISA_THUMB2|ISA_ARM|ASSEMBLER_ISA_THUMB2)"
)

# The definitions of the C library and of the platform; those of other
# subsystems and of the kernel stay out of a module.
DEFINE_KEEP = re.compile(
    r"-D(__ZEPHYR__|__LINUX_ERRNO_EXTENSIONS__|_POSIX_\w+|_XOPEN_\w+|_GNU_SOURCE|"
    r"_DEFAULT_SOURCE|_BSD_SOURCE|_ANSI_SOURCE|PICOLIBC_\w+|__PICOLIBC_\w+|"
    r"LL_EXTENSION_BUILD|APP_PLATFORM_MODULE)(=.*)?"
)

FORBIDDEN_HEADERS = re.compile(
    r"(^|/)(kernel\.h|kernel_includes\.h|syscall\.h|syscall_list\.h|autoconf\.h|"
    r"devicetree\.h|devicetree_generated\.h|device\.h|app_services\.h)$|"
    r"^zephyr/(kernel|syscalls|arch|drivers|devicetree)/"
)

FLAGS_DROPPED = re.compile(
    r"-fdiagnostics-color(=.*)?|-c|-MD|-MMD|--sysroot=.*|-f(macro|file|debug)-prefix-map=.*"
)
FLAGS_WITH_ARG = ("-o", "-MT", "-MF", "-MQ")
FLAGS_WITH_PATH = ("-isystem", "-iquote", "-imacros", "-include", "-I")


def fail(msg):
    sys.exit(f"app_platform_edk: {msg}")


def probe_command(args):
    """The compiler arguments the build compiles the probe module with."""
    probe = Path(args.probe).resolve()
    try:
        entries = json.loads(Path(args.compile_commands).read_text(encoding="utf-8"))
    except OSError as e:
        fail(f"{e}: the kit needs CMAKE_EXPORT_COMPILE_COMMANDS")
    for entry in entries:
        if Path(entry["directory"], entry["file"]).resolve() == probe:
            tokens = entry.get("arguments") or shlex.split(entry["command"])
            cc = Path(args.cc).resolve()
            start = next((i for i, t in enumerate(tokens) if Path(t).resolve() == cc), None)
            if start is None:
                fail(f"the probe module is not compiled with {cc}")
            return [t for t in tokens[start + 1 :] if Path(t).resolve() != probe]
    fail(f"{args.compile_commands} has no compile command for {probe}")


def under(path, root):
    try:
        return Path(path).resolve().relative_to(Path(root).resolve())
    except ValueError:
        return None


def module_flags(args):
    """Return the kit flags, a path into the kit given as (option, kind, path),
    the include options of the firmware build and the -imacros files to copy
    into the kit.
    """
    removed = [re.compile(r) for r in args.remove_flag]
    flags = []
    incs = []
    imacros = []
    tokens = iter(probe_command(args))
    for flag in tokens:
        if flag in FLAGS_WITH_ARG:
            next(tokens)
            continue
        option = next((o for o in FLAGS_WITH_PATH if flag.startswith(o)), None)
        if option is not None:
            value = flag[len(option) :] or next(tokens)
            if option != "-imacros":
                incs += [option, value]
            elif Path(value).resolve() == Path(args.autoconf).resolve():
                flags.append(("-imacros", "config", CONFIG_H))
            else:
                rel = under(value, Path(args.zephyr_base) / "include")
                if rel is None:
                    fail(f"-imacros {value} is outside the module headers")
                flags.append(("-imacros", "include", str(rel)))
                imacros.append(rel)
            continue
        if flag.startswith("-D"):
            if DEFINE_KEEP.fullmatch(flag):
                flags.append(flag)
            continue
        if FLAGS_DROPPED.fullmatch(flag) or any(r.fullmatch(flag) for r in removed):
            continue
        flags.append(flag)
    return flags, incs, imacros


def resolve(flags, include_dir, config_h):
    out = []
    for flag in flags:
        if isinstance(flag, tuple):
            option, kind, rel = flag
            out.append(option + str(config_h if kind == "config" else include_dir / rel))
        else:
            out.append(flag)
    return out


def kit_flags(flags):
    return [*flags, ("-I", "include", "")]


def dependencies(args, flags, cwd):
    res = subprocess.run(
        [args.cc, *flags, "-M", args.probe], cwd=cwd, capture_output=True, text=True, check=False
    )
    if res.returncode != 0:
        fail(f"the module SDK header does not build with the module flags:\n{res.stderr}")
    deps = shlex.split(res.stdout.replace("\\\n", " "))
    probe = Path(args.probe).resolve()
    return [Path(d) for d in deps[1:] if Path(d).resolve() != probe]


def config_header(autoconf):
    lines = ["/* Target options of the firmware the kit was made for. */", ""]
    for line in Path(autoconf).read_text(encoding="utf-8").splitlines():
        m = re.match(r"#define (\w+) ", line)
        if m and CONFIG_KEEP.fullmatch(m.group(1)):
            lines.append(line)
    return "\n".join(lines) + "\n"


def collect_headers(args, kit, flags, incs, toolchain_root, work):
    """Copy the headers the SDK header includes into the kit include dir."""
    roots = [Path(args.zephyr_base) / "include", Path(args.binary_dir) / "include/generated"]
    config_h = Path(work) / CONFIG_H
    copied = {}
    for dep in dependencies(args, resolve(flags, roots[0], config_h) + incs, work):
        if dep.resolve() == config_h.resolve():
            continue
        if under(dep, toolchain_root) is not None:
            continue
        rel = next((r for r in (under(dep, root) for root in roots) if r is not None), None)
        if rel is None:
            fail(f"{dep} is included by the module SDK header but is outside the kit")
        if FORBIDDEN_HEADERS.search(rel.as_posix()):
            fail(f"the module SDK header includes {rel.as_posix()}, which a module must not see")
        if rel in copied and copied[rel] != dep.resolve():
            fail(f"{rel.as_posix()} is both {copied[rel]} and {dep}")
        copied[rel] = dep.resolve()

    for rel, src in copied.items():
        dst = kit / "include" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    return sorted(copied)


def check_kit_headers(args, kit, flags, toolchain_root, work):
    """Build the probe against the kit alone."""
    include = kit / "include"
    deps = dependencies(args, resolve(kit_flags(flags), include, include / CONFIG_H), work)
    for dep in deps:
        if under(dep, include) is None and under(dep, toolchain_root) is None:
            fail(f"the kit headers include {dep} from outside the kit")


def cmake_value(flags, kit_var):
    out = []
    for flag in flags:
        if isinstance(flag, tuple):
            option, kind, rel = flag
            path = CONFIG_H if kind == "config" else rel
            flag = option + ("${" + kit_var + "}/include/" + path).rstrip("/")
        else:
            flag = flag.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$")
        out.append(flag)
    return ";".join(out)


def toolchain_facts(args):
    cc = Path(args.cc).resolve()
    facts = dict(f.split("=", 1) for f in args.fact)
    facts["compiler"] = cc.name
    if not cc.name.endswith("gcc"):
        fail(f"{cc.name} is not a GNU C compiler driver; the kit supports GNU toolchains")
    facts["tool_prefix"] = cc.name[: -len("gcc")]
    sdk_bin = under(cc.parent, args.sdk_dir) if args.sdk_dir else None
    facts["sdk_bin_dir"] = sdk_bin.as_posix() if sdk_bin is not None else ""
    return facts


def write_cmake_target(kit, facts, flags, link_flags):
    names = {
        "board": "APP_EDK_BOARD",
        "arch": "APP_EDK_ARCH",
        "llext_type": "APP_EDK_LLEXT_TYPE",
        "toolchain_variant": "APP_EDK_TOOLCHAIN_VARIANT",
        "sdk_version": "APP_EDK_SDK_VERSION",
        "sdk_bin_dir": "APP_EDK_SDK_BIN_DIR",
        "triple": "APP_EDK_TRIPLE",
        "tool_prefix": "APP_EDK_TOOL_PREFIX",
        "compiler": "APP_EDK_COMPILER",
        "compiler_version": "APP_EDK_COMPILER_VERSION",
    }
    lines = ["# Generated by gen_app_edk.py: the firmware target of this kit.", ""]
    lines += [f'set({var} "{facts.get(key, "")}")' for key, var in names.items()]
    lines.append(f'set(APP_EDK_CFLAGS "{cmake_value(kit_flags(flags), "APP_EDK_DIR")}")')
    lines.append(f'set(APP_EDK_LINK_FLAGS "{cmake_value(link_flags, "APP_EDK_DIR")}")')
    (kit / "cmake/app_edk_target.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_facts(args, kit, facts):
    """The firmware facts for the signer, with paths into the kit."""
    keep = ("abi_major", "target", "target_desc")
    fw = {}
    for line in Path(args.abi_file).read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() in keep:
            fw[key.strip()] = value.strip()
    missing = [k for k in keep if k not in fw]
    if missing:
        fail(f"{args.abi_file} lacks {', '.join(missing)}")
    fw["imports"] = "abi/app_imports.txt"
    fw["keys"] = "keys.json"
    fw["caps"] = "include/zephyr/app_platform/app_caps.h"
    for key in ("board", "arch", "llext_type", "toolchain_variant", "triple", "compiler"):
        fw[key] = facts.get(key, "")
    fw["compiler_version"] = facts.get("compiler_version", "")
    fw["sdk_version"] = facts.get("sdk_version", "")
    text = "".join(f"{k}={v}\n" for k, v in fw.items())
    (kit / "firmware.txt").write_text(text, encoding="utf-8")


def write_keys(args, kit):
    fields = {"id", "state", "caps", "prefix", "pubkey"}
    keys = json.loads(Path(args.keys).read_text(encoding="utf-8"))["keys"]
    for key in keys:
        if set(key) - fields:
            fail(f"{args.keys} holds fields other than {', '.join(sorted(fields))}")
    text = json.dumps({"keys": keys}, indent=1) + "\n"
    (kit / "keys.json").write_text(text, encoding="ascii")


def check_no_host_paths(args, kit):
    paths = {str(Path(p).resolve()) for p in (args.zephyr_base, args.binary_dir)}
    for path in sorted(kit.rglob("*")):
        if path.is_file():
            text = path.read_text(encoding="utf-8", errors="replace")
            for p in paths:
                if p in text:
                    fail(f"{path.relative_to(kit)} names the build host path {p}")


def archive(kit, out, name):
    def reset(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        if info.isdir() or os.access(kit.parent / info.name, os.X_OK):
            info.mode = 0o755
        else:
            info.mode = 0o644
        return info

    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:xz", format=tarfile.PAX_FORMAT) as tar:
        for path in [kit, *sorted(kit.rglob("*"))]:
            arcname = name if path == kit else f"{name}/{path.relative_to(kit).as_posix()}"
            tar.add(path, arcname=arcname, recursive=False, filter=reset)
    Path(out).write_bytes(data.getvalue())


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument("--out", required=True, help="kit archive (.tar.xz)")
    parser.add_argument("--name", required=True, help="top directory of the kit")
    parser.add_argument("--cc", required=True, help="C compiler of the firmware")
    parser.add_argument("--sdk-dir", help="Zephyr SDK the compiler belongs to")
    parser.add_argument("--compile-commands", required=True, help="compile_commands.json")
    parser.add_argument("--probe", required=True, help="source of the probe module")
    parser.add_argument("--remove-flag", action="append", default=[], help="regex of a flag")
    parser.add_argument("--link-flag", action="append", default=[], help="module link flag")
    parser.add_argument("--autoconf", required=True, help="autoconf.h of the firmware")
    parser.add_argument("--zephyr-base", required=True)
    parser.add_argument("--binary-dir", required=True, help="zephyr build directory")
    parser.add_argument("--abi-file", required=True, help="firmware facts for the signer")
    parser.add_argument("--imports", required=True, help="symbols a module may import")
    parser.add_argument("--keys", required=True, help="table of trusted keys (JSON)")
    parser.add_argument("--lock", required=True, help="interface lock file")
    parser.add_argument("--scripts", nargs="+", required=True, help="signer scripts")
    parser.add_argument("--templates", nargs="+", required=True, help="kit files")
    parser.add_argument("--fact", action="append", default=[], help="key=value")
    args = parser.parse_args()

    toolchain_root = Path(args.cc).resolve().parent.parent
    facts = toolchain_facts(args)

    with tempfile.TemporaryDirectory(prefix="app_edk_") as tmp:
        work = Path(tmp) / "work"
        kit = Path(tmp) / args.name
        work.mkdir()
        for sub in ("include", "abi", "cmake", "scripts"):
            (kit / sub).mkdir(parents=True)

        config = config_header(args.autoconf)
        (work / CONFIG_H).write_text(config, encoding="utf-8")
        (kit / "include" / CONFIG_H).write_text(config, encoding="utf-8")

        flags, incs, imacros = module_flags(args)
        headers = collect_headers(args, kit, flags, incs, toolchain_root, work)
        for rel in imacros:
            if rel not in headers:
                dst = kit / "include" / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(Path(args.zephyr_base) / "include" / rel, dst)
        check_kit_headers(args, kit, flags, toolchain_root, work)

        write_cmake_target(kit, facts, flags, args.link_flag)
        write_facts(args, kit, facts)
        write_keys(args, kit)
        shutil.copyfile(args.lock, kit / "abi/app_abi.lock")
        shutil.copyfile(args.imports, kit / "abi/app_imports.txt")
        for script in args.scripts:
            dst = kit / "scripts" / Path(script).name
            shutil.copyfile(script, dst)
            if dst.suffix == ".py":
                dst.chmod(0o755)
        for template in args.templates:
            sub = "cmake" if template.endswith(".cmake") else "."
            shutil.copyfile(template, kit / sub / Path(template).name)

        check_no_host_paths(args, kit)
        archive(kit, args.out, args.name)


if __name__ == "__main__":
    main()
