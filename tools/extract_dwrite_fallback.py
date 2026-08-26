#!/usr/bin/env python3
"""
Extract DirectWrite's system font fallback table from a Windows DLL.

    tools/extract_dwrite_fallback.py --dll DWrite.dll             # check
    tools/extract_dwrite_fallback.py --dll DWrite.dll --write     # regenerate
    tools/extract_dwrite_fallback.py --dll DWrite.dll --json -    # dump
    tools/extract_dwrite_fallback.py --dll A.dll --against B.dll  # diff two

Two products carry this table in two formats, and both are read here, into one
shape. The build of DWriteCore linked on Linux carries neither - the Windows
builds do, and this reads the table out instead of measuring what a Windows
machine happened to draw.

DWrite.dll is what Firefox on Windows actually asks, so it is the one to
generate from; DWriteCore.dll is the same table as this project's own
implementation would answer from.

  DWriteCore.dll (Rust) keeps it in .rdata as what
  BindingLayer::GetSystemFontFallbackData returns:

    struct FontFallbackData {          // 0x30 bytes
        const u8*  names;              // +0x00  NUL-separated family and locale
        const u32* targets;            // +0x08  0x40000000 | offset into names,
                                       //        0 terminates a list
        const Record* records;         // +0x10  { u16 locale_off, u16, u16 first }
        const u32* range_ends;         // +0x18  last codepoint of each range
        const u16* range_record;       // +0x20  first record of each range
        u16 names_len, targets_len, records_len, ranges_len;   // +0x28
    };

A range covers (previous end + 1 ..= its own end). Its records run from
range_record[i] up to the next larger value in that array, so consecutive
ranges sharing a value share one group. Within a group a record with a locale
applies to that locale and the one without is the default; `first` starts a
NUL-terminated run in `targets`, which is the family list to try in order.

The 0x30 bytes after that struct hold weight/stretch/style triplets that no
record in this table refers to - record field 2 is asserted zero so that a build
which starts using them fails here instead of decoding wrongly.

  DWrite.dll (C++) keeps it in the FONTFALLBACK/Fallback resource, which
  FontFallbackDataInitialize loads into FontFallback::staticFontFallbackData_,
  as a serialized list of IDWriteFontFallbackBuilder::AddMapping calls. Six
  sections behind a header of six counts and six offsets, named here after the
  types DWrite itself carries (FontFallbackRegion::*):

    S0  Mapping[]   { u32 ranges, u32 range_count, u32 families,
                      u32 family_count, u32 locale, u32 base_family,
                      u32 flags, f32 scale }   - offsets into the blob
    S1  DWRITE_UNICODE_RANGE[]      { u32 first, u32 last }
    S2  Lookup[]    { u32 first, u32 last, u32 mappings, u32 count }
    S3  LookupMapping[]   offsets of the mappings each Lookup selects
    S4  TargetFamilyName[]   offsets of family names
    S5  the string pool, UTF-16

  A codepoint picks one Lookup, which is a bucket: the mappings it selects
  carry ranges of their own that subdivide it, and a mapping applies to a
  codepoint inside one of those ranges when its locale is empty or matches. The
  family list is the concatenation of the mappings that apply, in order. Every
  mapping is reachable from the index, so a bucket that selects nothing means
  DirectWrite has no fallback there.

Nothing is hardcoded to one build: each format is found by its own anchor, and
every field is checked before anything is written.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER_OUT = HERE.parent / "cleartype" / "src" / "dwrite_fallback_table.h"

# The first family in the pool on every build seen so far, and the anchor each
# format is found by.
POOL_ASCII = b"\x00Segoe UI Emoji\x00"
POOL_UTF16 = "\0Segoe UI Emoji\0".encode("utf-16-le")

TARGET_FLAG = 0x40000000            # set on every non-terminator target


class Pe:
    """Just enough PE to turn a virtual address into a file offset."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec, = struct.unpack_from("<H", data, pe + 6)
        optsize, = struct.unpack_from("<H", data, pe + 20)
        self.base, = struct.unpack_from("<Q", data, pe + 24 + 24)
        self.sections = []
        for i in range(nsec):
            o = pe + 24 + optsize + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rsize, roff = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, vaddr, vsize, roff, rsize))

    def off(self, va):
        rva = va - self.base
        for _, vaddr, vsize, roff, rsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                o = roff + (rva - vaddr)
                if o < roff + rsize:
                    return o
        raise ValueError("va %#x is not in any section" % va)

    def va(self, off):
        for _, vaddr, _vsize, roff, rsize in self.sections:
            if roff <= off < roff + rsize:
                return self.base + vaddr + (off - roff)
        raise ValueError("offset %#x is not in any section" % off)

    def section(self, name):
        for n, vaddr, vsize, roff, rsize in self.sections:
            if n == name:
                return vaddr, vsize, roff, rsize
        return None

    def version(self):
        """FileVersion out of VS_FIXEDFILEINFO, for the record."""
        at = self.data.find(b"\xbd\x04\xef\xfe")
        if at < 0:
            return None
        ms, ls = struct.unpack_from("<II", self.data, at + 8)
        return "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)

    def resources(self):
        """Every leaf in .rsrc, as (id path, file offset, size)."""
        sec = self.section(".rsrc")
        if sec is None:
            return []
        vaddr, _vsize, roff, _rsize = sec
        out = []

        def walk(off, path):
            n_named, n_id = struct.unpack_from("<HH", self.data, roff + off + 12)
            for i in range(n_named + n_id):
                nid, ptr = struct.unpack_from("<II", self.data, roff + off + 16 + 8 * i)
                if ptr & 0x80000000:
                    walk(ptr & 0x7FFFFFFF, path + [nid])
                else:
                    rva, size = struct.unpack_from("<II", self.data, roff + ptr)
                    out.append((path + [nid], roff + (rva - vaddr), size))

        walk(0, [])
        return out


def _normalize(ranges):
    """Order every range's conditions the way the generated header expects."""
    for r in ranges:
        loc = [e for e in r["entries"] if e["locale"] is not None]
        default = [e for e in r["entries"] if e["locale"] is None]
        if len(default) != 1:
            raise ValueError("range %#x has %d default conditions"
                             % (r["first"], len(default)))
        r["entries"] = loc + default
    return ranges


def _table(source, version, ranges, counts):
    return {
        "source": source,
        "version": version,
        "counts": counts,
        "families": sorted({f for r in ranges for e in r["entries"] for f in e["families"]}),
        "locales": sorted({e["locale"] for r in ranges for e in r["entries"] if e["locale"]}),
        "ranges": _normalize(ranges),
    }


# --------------------------------------------------------------------------
# DWriteCore.dll - FontFallbackData in .rdata
# --------------------------------------------------------------------------

def find_core_struct(pe):
    vaddr, _vsize, roff, rsize = pe.section(".rdata")
    hits = []
    at = pe.data.find(POOL_ASCII, roff, roff + rsize)
    while at >= 0:
        hits.append(at)
        at = pe.data.find(POOL_ASCII, at + 1, roff + rsize)
    if not hits:
        return None

    lo, hi = pe.base + vaddr, pe.base + vaddr + rsize
    candidates = []
    for hit in hits:
        pool_va = pe.va(hit)
        want = struct.pack("<Q", pool_va)
        o = pe.data.find(want, roff, roff + rsize)
        while o >= 0:
            if o % 8 == 0 and o + 0x30 <= roff + rsize:
                ptrs = struct.unpack_from("<5Q", pe.data, o)
                lens = struct.unpack_from("<4H", pe.data, o + 0x28)
                if (all(lo <= p < hi for p in ptrs) and all(lens)
                        and hit + lens[0] <= roff + rsize
                        and pe.data[hit + lens[0] - 1] == 0):
                    candidates.append((o, pool_va))
            o = pe.data.find(want, o + 1, roff + rsize)

    if len(candidates) != 1:
        raise ValueError("expected one FontFallbackData, found %d" % len(candidates))
    return candidates[0]


def read_dwritecore(pe):
    found = find_core_struct(pe)
    if found is None:
        return None
    off, pool_va = found
    names_p, targets_p, records_p, ends_p, first_p = struct.unpack_from("<5Q", pe.data, off)
    names_len, targets_len, records_len, ranges_len = struct.unpack_from("<4H", pe.data, off + 0x28)
    if names_p != pool_va:
        raise ValueError("struct does not start at the pool pointer")

    d = pe.data
    names = d[pe.off(names_p):pe.off(names_p) + names_len]
    targets = struct.unpack_from("<%dI" % targets_len, d, pe.off(targets_p))
    records = [struct.unpack_from("<3H", d, pe.off(records_p) + 6 * i)
               for i in range(records_len)]
    ends = struct.unpack_from("<%dI" % ranges_len, d, pe.off(ends_p))
    first = struct.unpack_from("<%dH" % ranges_len, d, pe.off(first_p))

    def name(o):
        if o == 0 or o >= len(names):
            raise ValueError("name offset %d out of range" % o)
        return names[o:names.index(b"\0", o)].decode("ascii")

    # Every field, checked before it is believed.
    if names[:1] != b"\0":
        raise ValueError("the name pool does not start with a terminator")
    if any(ends[i] >= ends[i + 1] for i in range(ranges_len - 1)):
        raise ValueError("range ends are not ascending")
    if any(first[i] > first[i + 1] for i in range(ranges_len - 1)):
        raise ValueError("range record indices are not non-decreasing")
    if max(first) >= records_len:
        raise ValueError("a range points past the record array")
    for a, b, c in records:
        if b != 0:
            raise ValueError("record field 2 is not always zero (%d)" % b)
        if c >= targets_len:
            raise ValueError("a record points past the target array")
    for t in targets:
        if t and (t & ~0xFFFFFF) != TARGET_FLAG:
            raise ValueError("unexpected target flags %#x" % t)

    def target_list(start):
        out = []
        k = start
        while k < targets_len and targets[k] != 0:
            out.append(name(targets[k] & 0xFFFFFF))
            k += 1
        if k >= targets_len:
            raise ValueError("target list from %d is not terminated" % start)
        return out

    bounds = sorted(set(first)) + [records_len]
    group_end = {b: bounds[i + 1] for i, b in enumerate(bounds[:-1])}

    ranges = []
    low = 0
    for i in range(ranges_len):
        entries = [{"locale": name(a) if a else None, "families": target_list(c)}
                   for a, _b, c in records[first[i]:group_end[first[i]]]]
        ranges.append({"first": low, "last": ends[i], "entries": entries})
        low = ends[i] + 1

    return _table("DWriteCore.dll", pe.version(), ranges,
                  {"ranges": ranges_len, "records": records_len,
                   "targets": targets_len, "names": names_len})


# --------------------------------------------------------------------------
# DWrite.dll - serialized AddMapping calls in .rsrc
# --------------------------------------------------------------------------

def read_dwrite(pe):
    blob = None
    for _path, fo, size in pe.resources():
        hit = pe.data.find(POOL_UTF16, fo, fo + size)
        if hit >= 0:
            if blob is not None:
                raise ValueError("more than one resource holds the family pool")
            blob = pe.data[fo:fo + size]
    if blob is None:
        return None
    return parse_dwrite_blob(blob, "DWrite.dll", pe.version())


def parse_dwrite_blob(blob, source="FONTFALLBACK", version=None):
    """The FONTFALLBACK/Fallback resource, however it was obtained.

    FontFallbackDataInitialize will load one of these from a file named by
    HKLM\\Software\\Microsoft\\DirectWrite!FontFallback in preference to its
    own resource, so a standalone blob is a shape DirectWrite itself accepts.
    """
    pool_at = blob.find(POOL_UTF16)
    if pool_at < 0:
        raise ValueError("this is not a fallback blob: no family pool in it")

    counts = struct.unpack_from("<6I", blob, 0)
    offs = struct.unpack_from("<6I", blob, 24)
    widths = (32, 8, 16, 4, 4, 2)
    ends = list(offs[1:]) + [len(blob)]
    for i, (c, o, w, e) in enumerate(zip(counts, offs, widths, ends)):
        if o >= len(blob) or e > len(blob) or (e - o) != c * w:
            raise ValueError("section %d is %d bytes, not %d x %d"
                             % (i, e - o, c, w))
    if offs[5] != pool_at:
        raise ValueError("the string pool is not the last section")

    def wstr(at):
        if not offs[5] <= at < len(blob):
            raise ValueError("string offset %#x is outside the pool" % at)
        end = at
        while blob[end:end + 2] != b"\0\0":
            end += 2
        return blob[at:end].decode("utf-16-le")

    def mapping(at):
        if not offs[0] <= at < offs[1] or (at - offs[0]) % 32:
            raise ValueError("mapping offset %#x is not a mapping" % at)
        rp, rc, tp, tc, loc, base, flags, scale = struct.unpack_from("<7If", blob, at)
        if flags != 0 or scale != 1.0:
            raise ValueError("mapping at %#x has flags %d scale %g" % (at, flags, scale))
        if wstr(base):
            raise ValueError("mapping at %#x is conditioned on base family %r"
                             % (at, wstr(base)))
        if not offs[1] <= rp < offs[2] or (rp - offs[1]) % 8:
            raise ValueError("mapping at %#x has no range list" % at)
        range_list = [struct.unpack_from("<2I", blob, rp + 8 * k) for k in range(rc)]
        fams = [wstr(struct.unpack_from("<I", blob, tp + 4 * k)[0]) for k in range(tc)]
        return wstr(loc), fams, range_list

    # An index entry is a bucket: the mappings it selects carry their own
    # ranges, which subdivide it, so the bucket is cut at every boundary its
    # mappings introduce and each piece gets the mappings that cover it.
    ranges = []
    for i in range(counts[2]):
        first, last, mp, mc = struct.unpack_from("<4I", blob, offs[2] + 16 * i)
        if first > last:
            raise ValueError("index entry %d is empty" % i)
        applies = [mapping(struct.unpack_from("<I", blob, mp + 4 * j)[0])
                   for j in range(mc)]

        cuts = {first, last + 1}
        for _loc, _f, rs in applies:
            for lo, hi in rs:
                if first <= lo <= last:
                    cuts.add(lo)
                if first <= hi < last:
                    cuts.add(hi + 1)
        cuts = sorted(cuts)

        for lo, nxt in zip(cuts, cuts[1:]):
            here = [(loc, fams) for loc, fams, rs in applies
                    if any(a <= lo <= b for a, b in rs)]
            if not here:
                continue
            locales = []
            for loc, _f in here:
                if loc and loc not in locales:
                    locales.append(loc)
            entries = []
            for loc in locales + [""]:
                fams = []
                for mloc, mfams in here:
                    if mloc == "" or mloc == loc:
                        fams.extend(f for f in mfams if f not in fams)
                entries.append({"locale": loc or None, "families": fams})
            ranges.append({"first": lo, "last": nxt - 1, "entries": entries})

    ranges.sort(key=lambda r: r["first"])
    for a, b in zip(ranges, ranges[1:]):
        if a["last"] >= b["first"]:
            raise ValueError("index ranges overlap at %#x" % b["first"])

    return _table(source, version, ranges,
                  {"mappings": counts[0], "unicode_ranges": counts[1],
                   "index": counts[2], "selected": counts[3],
                   "family_refs": counts[4], "names": counts[5]})


def read_table(pe):
    for reader in (read_dwrite, read_dwritecore):
        table = reader(pe)
        if table is not None:
            return table
    raise ValueError("no fallback table found in this image")


# --------------------------------------------------------------------------

def resolve(table, cp, locale=None):
    """The family list a codepoint gets, or None where the table says nothing."""
    for r in table["ranges"]:
        if r["first"] <= cp <= r["last"]:
            best = None
            for e in r["entries"]:
                if e["locale"] is None:
                    best = best or e
                elif locale and e["locale"].lower() == locale.lower():
                    return e["families"]
            return best["families"] if best else None
    return None


def compare(a, b):
    """Codepoints where two tables answer differently, coalesced into runs."""
    bounds = sorted({0} | {r["first"] for r in a["ranges"]} | {r["last"] + 1 for r in a["ranges"]}
                    | {r["first"] for r in b["ranges"]} | {r["last"] + 1 for r in b["ranges"]})
    locales = sorted({l.lower() for l in a["locales"]} | {l.lower() for l in b["locales"]})
    diffs = []
    for cp in bounds:
        if cp > 0x10FFFF:
            continue
        for loc in [None] + locales:
            fa, fb = resolve(a, cp, loc), resolve(b, cp, loc)
            if fa != fb:
                diffs.append((cp, loc, fa, fb))
    return diffs


def emit_header(table):
    """The table, transcribed. Resolution is left to whoever uses it."""
    ranges = table["ranges"]
    families = table["families"]
    locales = table["locales"]
    strings = families + [l for l in locales if l not in families]
    index = {s: i for i, s in enumerate(strings)}

    flat = []
    recs = []
    for i, r in enumerate(ranges):
        for e in r["entries"]:
            recs.append((i, index[e["locale"]] if e["locale"] else -1, len(flat)))
            flat.extend(index[f] for f in e["families"])
            flat.append(-1)

    out = []
    w = out.append
    w("// GENERATED by tools/extract_dwrite_fallback.py - do not edit by hand.")
    w("//")
    w("// DirectWrite's system font fallback table, read out of %s%s."
      % (table["source"], " " + table["version"] if table["version"] else ""))
    w("// See the tool for the layout it was read from. Nothing here is measured:")
    w("// these are the ranges and family preferences DirectWrite itself carries.")
    w("//")
    w("// A range covers [first, last]. Its conditions are consecutive entries in")
    w("// kConditions; the one with locale < 0 is the default and comes last. Each")
    w("// condition starts a run in kFamilyIndices that ends at -1, listing the")
    w("// families to try in order, as indices into kStrings. A codepoint outside")
    w("// every range has no mapping.")
    w("")
    w("#ifndef DWRITECORE_DWRITE_FALLBACK_TABLE_H_INCLUDED")
    w("#define DWRITECORE_DWRITE_FALLBACK_TABLE_H_INCLUDED")
    w("")
    w("namespace dwc::dwrite_fallback")
    w("{")
    w("")
    w("struct Range { unsigned first; unsigned last; unsigned condition; };")
    w("struct Condition { unsigned range; int locale; unsigned families; };")
    w("")
    w("inline constexpr const char* kStrings[] = {")
    for s in strings:
        w('    "%s",' % s)
    w("};")
    w("")
    w("inline const Range kRanges[] = {")
    cond_of_range = {}
    for n, (ri, _loc, _f) in enumerate(recs):
        cond_of_range.setdefault(ri, n)
    for i, r in enumerate(ranges):
        w("    { .first = 0x%04X, .last = 0x%04X, .condition = %d },"
          % (r["first"], r["last"], cond_of_range[i]))
    w("};")
    w("")
    w("inline const Condition kConditions[] = {")
    for ri, loc, fi in recs:
        w("    { .range = %d, .locale = %d, .families = %d }," % (ri, loc, fi))
    w("};")
    w("")
    w("inline const short kFamilyIndices[] = {")
    for i in range(0, len(flat), 16):
        w("    " + " ".join("%d," % v for v in flat[i:i + 16]))
    w("};")
    w("")
    w("} // namespace dwc::dwrite_fallback")
    w("")
    w("#endif")
    w("")
    return "\n".join(out)


def describe(table, path):
    c = ", ".join("%d %s" % (v, k) for k, v in table["counts"].items())
    return "%s: %s %s - %s, %d families, %d locales" % (
        Path(path).name, table["source"], table["version"] or "?", c,
        len(table["families"]), len(table["locales"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dll", help="a Windows DWrite.dll or DWriteCore.dll")
    ap.add_argument("--blob", help="a FONTFALLBACK/Fallback resource on its own")
    ap.add_argument("--label", help="what to call the source of a --blob")
    ap.add_argument("--against", help="a second DLL, to compare rather than generate")
    ap.add_argument("--write", action="store_true", help="rewrite the generated header")
    ap.add_argument("--json", help="also write the decoded table as JSON ('-' for stdout)")
    ap.add_argument("--header", default=str(HEADER_OUT), help="where the header goes")
    args = ap.parse_args()

    if not args.dll and not args.blob:
        ap.error("one of --dll or --blob is required")
    if args.blob:
        table = parse_dwrite_blob(Path(args.blob).read_bytes(),
                                  args.label or Path(args.blob).name)
        print(describe(table, args.blob), file=sys.stderr)
    else:
        table = read_table(Pe(Path(args.dll).read_bytes()))
        print(describe(table, args.dll), file=sys.stderr)

    if args.against:
        other = read_table(Pe(Path(args.against).read_bytes()))
        print(describe(other, args.against), file=sys.stderr)
        diffs = compare(table, other)
        for cp, loc, fa, fb in diffs:
            print("U+%04X %-8s %s\n         %s" % (cp, loc or "-", fa, fb))
        print("%d codepoint(s) answered differently" % len(diffs), file=sys.stderr)
        return 1 if diffs else 0

    if args.json:
        text = json.dumps(table, indent=1, ensure_ascii=False)
        if args.json == "-":
            print(text)
        else:
            Path(args.json).write_text(text + "\n")

    header = emit_header(table)
    path = Path(args.header)
    if args.write:
        path.write_text(header)
        print("wrote %s" % path, file=sys.stderr)
        return 0
    if not path.exists():
        print("%s does not exist; run with --write" % path, file=sys.stderr)
        return 1
    if path.read_text() != header:
        print("%s is out of date; run with --write" % path, file=sys.stderr)
        return 1
    print("%s is up to date" % path, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
