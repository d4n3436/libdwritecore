#!/usr/bin/env python3
"""
Determine which DirectWrite interfaces the shipped libdwritecore.so supports.

The library is Rust: 248 distinct rust/.../*.rs source paths are embedded in it,
covering shaping, OpenType layout, TrueType interpretation and rasterization -
the ClearType pipeline is rust/glyph_rendering_subpixel/src/cleartype.rs and the
COM surface is rust/api_impl/. It links only bionic libc/libm/libdl/liblog, with
no libc++, so the COM layer is Rust `extern "system"` vtables rather than C++
classes.

DWriteCore's QueryInterface does not compare against GUID constants in .rodata.
It parses an ASCII GUID *string literal* at runtime and compares the four
resulting dwords. The factory's QI at 0x251a00 reads:

    FUN_00252070(local_30, "b859ee5a-d838-4b5b-a2e8-1adc7d93db48");   // IDWriteFactory
    if (iid matches) { ... } else { chain to the next version ... }

So the set of ASCII GUID strings in the binary is the inbound QI registry.
Separately, a handful of IIDs appear as raw 16-byte little-endian constants;
those are the ones DWriteCore uses for *outbound* QI on caller-supplied objects
(for example asking an app's IDWriteTextRenderer for IDWriteTextRenderer1).

Neither signal covers everything. Interfaces the app implements (callbacks) and
objects handed back directly by factory create methods need no inbound QI entry,
so absence from the registry is not absence from the API. Those are classified
separately below.

Usage:  python3 tools/analyze_interfaces.py [--json tools/interface_support.json]

The checked-in answer is tools/interface_support.json, beside this tool and
beside mirror_headers.py, whose PRUNE_INTERFACES is what it is evidence for.
"""

import glob
import hashlib
import json
import os
import re
import sys
import uuid

from paths import DWRITE_SDK as SDK, BINARY, ROOT

IFACE_RE = re.compile(
    r'DWRITE_BEGIN_INTERFACE\(\s*(\w+)\s*,\s*"([0-9a-fA-F-]{36})"\s*\)\s*:\s*(\w+)')

# Interfaces the *application* implements and DWriteCore calls. DWriteCore never
# QIs these inbound, so they carry no registry entry, but they are part of the API.
CALLBACK_INTERFACES = {
    "IDWriteFontCollectionLoader", "IDWriteFontFileEnumerator",
    "IDWriteFontFileLoader", "IDWriteFontFileStream",
    "IDWriteTextAnalysisSource", "IDWriteTextAnalysisSink",
    "IDWriteTextAnalysisSource1", "IDWriteTextAnalysisSink1",
    "IDWritePixelSnapping", "IDWriteTextRenderer", "IDWriteTextRenderer1",
    "IDWriteInlineObject", "IDWriteFontDownloadListener",
    "IDWriteFontFileLoader1", "IDWriteEventSink",
}

# Objects returned directly by create methods, implemented by the Rust api_impl /
# layout / font_fallback crates. Confirmed by source paths embedded in the binary.
RUST_IMPLEMENTED = {
    "IDWriteTextFormat": "rust/api_impl/src/text_format.rs",
    "IDWriteTextFormat1": "rust/api_impl/src/text_format.rs",
    "IDWriteTextFormat2": "rust/api_impl/src/text_format.rs",
    "IDWriteTextFormat3": "rust/api_impl/src/text_format.rs",
    "IDWriteTextLayout": "rust/api_impl/src/text_layout.rs",
    "IDWriteTextLayout1": "rust/api_impl/src/text_layout.rs",
    "IDWriteTextLayout2": "rust/api_impl/src/text_layout.rs",
    "IDWriteTextLayout3": "rust/api_impl/src/text_layout.rs",
    "IDWriteTextLayout4": "rust/api_impl/src/text_layout.rs",
    "IDWriteTextAnalyzer": "rust/api_impl/src/text_analyzer.rs",
    "IDWriteTextAnalyzer1": "rust/api_impl/src/text_analyzer.rs",
    "IDWriteTextAnalyzer2": "rust/api_impl/src/text_analyzer.rs",
    "IDWriteFontFallbackBuilder": "rust/font_fallback/src/builder.rs",
    "IDWriteTypography": "rust/layout/src/properties.rs",
    "IDWriteNumberSubstitution": "rust/unicode_analysis/src/number_substitution.rs",
}


def load_interfaces():
    ifaces, order = {}, []
    for h in sorted(glob.glob(os.path.join(SDK, "*.h"))):
        text = open(h, encoding="utf-8", errors="replace").read()
        for m in IFACE_RE.finditer(text):
            name = m.group(1)
            if name not in ifaces:
                ifaces[name] = dict(iid=m.group(2).lower(), base=m.group(3),
                                    header=os.path.basename(h))
                order.append(name)
    return ifaces, order


def classify(ifaces, order, data):
    low = data.lower()
    out = {}
    for name in order:
        info = dict(ifaces[name])
        u = uuid.UUID(info["iid"])
        evidence = []
        if info["iid"].encode() in low:
            evidence.append("qi-registry")       # inbound QueryInterface
        if u.bytes_le in data:
            evidence.append("outbound-const")    # QI performed *by* DWriteCore
        if name in CALLBACK_INTERFACES:
            evidence.append("app-implemented")
        if name in RUST_IMPLEMENTED:
            evidence.append("rust-impl:" + RUST_IMPLEMENTED[name])
        info["evidence"] = evidence
        out[name] = info

    # An interface that a supported interface derives from is implemented too:
    # its methods occupy the leading slots of the derived vtable. Absence of an
    # IID only means QueryInterface cannot reach it by that IID.
    # IDWriteAsyncResult is the real case - DWriteExperimental.h's
    # IDWriteAsyncResult1 is in the QI registry and derives from it.
    for name, info in out.items():
        if info["evidence"]:
            base = info["base"]
            if base in out and not out[base]["evidence"]:
                out[base]["evidence"].append("base-of:" + name)

    for info in out.values():
        info["supported"] = bool(info["evidence"])
    return out


def main():
    ifaces, order = load_interfaces()
    data = open(BINARY, "rb").read()
    result = classify(ifaces, order, data)

    by_header = {}
    for name in order:
        by_header.setdefault(result[name]["header"], []).append(name)

    print("DWriteCore interface support  (source: %s)\n" % os.path.basename(BINARY))
    total = supported = 0
    for hdr in ["dwrite.h", "dwrite_1.h", "dwrite_2.h", "dwrite_3.h",
                "dwrite_core.h", "DWriteExperimental.h"]:
        names = by_header.get(hdr, [])
        if not names:
            continue
        ok = [n for n in names if result[n]["supported"]]
        missing = [n for n in names if not result[n]["supported"]]
        total += len(names)
        supported += len(ok)
        print("%-22s %3d/%-3d supported" % (hdr, len(ok), len(names)))
        if missing:
            print("    not evidenced: %s" % ", ".join(missing))
    print("\nTOTAL %d/%d interfaces evidenced in the binary." % (supported, total))

    if "--json" in sys.argv:
        path = sys.argv[sys.argv.index("--json") + 1]
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        # JSON cannot carry the banner comment every other generated file in
        # this repository has, so the provenance goes in as data. Underscored
        # so it sorts ahead of the interface names and cannot collide with
        # one. `_source` names the binary the answers were read out of: the
        # classification means nothing without it.
        result["_generated_by"] = "tools/analyze_interfaces.py"
        result["_source"] = os.path.relpath(BINARY, ROOT)
        result["_source_sha256"] = hashlib.sha256(open(BINARY, "rb").read()).hexdigest()
        json.dump(result, open(path, "w"), indent=1, sort_keys=True)
        print("wrote %s" % path)


if __name__ == "__main__":
    main()
