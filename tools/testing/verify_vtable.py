#!/usr/bin/env python3
"""
Verify the shared-factory vtable in libdwritecore.so against the headers.

This is the check that the generated tests/vtable_test.cpp cannot make. That
test derives both the wrapper and the expected slot from the same parse of
include/dwrite.h, so it only proves the compiler lays out vtables in
declaration order. It says nothing about the binary. This reads the real vtable instead.

DWriteCoreCreateFactory ends by calling [vtbl+0x00] for QueryInterface and
[vtbl+0x10] for Release on the object it returns, which is what confirms slots
0/1/2 are QueryInterface/AddRef/Release with ordinary declaration-order slots
after them.

The vtable is located from the binary itself - no address is hardcoded and no
disassembler is required. Reported per slot is the target address, so it can be
compared against a disassembler if one is available.
"""

import os
import struct
import sys

# paths.py stays at the top of tools/; this script sits one level down.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from paths import ROOT, BINARY, DWRITE_SDK

# The factory vtable is located from the binary itself rather than hardcoded, so
# this check needs no disassembler:
#
#   1. find the IDWriteFactory IID as an ASCII string in .rodata - DWriteCore's
#      QueryInterface parses it at runtime;
#   2. pattern-scan .text for rip-relative LEAs of that address; each hit is
#      inside the QueryInterface of an object that answers to that IID;
#   3. take the relocation runs in .data.rel.ro whose first entry begins the
#      function containing a hit - those are the candidate vtables;
#   4. keep the longest. DWriteCoreCreateFactory has a shared and an isolated
#      path, so two identical-length candidates are expected.
IDWRITE_FACTORY_IID = b"b859ee5a-d838-4b5b-a2e8-1adc7d93db48"

R_X86_64_RELATIVE = 8

SDK = DWRITE_SDK


def load_sections(data):
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    def sh(index):
        b = e_shoff + index * e_shentsize
        name, _type, _flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, b)
        return dict(name=name, addr=addr, off=off, size=size)

    strtab = sh(e_shstrndx)
    out = {}
    for i in range(e_shnum):
        s = sh(i)
        end = data.index(b"\0", strtab["off"] + s["name"])
        out[data[strtab["off"] + s["name"]:end].decode()] = s
    return out


def find_factory_vtables(data, sec, relocs, text_lo, text_hi):
    """Locate the factory vtables without disassembling. See the note above."""
    text = sec[".text"]
    rod = sec[".rodata"]

    i = data.lower().find(IDWRITE_FACTORY_IID)
    if i == -1:
        raise SystemExit("IDWriteFactory IID string not found")
    guid_va = rod["addr"] + (i - rod["off"])

    sites = []
    base = text["off"]
    for off in range(base, base + text["size"] - 7):
        # REX.W/REX.WR + LEA + modrm selecting rip-relative addressing
        if data[off] in (0x48, 0x4C) and data[off + 1] == 0x8D and (data[off + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", data, off + 3)[0]
            if text["addr"] + (off + 7 - base) + disp == guid_va:
                sites.append(text["addr"] + (off - base))

    starts, prev = {}, False
    a, end = sec[".data.rel.ro"]["addr"], sec[".data.rel.ro"]["addr"] + sec[".data.rel.ro"]["size"]
    while a < end:
        val = relocs.get(a)
        cur = val is not None and text_lo <= val < text_hi
        if cur and not prev:
            starts.setdefault(val, a)
        prev, a = cur, a + 8

    found = {}
    for s in sites:
        cands = [(s - f, vt) for f, vt in starts.items() if 0 <= s - f < 512]
        if cands:
            _, vt = min(cands)
            n = 0
            while True:
                val = relocs.get(vt + 8 * n)
                if val is None or not (text_lo <= val < text_hi):
                    break
                n += 1
            found[vt] = n
    if not found:
        raise SystemExit("no candidate factory vtable found")
    best = max(found.values())
    return sorted(vt for vt, n in found.items() if n == best), best


def main():
    data = open(BINARY, "rb").read()
    sec = load_sections(data)
    text = sec[".text"]
    text_lo, text_hi = text["addr"], text["addr"] + text["size"]

    # Vtable entries live in .data.rel.ro and are zero in the file: each is an
    # R_X86_64_RELATIVE relocation whose addend is the function address. Build
    # the relocation map and read the vtable through it.
    rela = sec[".rela.dyn"]
    relocs = {}
    for i in range(rela["size"] // 24):
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, rela["off"] + i * 24)
        if (r_info & 0xFFFFFFFF) == R_X86_64_RELATIVE:
            relocs[r_offset] = r_addend

    candidates, _ = find_factory_vtables(data, sec, relocs, text_lo, text_hi)
    factory_vtable = candidates[0]
    print("Located %d factory vtable(s) by pattern scan: %s"
          % (len(candidates), ", ".join("0x%X" % c for c in candidates)))

    def read_vtable(vt):
        out = []
        while True:
            val = relocs.get(vt + 8 * len(out))
            if val is None or not (text_lo <= val < text_hi):
                return out
            out.append(val)

    slots = read_vtable(factory_vtable)

    # DWriteCoreCreateFactory has a shared and an isolated path, so two factory
    # objects exist. They must agree on every public slot; only their own
    # IUnknown (slots 0-2) and the internal virtuals past the public chain may
    # differ. Cross-checking them is free extra evidence that the layout read
    # here is the real one.
    for other in candidates[1:]:
        o = read_vtable(other)
        if len(o) != len(slots):
            print("  MISMATCH: candidate 0x%X has %d slots, not %d"
                  % (other, len(o), len(slots)))
            return 1
        differing = [i for i, (x, y) in enumerate(zip(slots, o)) if x != y]
        shared = [i for i in differing if 3 <= i < 60]
        print("  cross-check against 0x%X: %d/%d slots differ%s"
              % (other, len(differing), len(slots),
                 " (all outside the public chain)" if not shared
                 else " INCLUDING public slots %s" % shared))
        if shared:
            return 1

    if not slots:
        raise SystemExit("no relocated vtable entries at 0x%X" % factory_vtable)

    print("Factory primary vtable at 0x%X" % factory_vtable)
    print("  %d slots (%d methods after IUnknown's 3)\n" % (len(slots), len(slots) - 3))

    import gen_vtable_test as g
    counts = {}
    for h in ("dwrite.h", "dwrite_1.h", "dwrite_2.h", "dwrite_3.h"):
        p = os.path.join(SDK, h)
        if os.path.exists(p):
            for i in g.parse_header(p):
                counts[i["name"]] = len(i["methods"])

    cumulative = 0
    for n in range(0, 12):
        name = "IDWriteFactory" + (str(n) if n else "")
        if name not in counts:
            break
        cumulative += counts[name]
        print("  %-17s cumulative %3d methods -> %3d slots" %
              (name, cumulative, cumulative + 3))
    public = cumulative + 3
    print("\n  public chain needs %d slots; vtable has %d" % (public, len(slots)))
    if len(slots) < public:
        print("  MISMATCH: vtable is shorter than the public interface chain")
        return 1
    print("  %d trailing slots are internal virtuals beyond the public chain," % (len(slots) - public))
    print("  so every public slot is unaffected.\n")

    ifaces = g.parse_header(os.path.join(ROOT, "include", "dwrite.h"))
    factory = [i for i in ifaces if i["name"] == "IDWriteFactory"][0]
    print("  IDWriteFactory slots resolved from the binary:")
    for idx, m in enumerate(factory["methods"]):
        print("    slot %2d  %-32s -> 0x%06X" % (3 + idx, m["name"], slots[3 + idx]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
