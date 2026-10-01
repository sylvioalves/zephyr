#!/usr/bin/env python3
#
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

"""Generate the application module files of the LLEXT EDK of one firmware.

The LLEXT EDK of a firmware with the application module profile holds what a
module is built, checked and signed with on a host that has neither the Zephyr
tree nor the firmware build. This script writes the files of the profile that
depend on the firmware build:

- a configuration header with the few architecture options the module headers
  test, which the EDK holds in place of the firmware configuration,
- the toolchain the firmware was built with: its variant, target triple,
  compiler version and Zephyr SDK version, but not the toolchain itself, the
  definitions of the C library and of the platform a module is compiled with
  in place of those of the firmware build, and the module link flags, as a
  CMake file,
- the firmware facts the signer puts in a manifest, the symbols a module may
  import and the public part of the table of trusted keys.

It also checks that the module SDK header app_sdk.h, preprocessed with the
compiler flags of a probe module built with add_llext_target(), includes no
kernel, syscall, devicetree or driver header and no header from outside the
tree or the build: the EDK holds these headers.
"""

import argparse
import json
import re
import shlex
import shutil
import subprocess
import sys
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
    """Return the module flags, a header path given as (option, kind, path),
    and the include options of the firmware build.
    """
    removed = [re.compile(r) for r in args.remove_flag]
    flags = []
    incs = []
    tokens = iter(probe_command(args))
    for flag in tokens:
        if flag in FLAGS_WITH_ARG:
            next(tokens)
            continue
        option = next((o for o in FLAGS_WITH_PATH if flag.startswith(o)), None)
        if option is not None:
            value = flag[len(option) :] or next(tokens)
            if option == "-include":
                continue
            if option != "-imacros":
                incs += [option, value]
            elif Path(value).resolve() == Path(args.autoconf).resolve():
                flags.append(("-imacros", "config", CONFIG_H))
            else:
                rel = under(value, Path(args.zephyr_base) / "include")
                if rel is None:
                    fail(f"-imacros {value} is outside the module headers")
                flags.append(("-imacros", "include", str(rel)))
            continue
        if flag.startswith("-D"):
            if DEFINE_KEEP.fullmatch(flag):
                flags.append(flag)
            continue
        if FLAGS_DROPPED.fullmatch(flag) or any(r.fullmatch(flag) for r in removed):
            continue
        flags.append(flag)
    return flags, incs


def resolve(flags, include_dir, config_h):
    out = []
    for flag in flags:
        if isinstance(flag, tuple):
            option, kind, rel = flag
            out.append(option + str(config_h if kind == "config" else include_dir / rel))
        else:
            out.append(flag)
    return out


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
    lines += ["", "#define APP_PLATFORM_MODULE 1"]
    return "\n".join(lines) + "\n"


def check_headers(args, flags, incs, config_h, toolchain_root):
    """Check the headers the SDK header includes."""
    roots = [Path(args.zephyr_base) / "include", Path(args.binary_dir) / "include/generated"]
    kit_incs = []
    for option, value in zip(incs[::2], incs[1::2], strict=True):
        if any(under(value, root) is not None for root in roots):
            kit_incs += [option, value]
    deps = dependencies(args, resolve(flags, roots[0], config_h) + kit_incs, config_h.parent)
    for dep in deps:
        if dep.resolve() == config_h.resolve():
            continue
        if under(dep, toolchain_root) is not None:
            continue
        rel = next((r for r in (under(dep, root) for root in roots) if r is not None), None)
        if rel is None:
            fail(f"{dep} is included by the module SDK header but is outside the kit")
        if FORBIDDEN_HEADERS.search(rel.as_posix()):
            fail(f"the module SDK header includes {rel.as_posix()}, which a module must not see")


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


def cmake_list(flags):
    return ";".join(f.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") for f in flags)


def write_cmake_target(out, facts, defines, link_flags):
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
    lines.append(f'set(APP_EDK_DEFINES "{cmake_list(defines)}")')
    lines.append(f'set(APP_EDK_LINK_FLAGS "{cmake_list(link_flags)}")')
    (out / "cmake/app_edk_target.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_facts(args, out, facts):
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
    fw["caps"] = "include/zephyr/include/zephyr/app_platform/app_caps.h"
    for key in ("board", "arch", "llext_type", "toolchain_variant", "triple", "compiler"):
        fw[key] = facts.get(key, "")
    fw["compiler_version"] = facts.get("compiler_version", "")
    fw["sdk_version"] = facts.get("sdk_version", "")
    text = "".join(f"{k}={v}\n" for k, v in fw.items())
    (out / "firmware.txt").write_text(text, encoding="utf-8")


def write_keys(args, out):
    fields = {"id", "state", "caps", "prefix", "pubkey"}
    keys = json.loads(Path(args.keys).read_text(encoding="utf-8"))["keys"]
    for key in keys:
        if set(key) - fields:
            fail(f"{args.keys} holds fields other than {', '.join(sorted(fields))}")
    text = json.dumps({"keys": keys}, indent=1) + "\n"
    (out / "keys.json").write_text(text, encoding="ascii")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument("--out-dir", required=True, help="directory of the generated files")
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
    parser.add_argument("--fact", action="append", default=[], help="key=value")
    args = parser.parse_args()

    toolchain_root = Path(args.cc).resolve().parent.parent
    facts = toolchain_facts(args)

    out = Path(args.out_dir)
    for sub in ("abi", "cmake"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    config_h = out / CONFIG_H
    config_h.write_text(config_header(args.autoconf), encoding="utf-8")
    flags, incs = module_flags(args)
    check_headers(args, flags, incs, config_h, toolchain_root)

    defines = [f for f in flags if isinstance(f, str) and f.startswith("-D")]
    write_cmake_target(out, facts, defines, args.link_flag)
    write_facts(args, out, facts)
    write_keys(args, out)
    shutil.copyfile(args.imports, out / "abi/app_imports.txt")


if __name__ == "__main__":
    main()
