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
  condition is enabled in the firmware,
- the sorted list of symbols a module may import, every exported function and
  the entries of the import table (app_imports.txt) for the architecture, as C
  string literals for the loader and as a text file for the signer,
- a C file asserting the layouts and values recorded in the lock file
  (app_abi.lock) against the compiler.

Before writing anything it compares the interface, the exported functions, the
types, constants and structure layouts of app_svc_types.h and app_caps.h and
the module entry point, with the lock file and fails on a recorded entry that
changed or disappeared, or on a new entry the lock does not record yet. With
--update-lock it appends the new entries to the lock instead, or rewrites the
lock after a major version change, and writes nothing else.
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass

GROUP = "APP_ABI"
IMPORT_NAME_MAX = 64


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


def parse_imports(path, arch):
    names = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if line == "":
                continue
            parts = line.split()
            if len(parts) != 2:
                sys.exit(f"{path}:{n}: malformed entry")
            if parts[1] == "-" or arch in parts[1].split(","):
                names.append(parts[0])
    return names


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


def write_imports(inc_path, txt_path, entries, imports):
    names = sorted(set([e.symbol for e in entries] + imports))
    for name in names:
        if len(name) >= IMPORT_NAME_MAX:
            sys.exit(f"{name}: an importable symbol name must be shorter than {IMPORT_NAME_MAX}")
    write(inc_path, HEADER + "".join(f'"{name}",\n' for name in names))
    write(txt_path, "".join(f"{name}\n" for name in names))


LOCK_HEADER = """\
# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# The interface loadable application modules are built against, as released.
#
# Every build compares the interface with this file: the functions of app_abi.txt, the types and
# constants of app_svc_types.h and app_caps.h, and the module entry point of app_sdk.h, and
# asserts the recorded layouts and values against the compiler. A recorded line never changes and
# is never removed while the major version stays the same. A new entry is appended, by hand or with
# gen_app_abi.py --update-lock, which appends nothing but new entries. Raising the major version
# in app_abi.txt lets --update-lock rewrite the whole file.
#
# func <name> <version> <kind> <return type> (<parameter types>)
# type <typedef> <type>
# entry <symbol> <return type> (<parameter types>)
# const <macro> <value>
# enum <enum>.<enumerator> <value>
# struct <struct> size <bytes>
# field <struct>.<field> offset <bytes> size <bytes>
"""

LOCK_COLUMNS = 100

# Macros that follow the capability set as it grows rather than name a fixed value.
LOCK_UNSTABLE = {"APP_CAP_LAST", "APP_CAP_ALL"}

SCALARS = {
    "bool": 1,
    "char": 1,
    "int8_t": 1,
    "uint8_t": 1,
    "int16_t": 2,
    "uint16_t": 2,
    "int32_t": 4,
    "uint32_t": 4,
    "int64_t": 8,
    "uint64_t": 8,
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def c_body(text):
    """Return the C text of a header without comments and preprocessor lines."""
    lines = strip_comments(text).replace("\\\n", " ").splitlines()
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


def eval_const(expr, known, where):
    expr = re.sub(r"\((?:app_caps_t|u?int\d+_t)\)", "", expr)
    expr = re.sub(r"\b(?:APP_CAP_BIT|BIT)\s*\(", "(1 << ", expr)
    expr = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr)

    def name(m):
        if m.group(0) not in known:
            sys.exit(f"{where}: cannot evaluate '{m.group(0)}'")
        return str(known[m.group(0)])

    expr = re.sub(r"\b[A-Za-z_]\w*\b", name, expr)
    if re.fullmatch(r"[0-9a-fA-FxX\s()<>|+\-*~]*", expr) is None:
        sys.exit(f"{where}: cannot evaluate '{expr}'")
    return eval(expr, {"__builtins__": {}})


def parse_consts(paths):
    consts = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            text = strip_comments(f.read())
        for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(APP_\w+)[ \t]+(.+)$", text, re.M):
            name, value = m.group(1), m.group(2).strip()
            consts[name] = eval_const(value, consts, f"{path}: {name}")
    return {n: v for n, v in consts.items() if n not in LOCK_UNSTABLE}


def parse_enums(path, known):
    with open(path, encoding="utf-8") as f:
        body = c_body(f.read())
    enums = []
    for m in re.finditer(r"\benum\s+(\w+)\s*\{(.*?)\}\s*;", body, re.S):
        value = -1
        for item in m.group(2).split(","):
            item = item.strip()
            if item == "":
                continue
            name, _, expr = (part.strip() for part in item.partition("="))
            where = f"{path}: {name}"
            value = eval_const(expr, known, where) if expr else value + 1
            known[name] = value
            enums.append((m.group(1), name, value))
    return enums


def parse_typedefs(paths):
    types = []
    for path in paths:
        with open(path, encoding="utf-8") as f:
            body = c_body(f.read())
        for m in re.finditer(r"\btypedef\s+([^;{}]*?)\s*;", body):
            decl = " ".join(m.group(1).split())
            fn = re.fullmatch(r"(.+?)\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)", decl)
            if fn is not None:
                args = [t for t, _ in split_params(fn.group(3), path)] or ["void"]
                types.append((fn.group(2), f"{fmt_type(fn.group(1))} (*)({', '.join(args)})"))
                continue
            plain = re.fullmatch(r"(.+?)\s*\b(\w+)", decl)
            if plain is None:
                sys.exit(f"{path}: cannot parse 'typedef {decl}'")
            types.append((plain.group(2), fmt_type(plain.group(1))))
    return types


def parse_entry(path):
    with open(path, encoding="utf-8") as f:
        body = c_body(f.read())
    m = re.search(r"^\s*([\w\s*]+?)\bapp_main\s*\(([^)]*)\)\s*;", body, re.M)
    if m is None:
        sys.exit(f"{path}: app_main is not declared")
    return fmt_type(m.group(1)), [t for t, _ in split_params(m.group(2), path)]


def struct_layout(name, fields, structs, align64, where):
    offset = 0
    align = 1
    out = []
    for typ, field, dims in fields:
        if typ in SCALARS:
            size = SCALARS[typ]
            falign = align64 if size == 8 else size
        elif typ.startswith("struct ") and typ[7:] in structs:
            inner = structs[typ[7:]]
            size, falign = inner[align64]
        else:
            sys.exit(
                f"{where}: field {name}.{field} has type '{typ}'; a structure shared with "
                "modules uses fixed-width integer, char and bool fields and other such structures"
            )
        count = 1
        for dim in dims:
            count *= dim
        offset = (offset + falign - 1) // falign * falign
        out.append((field, offset, size * count))
        offset += size * count
        align = max(align, falign)
    return (offset + align - 1) // align * align, align, out


def parse_structs(path, consts):
    with open(path, encoding="utf-8") as f:
        body = c_body(f.read())
    structs = {}
    result = []
    for m in re.finditer(r"\b(struct|union)\s+(\w+)\s*\{(.*?)\}\s*;", body, re.S):
        kind, name = m.group(1), m.group(2)
        where = f"{path}: {kind} {name}"
        if kind == "union":
            sys.exit(f"{where}: a union shared with modules is not supported")
        fields = []
        for decl in m.group(3).split(";"):
            decl = " ".join(decl.split())
            if decl == "":
                continue
            dm = re.fullmatch(
                r"(?:volatile\s+)?(?:const\s+)?(.+?)\s*\b(\w+)((?:\s*\[[^\]]+\])*)", decl
            )
            if dm is None:
                sys.exit(f"{where}: cannot parse field '{decl}'")
            dims = [
                eval_const(d, consts, f"{where}.{dm.group(2)}")
                for d in re.findall(r"\[([^\]]+)\]", dm.group(3))
            ]
            fields.append((norm_type(dm.group(1)), dm.group(2), dims))
        layouts = {a: struct_layout(name, fields, structs, a, where) for a in (4, 8)}
        if layouts[4][0] != layouts[8][0] or layouts[4][2] != layouts[8][2]:
            sys.exit(
                f"{where}: the layout depends on the alignment of 64-bit fields; "
                "place them at offsets that are multiples of 8 with explicit reserved fields"
            )
        structs[name] = {a: (layouts[a][0], layouts[a][1]) for a in (4, 8)}
        result.append((name, layouts[8][0], layouts[8][2]))
    return result


def sig_text(ret, params):
    return f"{ret} ({', '.join(params) or 'void'})"


def lock_lines(major, entries, types_h, caps_h, sdk_h):
    consts = parse_consts([caps_h, types_h])
    known = dict(consts)
    enums = parse_enums(types_h, known)
    lines = [f"major {major}"]
    for e in entries:
        lines.append(
            f"func {e.name} {e.version} {e.kind} {sig_text(e.ret, [t for t, _ in e.params])}"
        )
    for name, typ in parse_typedefs([caps_h, types_h]):
        lines.append(f"type {name} {typ}")
    ret, params = parse_entry(sdk_h)
    lines.append(f"entry app_main {sig_text(ret, params)}")
    for name, value in consts.items():
        lines.append(f"const {name} {value:#x}")
    for enum, name, value in enums:
        lines.append(f"enum {enum}.{name} {value:#x}")
    structs = parse_structs(types_h, consts)
    names = {name for name, _, _ in structs}
    for e in entries:
        for typ in [e.ret] + [t for t, _ in e.params]:
            for used in re.findall(r"\bstruct\s+(\w+)", typ):
                if used not in names:
                    sys.exit(f"{e.symbol} passes struct {used}, which is not defined in {types_h}")
    for name, size, fields in structs:
        lines.append(f"struct {name} size {size}")
        for field, offset, fsize in fields:
            lines.append(f"field {name}.{field} offset {offset} size {fsize}")
    return lines


def lock_key(line):
    parts = line.split()
    return tuple(parts[:3]) if parts[0] == "func" else tuple(parts[:2])


def read_lock(path):
    lines = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = " ".join(raw.split())
            if line == "" or line.startswith("#"):
                continue
            if raw[0].isspace() and lines:
                lines[-1] = (lines[-1][0], f"{lines[-1][1]} {line}")
            else:
                lines.append((n, line))
    return lines


def lock_text(lines):
    out = []
    for line in lines:
        indent = " " * (line.find("(") + 1)
        words = line.split(", ")
        text = words[0]
        for word in words[1:]:
            if len(text) + 2 + len(word) > LOCK_COLUMNS:
                out.append(text + ",")
                text = indent + word
            else:
                text = f"{text}, {word}"
        out.append(text)
    return "".join(f"{text}\n" for text in out)


def lock_changed(old, new):
    """Return whether new differs from the recorded old line in a way that breaks modules."""
    if old == new:
        return False
    o, n = old.split(), new.split()
    # A superseded service stays exported as a function of the firmware.
    return not (o[0] == "func" and o[3] == "svc" and n[3] == "fw" and o[4:] == n[4:])


def check_lock(path, current, update):
    lock_hint = f"{sys.argv[0]} --update-lock, or west build -t app_platform_abi_lock"
    try:
        recorded = read_lock(path)
    except FileNotFoundError:
        recorded = []
    if not recorded and not update:
        sys.exit(f"{path}: missing; create it with {lock_hint}")

    old_major = next((ln for n, ln in recorded if ln.startswith("major ")), None)
    if recorded and old_major != current[0]:
        if not update:
            sys.exit(
                f"{path}: records '{old_major}' and app_abi.txt '{current[0]}'. "
                f"Rewrite the lock for the new major version with {lock_hint}."
            )
        if old_major is not None and int(old_major.split()[1]) > int(current[0].split()[1]):
            sys.exit(f"{path}: the major version may only be raised")
    if not recorded or old_major != current[0]:
        write(path, LOCK_HEADER + "\n" + lock_text(current))
        print(f"{path}: written for {current[0]}")
        return

    now = {lock_key(line): line for line in current}
    errors = []
    for n, line in recorded:
        key = lock_key(line)
        if key not in now:
            errors.append(f"{path}:{n}: '{line}' is gone")
        elif lock_changed(line, now[key]):
            errors.append(f"{path}:{n}: '{line}' is now '{now[key]}'")
    if errors:
        sys.exit(
            "\n".join(errors) + "\nA released interface entry never changes or disappears within a "
            "major version (doc/services/app_platform/policy.rst): give a changed function a new "
            "version, a changed structure a new name, or raise the major version in app_abi.txt "
            f"and rewrite the lock with {lock_hint}."
        )

    keys = {lock_key(line) for _, line in recorded}
    added = [line for line in current if lock_key(line) not in keys]
    if not added:
        return
    if update:
        with open(path, "a", encoding="utf-8") as f:
            f.write(lock_text(added))
        print(f"{path}: appended {len(added)} entries")
        return
    sys.exit(
        f"{path}: the interface has entries the lock does not record. Append these lines to it, "
        f"or run {lock_hint}, which appends them:\n" + "\n".join(added)
    )


def write_lock_asserts(path, lock_path, recorded):
    name = os.path.basename(lock_path)
    out = [
        HEADER,
        "#include <stddef.h>",
        "",
        "#include <zephyr/app_platform/app_sdk.h>",
        "#include <zephyr/toolchain.h>",
        "",
    ]
    for n, line in recorded:
        parts = line.split()
        where = f'"{name}:{n}: {line} does not match the compiler"'
        rest = " ".join(parts[2:])
        if parts[0] == "type":
            test = f"__builtin_types_compatible_p({parts[1]}, {rest})"
        elif parts[0] == "entry":
            ret, args = re.fullmatch(r"(.+?) \((.*)\)", rest).groups()
            test = f"__builtin_types_compatible_p(__typeof__(&{parts[1]}), {ret} (*)({args}))"
        elif parts[0] == "const":
            test = f"({parts[1]}) == {parts[2]}ULL"
        elif parts[0] == "enum":
            test = f"({parts[1].split('.', 1)[1]}) == {parts[2]}"
        elif parts[0] == "struct":
            test = f"sizeof(struct {parts[1]}) == {parts[3]}U"
        elif parts[0] == "field":
            st, field = parts[1].split(".", 1)
            test = (
                f"offsetof(struct {st}, {field}) == {parts[3]}U && "
                f"sizeof(((struct {st} *)0)->{field}) == {parts[5]}U"
            )
        else:
            continue
        if parts[0] in ("type", "entry"):
            out.append("#if defined(__GNUC__)")
            out.append(f"BUILD_ASSERT({test},\n\t     {where});")
            out.append("#endif")
        else:
            out.append(f"BUILD_ASSERT({test},\n\t     {where});")
    out.append("")
    write(path, "\n".join(out))


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    inc = os.path.join(here, "..", "..", "..", "include", "zephyr", "app_platform")
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--table", default=os.path.join(here, "app_abi.txt"), help="export table")
    parser.add_argument(
        "--services", default=os.path.join(inc, "app_services.h"), help="app_services.h"
    )
    parser.add_argument(
        "--types", default=os.path.join(inc, "app_svc_types.h"), help="app_svc_types.h"
    )
    parser.add_argument("--caps", default=os.path.join(inc, "app_caps.h"), help="app_caps.h")
    parser.add_argument("--sdk", default=os.path.join(inc, "app_sdk.h"), help="app_sdk.h")
    parser.add_argument("--lock", default=os.path.join(here, "app_abi.lock"), help="lock file")
    parser.add_argument(
        "--update-lock",
        action="store_true",
        help="append new entries to the lock, or rewrite it after a major version change",
    )
    parser.add_argument("--decls", help="output declaration header")
    parser.add_argument("--forward", help="output module forwarding header")
    parser.add_argument("--exports", help="output firmware C file")
    parser.add_argument("--lock-asserts", help="output C file asserting the lock")
    parser.add_argument(
        "--imports", default=os.path.join(here, "app_imports.txt"), help="import table"
    )
    parser.add_argument("--arch", help="architecture the firmware is built for")
    parser.add_argument("--allowed-inc", help="output loader import list")
    parser.add_argument("--allowed-txt", help="output signer import list")
    args = parser.parse_args()

    major, entries = parse_table(args.table)
    check(entries, parse_services(args.services), args.table)
    check_lock(
        args.lock, lock_lines(major, entries, args.types, args.caps, args.sdk), args.update_lock
    )
    if args.update_lock or args.decls is None:
        return
    write_decls(args.decls, major, entries)
    write_forward(args.forward, entries)
    write_exports(args.exports, entries)
    write_lock_asserts(args.lock_asserts, args.lock, read_lock(args.lock))
    imports = parse_imports(args.imports, args.arch)
    write_imports(args.allowed_inc, args.allowed_txt, entries, imports)


if __name__ == "__main__":
    main()
