#!/usr/bin/env python3
"""
Mirror the Windows App SDK DirectWrite headers into include/ for DWriteCore.

Rules (see README):
  * The mirrored headers stay byte-faithful to the originals apart from the
    substitutions below, so diffing against the SDK stays meaningful.
  * All Windows-only scaffolding is redirected to compat.h.
  * Declarations for entry points the real libdwritecore.so does not export
    are removed.

All six DirectWrite headers are mirrored. Four of them - dwrite.h, dwrite_1.h,
dwrite_2.h and DWriteExperimental.h - end up differing from the SDK originals by
exactly five lines: the appended IID table, and the three that bracket the file
in NOLINTBEGIN/NOLINTEND. The other two differ by that plus their prunings
below, and nothing else. Diffing include/ against third_party/windows-app-sdk/
shows the whole delta.

The NOLINT bracket keeps clang-tidy and clangd off vendor text. It goes around
the file rather than around each construct, so no original line is touched, and
it is deliberately not a substitute for that diff: drift from the SDK is caught
by diffing, never by static analysis noticing Microsoft's house style.

Also emits dwrite_iids.h, associating each mirrored interface with its IID.
"""

import os
import re
import sys
import uuid

from paths import DWRITE_SDK as SRC, INCLUDE as DST

# Flat entry points declared by the SDK headers that libdwritecore.so does NOT
# export. Verified with `nm -D --defined-only libdwritecore.so`, which lists
# exactly two symbols: DWriteCoreCreateFactory and DWriteCoreSetFeatureStagingCallback.
UNEXPORTED = [
    "DWriteCoreRegisterEventSink",
    "DWriteCoreUnregisterEventSink",
]

# Interfaces the shipped libdwritecore.so shows no evidence of implementing.
# tools/analyze_interfaces.py classifies all 104 public interfaces against the
# binary; these are the only two with no QueryInterface registry entry, no
# outbound IID constant, no Rust implementation module, and no role as a
# caller-implemented callback.
#
# Their declarations are replaced with a forward declaration, never deleted
# outright: IDWriteAsyncResult is still named in the signature of a method on
# another interface, and removing a *method* would shift every vtable slot
# after it.
PRUNE_INTERFACES = {
    "dwrite_3.h": ["IDWriteAsyncResult", "IDWriteFontFallback1"],
}


def resolve_prunes():
    """
    Drop any prune request for an interface that a *kept* interface derives
    from, and report it.

    Absence of an IID means the interface cannot be reached by
    QueryInterface - not that it is unimplemented. IDWriteAsyncResult has no
    IID anywhere in the binary, but DWriteExperimental.h's IDWriteAsyncResult1
    derives from it and is in the QI registry, so its methods necessarily
    occupy the leading slots of that vtable. The base is implemented; only the
    older IID is unreachable. Pruning it would also leave IDWriteAsyncResult1
    with an incomplete base type.
    """
    bases = {}
    for fname in sorted(os.listdir(SRC)):
        if not fname.endswith(".h"):
            continue
        text = open(os.path.join(SRC, fname), encoding="utf-8",
                    errors="surrogateescape").read()
        for m in IFACE.finditer(text):
            bases[m.group(1)] = m.group(3)

    requested = {i for v in PRUNE_INTERFACES.values() for i in v}
    protected = {base for name, base in bases.items()
                 if name not in requested and base in requested}

    for name in sorted(protected):
        derived = sorted(n for n, b in bases.items()
                         if b == name and n not in requested)
        print("  keeping %s: %s derives from it" % (name, ", ".join(derived)))

    return {h: [i for i in v if i not in protected]
            for h, v in PRUNE_INTERFACES.items()}

PLATFORM_INCLUDES = re.compile(
    r'#include <specstrings\.h>\r?\n#include <unknwn\.h>\r?\n#include <dcommon\.h>\r?\n')

IFACE = re.compile(
    r'DWRITE_BEGIN_INTERFACE\(\s*(\w+)\s*,\s*"([0-9a-fA-F-]{36})"\s*\)\s*:\s*(\w+)')

# Per-header IID tables. Each is included at the tail of its own header so that
# every interface is declared before its traits specialization appears.
IID_HEADER_FOR = {
    "dwrite.h":      "dwrite_iids.h",
    "dwrite_1.h":    "dwrite_1_iids.h",
    "dwrite_2.h":    "dwrite_2_iids.h",
    "dwrite_3.h":    "dwrite_3_iids.h",
    "dwrite_core.h": "dwrite_core_iids.h",
    "DWriteExperimental.h": "dwrite_experimental_iids.h",
}
FINAL_ENDIF_FOR = {
    "dwrite.h":      "#endif /* DWRITE_H_INCLUDED */",
    "dwrite_1.h":    "#endif /* DWRITE_1_H_INCLUDED */",
    "dwrite_2.h":    "#endif /* DWRITE_2_H_INCLUDED */",
    "dwrite_3.h":    "#endif // DWRITE_3_H_INCLUDED",
    "dwrite_core.h": "#endif // DWRITE_CORE_H_INCLUDED",
    "DWriteExperimental.h": "#endif",
}


def strip_decl(text, funcname):
    """Remove an `EXTERN_C ... funcname( ... );` declaration and its doc comment."""
    m = re.search(r'^EXTERN_C[^;]*?\b' + funcname + r'\b[^;]*?;\s*$',
                  text, re.M | re.S)
    if not m:
        return text, False
    start = m.start()
    # Walk backwards over the preceding /// documentation block.
    lines = text[:start].split("\n")
    i = len(lines) - 1
    while i > 0 and (lines[i].strip().startswith("///") or lines[i].strip() == ""):
        i -= 1
    start = len("\n".join(lines[:i + 1])) + 1
    nl = "\r\n" if "\r\n" in text else "\n"
    note = ("// NOTE: %s is declared by the Windows App SDK but is NOT exported by%s"
            "// libdwritecore.so, so it is omitted from this mirror.%s" % (funcname, nl, nl))
    return text[:start] + note + text[m.end():].lstrip("\n"), True


def prune_interface(text, iface):
    """Replace an interface declaration with a forward declaration."""
    m = re.search(r'DWRITE_BEGIN_INTERFACE\(\s*' + iface +
                  r'\s*,\s*"[0-9a-fA-F-]{36}"\s*\)\s*:\s*\w+\s*\r?\n\{', text)
    if not m:
        return text, False

    # Brace-match through to the closing "};".
    i = text.index("{", m.start())
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = text.find(";", i) + 1
                break
        i += 1
    else:
        return text, False

    # Absorb the preceding /// documentation block.
    start = m.start()
    lines = text[:start].split("\n")
    j = len(lines) - 1
    while j > 0 and (lines[j].strip().startswith("///") or lines[j].strip() == ""):
        j -= 1
    start = len("\n".join(lines[:j + 1])) + 1

    nl = "\r\n" if "\r\n" in text else "\n"
    note = ("// NOTE: %s is declared by the Windows App SDK but the shipped"
            " libdwritecore.so%s"
            "// shows no evidence of implementing it, so only a forward"
            " declaration is kept%s"
            "// here (it is still named in another interface's signature)."
            " See tools/analyze_interfaces.py.%s"
            "interface %s;%s" % (iface, nl, nl, nl, iface, nl))
    return text[:start] + note + text[end:].lstrip("\r\n"), True


def mirror(name, prunes, extra=None):
    src = os.path.join(SRC, name)
    text = open(src, encoding="utf-8", errors="surrogateescape", newline="").read()

    # The platform includes are left exactly as the SDK wrote them:
    # tools/mirror_sdk_headers.py mirrors specstrings.h, unknwn.h and
    # dcommon.h into include/, and each of those pulls in compat.h.

    removed = []
    for fn in UNEXPORTED:
        text, hit = strip_decl(text, fn)
        if hit:
            removed.append(fn)
    if removed:
        print("  %-16s pruned unexported: %s" % (name, ", ".join(removed)))

    pruned = []
    for iface in prunes.get(name, []):
        text, hit = prune_interface(text, iface)
        if hit:
            pruned.append(iface)
    if pruned:
        print("  %-16s pruned unevidenced interfaces: %s" % (name, ", ".join(pruned)))

    if extra:
        text = extra(text, name)

    # Each mirrored header pulls in its own IID table, at the very end so every
    # interface it declares is already visible to the traits specializations.
    iid_header = IID_HEADER_FOR[name]
    guard = FINAL_ENDIF_FOR[name]
    at = text.rfind(guard)
    assert at != -1, "could not locate final #endif in " + name
    # Anchored to the *last* occurrence: DWriteExperimental.h closes with a
    # bare "#endif" rather than a named guard.
    text = text[:at] + '#include "%s"\r\n\r\n' % iid_header + text[at:]

    # Brackets the whole file, so Microsoft's own lines stay byte-for-byte what
    # the SDK ships. The IID table lives in its own header and is this project's
    # work, so it stays outside the bracket and keeps getting analyzed.
    text = ("// Mirrored Windows SDK text - generated by tools/mirror_headers.py.\r\n"
            "// NOLINTBEGIN\r\n" + text + "// NOLINTEND\r\n")

    out = os.path.join(DST, name)
    open(out, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    return text


def emit_iids(name, text):
    """Emit DWRITE_DEFINE_IID entries for the interfaces declared in one header."""
    entries = []
    for m in IFACE.finditer(text):
        iface, iid = m.group(1), m.group(2)
        u = uuid.UUID(iid)
        entries.append((iface, u.time_low, u.time_mid, u.time_hi_version, u.bytes[8:]))

    out_name = IID_HEADER_FOR[name]
    tag = out_name.replace(".", "_").upper() + "_INCLUDED"
    lines = [
        "//+--------------------------------------------------------------------------",
        "//",
        "//  %s - IID associations for the interfaces in %s." % (out_name, name),
        "//",
        "//  GENERATED by tools/mirror_headers.py - do not edit by hand.",
        "//",
        "//  DWriteCore's QueryInterface parses these IIDs from ASCII string literals",
        "//  at runtime (confirmed in libdwritecore.so at 0x251a00), so the values",
        "//  below were cross-checked against the string table in the binary.",
        "//",
        "//----------------------------------------------------------------------------",
        "",
        "#ifndef " + tag,
        "#define " + tag,
        "#pragma once",
        "",
        '#include "compat.h"',
        "",
        "#ifdef __cplusplus",
        "",
    ]
    for iface, d1, d2, d3, b in sorted(entries):
        bs = ", ".join("0x%02X" % x for x in b)
        lines.append("DWRITE_DEFINE_IID(%s, 0x%08X, 0x%04X, 0x%04X, %s)"
                     % (iface, d1, d2, d3, bs))
    lines += ["", "#endif // __cplusplus", "",
              "#endif // " + tag, ""]

    open(os.path.join(DST, out_name), "w").write("\n".join(lines))
    print("  %-16s %d interface IIDs" % (out_name, len(entries)))
    return entries


def main():
    os.makedirs(DST, exist_ok=True)
    print("Mirroring DirectWrite headers -> include/")
    total = 0
    prunes = resolve_prunes()
    for name in ("dwrite.h", "dwrite_1.h", "dwrite_2.h", "dwrite_3.h",
                 "dwrite_core.h", "DWriteExperimental.h"):
        total += len(emit_iids(name, mirror(name, prunes)))
    print("done. %d interfaces mirrored." % total)


if __name__ == "__main__":
    main()
