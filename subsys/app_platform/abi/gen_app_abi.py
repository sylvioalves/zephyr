#!/usr/bin/env python3
#
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
#
# SPDX-License-Identifier: Apache-2.0

"""Generate the functions the firmware exports to application modules.

Reads the export table (app_abi.txt) and the service declarations in
app_services.h, checks that every service has an entry with a matching
signature, and writes:

- a header declaring every exported function and the interface major version,
  which includes no kernel header,
- a header of inline functions giving a module the familiar service names,
- a C file defining the service forwarders and exporting every function whose
  condition is enabled in the firmware.
"""

import argparse
import re
import sys
from dataclasses import dataclass

GROUP = "APP_ABI"


@dataclass
class Entry:
    name: str
    version: int
    kind: str
    cond: str
    ret: str
    params: list[tuple[str, str]]
    line: int

    @property
    def symbol(self):
        return f"app_abi_{self.name}_v{self.version}"

    def proto(self, fn):
        if self.params:
            args = ", ".join(join_decl(t, n) for t, n in self.params)
        else:
            args = "void"
        return f"{join_decl(self.ret, fn)}({args})"

    def call(self, fn):
        return f"{fn}({', '.join(n for _, n in self.params)})"


def join_decl(typ, name):
    return f"{typ}{name}" if typ.endswith("*") else f"{typ} {name}"


def norm_type(typ):
    typ = " ".join(typ.split())
    return re.sub(r"\s*\*\s*", " *", typ).replace(" *", "*").strip()


def fmt_type(typ):
    return re.sub(r"\s*(\*+)$", r" \1", norm_type(typ))


def split_params(text, where):
    text = " ".join(text.split())
    if text in ("", "void"):
        return []
    params = []
    for p in text.split(","):
        m = re.match(r"^(.*?)\s*\b([A-Za-z_]\w*)$", p.strip())
        if m is None or m.group(1) == "":
            sys.exit(f"{where}: cannot parse parameter '{p.strip()}'")
        params.append((fmt_type(m.group(1)), m.group(2)))
    return params


def parse_table(path):
    logical = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if line.strip() == "" or line.lstrip().startswith("#"):
                continue
            if line[0].isspace():
                if not logical:
                    sys.exit(f"{path}:{n}: continuation without an entry")
                logical[-1][1] += " " + line.strip()
            else:
                logical.append([n, line.strip()])

    major = None
    entries = []
    for n, text in logical:
        m = re.match(r"^major\s+(\d+)$", text)
        if m is not None:
            if major is not None:
                sys.exit(f"{path}:{n}: major listed twice")
            major = int(m.group(1))
            continue
        m = re.match(r"^(\w+)\s+(\d+)\s+(svc|fw)\s+(\S+)\s+(.+?)\((.*)\)$", text)
        if m is None:
            sys.exit(f"{path}:{n}: malformed entry")
        where = f"{path}:{n}"
        entries.append(
            Entry(
                name=m.group(1),
                version=int(m.group(2)),
                kind=m.group(3),
                cond=m.group(4),
                ret=fmt_type(m.group(5)),
                params=split_params(m.group(6), where),
                line=n,
            )
        )
    if major is None or major < 1:
        sys.exit(f"{path}: needs a 'major <n>' line, starting at 1")
    return major, entries


def parse_services(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    services = {}
    for m in re.finditer(r"__syscall\s+([^;]*?)\bapp_svc_(\w+)\s*\(([^;]*)\)\s*;", text, re.S):
        services[m.group(2)] = (fmt_type(m.group(1)), split_params(m.group(3), path))
    return services


def check(entries, services, table):
    seen = set()
    svc_names = set()
    for e in entries:
        where = f"{table}:{e.line}"
        if e.version < 1:
            sys.exit(f"{where}: version must start at 1")
        if (e.name, e.version) in seen:
            sys.exit(f"{where}: {e.symbol} listed twice")
        seen.add((e.name, e.version))
        if e.kind != "svc":
            continue
        if e.name in svc_names:
            sys.exit(f"{where}: more than one svc entry for {e.name}")
        svc_names.add(e.name)
        if e.name not in services:
            sys.exit(f"{where}: app_svc_{e.name} is not declared in app_services.h")
        ret, params = services[e.name]
        mine = (norm_type(e.ret), [norm_type(t) for t, _ in e.params])
        theirs = (norm_type(ret), [norm_type(t) for t, _ in params])
        if mine != theirs:
            sys.exit(
                f"{where}: {e.symbol} does not match app_svc_{e.name} in app_services.h; "
                "a changed service takes a new version"
            )
    missing = sorted(set(services) - svc_names)
    if missing:
        sys.exit(f"{table}: no entry for " + ", ".join(f"app_svc_{n}" for n in missing))


def module_name(entries, name):
    if any(e.kind == "svc" and e.name == name for e in entries):
        return f"app_svc_{name}"
    return f"app_{name}"


def latest(entries):
    best = {}
    for e in entries:
        if e.name not in best or e.version > best[e.name].version:
            best[e.name] = e
    return [best[n] for n in dict.fromkeys(e.name for e in entries)]


def cond_open(e):
    return "" if e.cond == "-" else f"#if defined(CONFIG_{e.cond})\n"


def cond_close(e):
    return "" if e.cond == "-" else "#endif\n"


HEADER = "/* auto-generated by gen_app_abi.py, do not edit */\n"


def write_decls(path, major, entries):
    out = [
        HEADER,
        "#ifndef ZEPHYR_INCLUDE_GENERATED_APP_ABI_H_",
        "#define ZEPHYR_INCLUDE_GENERATED_APP_ABI_H_",
        "",
        "#include <zephyr/app_platform/app_svc_types.h>",
        "",
        f"#define APP_ABI_MAJOR {major}",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    out += [f"{e.proto(e.symbol)};" for e in entries]
    out += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif", ""]
    write(path, "\n".join(out))


def write_forward(path, entries):
    out = [
        HEADER,
        "#ifndef ZEPHYR_INCLUDE_GENERATED_APP_ABI_FORWARD_H_",
        "#define ZEPHYR_INCLUDE_GENERATED_APP_ABI_FORWARD_H_",
        "",
        "#include <zephyr/app_platform/app_abi.h>",
        "",
    ]
    for e in latest(entries):
        body = e.call(e.symbol)
        body = f"\t{body};" if e.ret == "void" else f"\treturn {body};"
        out += [f"static inline {e.proto(module_name(entries, e.name))}", "{", body, "}", ""]
    out += ["#endif", ""]
    write(path, "\n".join(out))


def write_exports(path, entries):
    out = [
        HEADER,
        "#include <zephyr/app_platform/app_services.h>",
        "#include <zephyr/app_platform/app_abi.h>",
        "#include <zephyr/llext/symbol.h>",
        "",
    ]
    for e in entries:
        text = cond_open(e)
        if e.kind == "svc":
            body = e.call(f"app_svc_{e.name}")
            body = f"\t{body};" if e.ret == "void" else f"\treturn {body};"
            text += f"{e.proto(e.symbol)}\n{{\n{body}\n}}\n"
        text += f"EXPORT_GROUP_SYMBOL({GROUP}, {e.symbol});\n"
        text += cond_close(e)
        out.append(text)
    write(path, "\n".join(out))


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--table", required=True, help="export table")
    parser.add_argument("--services", required=True, help="app_services.h")
    parser.add_argument("--decls", required=True, help="output declaration header")
    parser.add_argument("--forward", required=True, help="output module forwarding header")
    parser.add_argument("--exports", required=True, help="output firmware C file")
    args = parser.parse_args()

    major, entries = parse_table(args.table)
    check(entries, parse_services(args.services), args.table)
    write_decls(args.decls, major, entries)
    write_forward(args.forward, entries)
    write_exports(args.exports, entries)


if __name__ == "__main__":
    main()
