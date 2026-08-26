#!/usr/bin/env python3
"""
Produce a SONAME-renamed copy of the stock libdwritecore.so.

The shim is itself named libdwritecore.so, so it cannot dlopen the original
under that same SONAME - the dynamic loader would dedup the request and hand
back the shim, making the forward recurse. This writes a copy whose DT_SONAME
reads "libdwcoreimpl.so" instead.

DT_SONAME is an offset into .dynstr, so the replacement name must be exactly as
long as the original for every other offset to stay valid. Both names are 16
characters. The original file is never modified.

(patchelf would also do this, but it is not required to be installed.)
"""

import os
import shutil
import struct
import sys

ORIGINAL_SONAME = b"libdwritecore.so"
NEW_SONAME = b"libdwcoreimpl.so"

DT_NULL = 0
DT_STRTAB = 5
DT_SONAME = 14

from paths import BINARY, ROOT


def patch(src, dst):
    assert len(ORIGINAL_SONAME) == len(NEW_SONAME), "names must match in length"

    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        data = bytearray(f.read())

        if data[:4] != b"\x7fELF" or data[4] != 2:
            raise SystemExit("%s: not a 64-bit ELF" % src)

        e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
        e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
        e_phnum = struct.unpack_from("<H", data, 0x38)[0]

        # Locate PT_DYNAMIC (2) and build a vaddr -> file offset map from PT_LOAD (1).
        dyn_off = dyn_size = None
        loads = []
        for i in range(e_phnum):
            base = e_phoff + i * e_phentsize
            p_type = struct.unpack_from("<I", data, base)[0]
            p_offset, p_vaddr = struct.unpack_from("<QQ", data, base + 0x08)
            p_filesz = struct.unpack_from("<Q", data, base + 0x20)[0]
            if p_type == 2:
                dyn_off, dyn_size = p_offset, p_filesz
            elif p_type == 1:
                loads.append((p_vaddr, p_filesz, p_offset))

        if dyn_off is None:
            raise SystemExit("%s: no PT_DYNAMIC" % src)

        def vaddr_to_off(v):
            for seg_vaddr, seg_filesz, seg_offset in loads:
                if seg_vaddr <= v < seg_vaddr + seg_filesz:
                    return seg_offset + (v - seg_vaddr)
            raise SystemExit("vaddr 0x%x not in any PT_LOAD" % v)

        strtab = soname_index = None
        for off in range(dyn_off, dyn_off + dyn_size, 16):
            tag, val = struct.unpack_from("<QQ", data, off)
            if tag == DT_NULL:
                break
            if tag == DT_STRTAB:
                strtab = val
            elif tag == DT_SONAME:
                soname_index = val

        if strtab is None or soname_index is None:
            raise SystemExit("%s: missing DT_STRTAB or DT_SONAME" % src)

        pos = vaddr_to_off(strtab) + soname_index
        current = bytes(data[pos:pos + len(ORIGINAL_SONAME)])
        if current != ORIGINAL_SONAME:
            raise SystemExit("%s: DT_SONAME is %r, expected %r"
                             % (src, current, ORIGINAL_SONAME))

        data[pos:pos + len(NEW_SONAME)] = NEW_SONAME
        f.seek(0)
        f.write(data)
        f.truncate()

    print("%s -> %s (DT_SONAME %s -> %s)"
          % (os.path.basename(src), os.path.basename(dst),
             ORIGINAL_SONAME.decode(), NEW_SONAME.decode()))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else BINARY
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", NEW_SONAME.decode())
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    patch(src, dst)


if __name__ == "__main__":
    main()
