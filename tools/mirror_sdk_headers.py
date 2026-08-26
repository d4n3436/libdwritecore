#!/usr/bin/env python3
"""
Mirror the Windows SDK headers into include/ - only the declarations the
DirectWrite headers actually need.

dwrite.h includes <specstrings.h>, <unknwn.h> and <dcommon.h>. Providing
mirrors of those three means include/dwrite.h can keep its original include
lines verbatim, so its only remaining difference from the SDK is the appended
IID table.
"""

import os
import re
import sys

from paths import WINDOWS_SDK as SDK, INCLUDE as DST

# SAL annotations used by the mirrored DirectWrite headers. Kept in sync by
# scanning include/dwrite*.h below.
SAL_EXTRA = ["_Analysis_assume_", "_Use_decl_annotations_", "_Success_"]

# Names that match the SAL shape (_Word_) but are not annotations. Taking the
# no-op form of _HRESULT_TYPEDEF_ would expand every E_* status code to nothing.
SAL_EXCLUDE = {"_HRESULT_TYPEDEF_", "_COM_Outptr_result_maybenull_x_"}


def read(path):
    return open(os.path.join(SDK, path), encoding="utf-8", errors="surrogateescape").read()


def _match_braces(text, start):
    """From the '{' at/after start, return the index just past the closing ';'."""
    i = text.index("{", start)
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                j = text.find(";", i)
                return j + 1 if j != -1 else i + 1
        i += 1
    return -1


def strip_midl(text):
    """
    Blank out regions guarded by __midl, preserving offsets.

    Several SDK headers carry a MIDL variant of a declaration alongside the C
    one - basetsd.h spells INT_PTR as "typedef [public] __int3264 INT_PTR", and
    guiddef.h has a second GUID whose fields use the MIDL "byte" type. Those
    are not valid C++, and taking one instead of the real definition produces a
    confusing syntax error far from its cause.
    """
    out = []
    depth = 0
    midl_at = None
    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if re.match(r'#\s*(if|ifdef|ifndef)\b', stripped):
            depth += 1
            # Only the branch MIDL itself takes. A negated test such as
            # guiddef.h's "#if !defined (__midl)" is the branch a C++ compiler
            # takes, and blanking it would discard the real definitions.
            positive_midl = (
                re.match(r'#\s*ifdef\s+__midl\b', stripped) or
                re.match(r'#\s*if\s+defined\s*\(?\s*__midl\s*\)?\s*$', stripped))
            if midl_at is None and positive_midl:
                midl_at = depth
        elif re.match(r'#\s*endif\b', stripped):
            if midl_at is not None and depth == midl_at:
                midl_at = None
            depth -= 1
        elif re.match(r'#\s*(else|elif)\b', stripped):
            if midl_at is not None and depth == midl_at:
                midl_at = None

        out.append(re.sub(r'[^\n]', ' ', line) if midl_at is not None else line)
    return "".join(out)


def _cond_map(text):
    """For each line, the stack of enclosing #if line indices."""
    lines = text.splitlines(keepends=True)
    stacks, stack = [], []
    for i, line in enumerate(lines):
        t = line.lstrip()
        opened = bool(re.match(r'#\s*(if|ifdef|ifndef)\b', t))
        closed = bool(re.match(r'#\s*endif\b', t))
        if closed and stack:
            stack.pop()
        stacks.append(list(stack))
        if opened:
            stack.append(i)
    return lines, stacks


def extract_conditional(text, anchor, max_lines=260):
    """
    Return the whole #if/#else/#endif block that selects between alternative
    definitions of `anchor`, rather than one #define line.

    This is required wherever the SDK offers per-compiler or per-language
    variants: basetyps.h defines STDMETHOD and PURE differently for C and C++,
    and winnt.h defines DECLSPEC_NOVTABLE as __declspec(novtable) only under
    _MSC_VER with an empty #else. Taking a single line would pick the wrong
    branch. Returning the conditional intact lets the preprocessor choose.
    """
    lines, stacks = _cond_map(text)
    define_re = re.compile(r'[ \t]*#[ \t]*define[ \t]+' + re.escape(anchor) + r'([ \t(]|$)')
    hits = [i for i, l in enumerate(lines) if define_re.match(l)]
    if len(hits) < 2:
        return None

    common = None
    for h in hits:
        common = stacks[h] if common is None else [
            a for a, b in zip(common, stacks[h]) if a == b]
    if not common:
        return None

    start = common[-1]
    depth = 0
    for j in range(start, len(lines)):
        t = lines[j].lstrip()
        if re.match(r'#\s*(if|ifdef|ifndef)\b', t):
            depth += 1
        elif re.match(r'#\s*endif\b', t):
            depth -= 1
            if depth == 0:
                if j - start > max_lines:
                    return None
                return "".join(lines[start:j + 1]).rstrip()
    return None


def extract_block(text, pattern, max_lines=260):
    """Return the enclosing conditional of the first line matching `pattern`."""
    lines, stacks = _cond_map(text)
    for i, l in enumerate(lines):
        if re.search(pattern, l):
            if not stacks[i]:
                return l.rstrip()
            start = stacks[i][-1]
            depth = 0
            for j in range(start, len(lines)):
                t = lines[j].lstrip()
                if re.match(r'#\s*(if|ifdef|ifndef)\b', t):
                    depth += 1
                elif re.match(r'#\s*endif\b', t):
                    depth -= 1
                    if depth == 0:
                        if j - start > max_lines:
                            return None
                        return "".join(lines[start:j + 1]).rstrip()
    return None


def strip_comments(text):
    """Blank out /* */ comments, preserving offsets and line structure."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'/\*.*?\*/', blank, text, flags=re.S)


def extract(text, name):
    """Return the source text of the declaration of `name`, or None."""

    # Prefer a whole conditional when the SDK offers alternative definitions.
    block = extract_conditional(text, name)
    if block is not None:
        return block

    # 0. MIDL_INTERFACE("guid") Name { ... };
    #    Unknwn.h declares IUnknown this way, and also shows it inside a
    #    documentation comment earlier in the file - hence the comment strip,
    #    which additionally drops the inline /* [in] */ IDL parameter markers.
    stripped = strip_comments(text)
    m = re.search(r'MIDL_INTERFACE\(\s*"[0-9A-Fa-f-]+"\s*\)\s*' +
                  re.escape(name) + r'\b[^{;]*\{', stripped)
    if m:
        end = _match_braces(stripped, m.start())
        if end != -1:
            block = stripped[m.start():end]
            return "\n".join(l.rstrip() for l in block.splitlines())

    # 1. #define, including line continuations.
    m = re.search(r'^[ \t]*#[ \t]*define[ \t]+' + re.escape(name) + r'(?![\w])', text, re.M)
    if m:
        i = m.start()
        j = i
        while True:
            eol = text.find("\n", j)
            if eol == -1:
                eol = len(text)
            if text[max(i, eol - 1):eol].rstrip().endswith("\\"):
                j = eol + 1
                continue
            return text[i:eol]

    # 2. DECLARE_HANDLE(name);
    m = re.search(r'^[ \t]*DECLARE_HANDLE\s*\(\s*' + re.escape(name) + r'\s*\)\s*;', text, re.M)
    if m:
        return m.group(0)

    # 3. Plain single-line typedef. Tried before the brace matcher so a simple
    #    declaration is never mistaken for part of an unrelated struct.
    #
    #    The name has to be one of the *declarators*, not merely mentioned:
    #    windef.h contains "typedef const RECT FAR* LPCRECT;", which declares
    #    LPCRECT and only refers to RECT. Requiring the name to be followed by
    #    a comma, semicolon or array bound distinguishes the two, while still
    #    accepting multi-declarator forms like "typedef WCHAR *LPWSTR, *PWSTR;".
    declarator = re.compile(r'\b' + re.escape(name) + r'\s*[,;\[]')
    for m in re.finditer(r'^[ \t]*typedef\b[^;{}\n]*;', text, re.M):
        if declarator.search(m.group(0)):
            return m.group(0)

    # 4. struct / enum / union, tagged or typedef'd, named either before the
    #    brace or after the closing brace.
    for m in re.finditer(r'(?:typedef\s+)?(?:struct|enum|union)\b[^;{}]*\{', text):
        head = m.group(0)
        end = _match_braces(text, m.start())
        if end == -1:
            continue
        block = text[m.start():end]
        tail = block[block.rindex("}"):]
        named_in_head = re.search(r'\b(?:tag)?' + re.escape(name) + r'\b', head)
        named_in_tail = re.search(r'\b' + re.escape(name) + r'\b', tail)
        if named_in_head or named_in_tail:
            return block

    return None


BANNER = """//+--------------------------------------------------------------------------
//
//  {out} - mirrored from the Windows SDK ({src}).
//
//  GENERATED by tools/mirror_sdk_headers.py - do not edit by hand.
//
//  Only the declarations the DirectWrite headers need are reproduced here;
//  the SDK originals are far larger. Types with no source in this SDK subset
//  (GUID, HRESULT) and anything that has to differ off-Windows live in
//  compat.h instead.
//
//----------------------------------------------------------------------------

#ifndef {guard}
#define {guard}
#pragma once

#include "compat.h"

// Vendor text. Drift from the SDK is caught by diffing include/ against
// third_party/, never by static analysis noticing Microsoft's house style.
// NOLINTBEGIN
"""


def emit(out_name, src_name, symbols, prologue="", epilogue="", blocks=()):
    text = strip_midl(read(src_name))
    guard = "DWC_MIRROR_" + out_name.replace(".", "_").upper()
    parts = [BANNER.format(out=out_name, src=src_name, guard=guard)]
    if prologue:
        parts.append(prologue)

    missing = []
    for sym in symbols:
        block = extract(text, sym)
        if block is None:
            missing.append(sym)
            continue
        parts.append("\n/* --- %s --- */\n%s\n" % (sym, block.strip()))

    for label, pattern in blocks:
        block = extract_block(text, pattern)
        if block is None:
            missing.append(label)
        else:
            parts.append("\n/* --- %s --- */\n%s\n" % (label, block.strip()))

    if epilogue:
        parts.append(epilogue)
    parts.append("\n// NOLINTEND\n")
    parts.append("\n#endif // %s\n" % guard)

    open(os.path.join(DST, out_name), "w").write("\n".join(parts))
    total = len(symbols) + len(blocks)
    status = "OK" if not missing else "missing: " + ", ".join(missing)
    print("  %-16s <- %-22s %2d/%2d  %s"
          % (out_name, src_name, total - len(missing), total, status))
    return missing


def sal_names():
    """
    Every SAL annotation appearing anywhere in include/.

    This deliberately covers the mirrored Windows SDK headers as well as the
    DirectWrite ones: extracted SDK text carries its own annotations - winnt.h
    spells LPSTR with _Null_terminated_ - and missing one is a compile error,
    not a silent problem. specstrings.h itself is skipped since it is the file
    being generated.
    """
    names = set(SAL_EXTRA)
    skip = {"specstrings.h"}
    for h in sorted(os.listdir(DST)):
        if not h.endswith(".h") or h in skip:
            continue
        text = open(os.path.join(DST, h), encoding="utf-8",
                    errors="surrogateescape").read()
        names |= set(re.findall(r'\b_[A-Z]\w*_(?=\s*\()', text))
        names |= set(re.findall(r'\b_[A-Z]\w*_(?!\w)', text))
    return sorted(names - SAL_EXCLUDE)


def emit_specstrings():
    """
    SAL annotations, taken from shared/no_sal2.h.

    That header is the SDK's own "turn SAL off" definition set, so it supplies
    exactly the no-op forms - with the correct arity for each annotation, which
    matters: dwrite_1.h uses two-argument forms such as _Out_range_(lo, hi).
    Deriving these by guessing from usage would get those wrong.
    """
    names = sal_names()
    src = read("shared/no_sal2.h")

    # Usage text, so a fallback definition gets the right arity.
    usage = ""
    for h in ("dwrite.h", "dwrite_1.h", "dwrite_2.h", "dwrite_3.h", "dwrite_core.h"):
        pth = os.path.join(DST, h)
        if os.path.exists(pth):
            usage += open(pth, encoding="utf-8", errors="surrogateescape").read()

    found, fallback = [], []
    for n in sorted(names):
        block = extract(src, n)
        if block is not None:
            found.append(block.strip())
            continue
        # A few SAL2 annotations are declared only in sal.h, whose definitions
        # route through macro chains this subset does not carry. They annotate
        # and nothing more, so a no-op of the observed arity is equivalent.
        takes_args = re.search(r'\b' + re.escape(n) + r'\s*\(', usage)
        fallback.append("#define %s%s" % (n, "(x)" if takes_args else ""))

    lines = [
        "//+--------------------------------------------------------------------------",
        "//",
        "//  specstrings.h - SAL annotations for the DirectWrite headers.",
        "//",
        "//  GENERATED by tools/mirror_sdk_headers.py - do not edit by hand.",
        "//",
        "//  Extracted from shared/no_sal2.h, the SDK's own set of no-op SAL",
        "//  definitions. These annotations drive static analysis only and have",
        "//  no ABI effect; without _PREFAST_ the real SDK also compiles them",
        "//  away. Using the SDK's own no-op forms keeps the arities right.",
        "//",
        "//----------------------------------------------------------------------------",
        "",
        "#ifndef DWC_MIRROR_SPECSTRINGS_H",
        "#define DWC_MIRROR_SPECSTRINGS_H",
        "#pragma once",
        "",
        "#if defined(_WIN32)",
        "#include <specstrings.h>",
        "#else",
        "",
        "// Vendor text. Drift from the SDK is caught by diffing include/",
        "// against third_party/, never by static analysis.",
        "// NOLINTBEGIN",
        "",
    ]
    lines += found
    if fallback:
        lines += ["",
                  "/* Declared only in sal.h, whose macro chains are not part of",
                  "   this SDK subset. Annotation-only, so a no-op is equivalent. */"]
        lines += fallback
    lines += ["", "#endif // !_WIN32", "", "// NOLINTEND", "",
              "#endif // DWC_MIRROR_SPECSTRINGS_H", ""]

    open(os.path.join(DST, "specstrings.h"), "w").write("\n".join(lines))
    print("  %-16s <- %-22s %d/%d annotations%s"
          % ("specstrings.h", "shared/no_sal2.h", len(found), len(names),
             "" if not fallback else "  (%d no-op fallback)" % len(fallback)))
    return []


def main():
    if not os.path.isdir(SDK):
        sys.exit("no Windows SDK at %s" % SDK)
    print("Mirroring Windows SDK headers -> include/")

    # noinspection PyDictCreation
    all_missing = {}

    # winnt.h and guiddef.h supply the core scalar and GUID types, so
    # compat.h does not have to reconstruct them.
    all_missing["winnt.h"] = emit(
        "winnt.h", "um/winnt.h",
        # LONG is NOT mirrored: winnt.h spells it "long", which is
        # 32-bit under Windows' LLP64 but 64-bit under LP64. compat.h pins it,
        # and HRESULT (typedef LONG HRESULT) then comes out right.
        # HRESULT is NOT mirrored either: outside the __midl
        # branch winnt.h spells it "long", not "LONG", so it is LP64-unsafe in
        # its own right. compat.h pins it.
        # winnt.h defines __cdecl/_cdecl as empty in its non-MSVC branch, which
        # is the branch that applies here. __stdcall has no such definition -
        # it is purely a compiler keyword - so it stays in compat.h.
        ["__cdecl", "_cdecl",
         "VOID", "PVOID", "STDMETHODCALLTYPE", "STDAPICALLTYPE", "EXTERN_C",
         "DECLARE_HANDLE",
         "DECLSPEC_UUID", "DECLSPEC_NOVTABLE", "DECLSPEC_SELECTANY",
         "DECLSPEC_IMPORT", "DECLSPEC_NOTHROW", "DEFINE_ENUM_FLAG_OPERATORS",
         "CHAR", "SHORT", "LONGLONG", "ULONGLONG", "HANDLE",
         "LPSTR", "LPCSTR", "LPWSTR", "LPCWSTR"])

    all_missing["guiddef.h"] = emit(
        "guiddef.h", "shared/guiddef.h",
        # GUID is NOT mirrored: its Data1 is declared
        # "unsigned long", 64-bit under LP64, which would make GUID 24 bytes
        # instead of 16. compat.h pins it; the aliases below are safe.
        ["IID", "CLSID", "REFGUID", "REFIID", "REFCLSID"],
        blocks=[("GUID comparison", r'__inline\s+int\s+InlineIsEqualGUID'),
                ("GUID operators", r'__inline\s+bool\s+operator==')])

    # The COM declaration machinery. basetyps.h in particular defines
    # STDMETHOD/PURE/interface differently for C and C++, so these come across
    # as whole conditionals rather than single lines.
    all_missing["basetyps.h"] = emit(
        "basetyps.h", "shared/basetyps.h",
        ["COM_DECLSPEC_NOTHROW", "interface", "STDMETHOD", "STDMETHOD_", "PURE",
         "THIS", "THIS_"])

    all_missing["rpcndr.h"] = emit(
        "rpcndr.h", "shared/rpcndr.h",
        # MIDL_INTERFACE lives here, not in combaseapi.h as one might expect.
        ["MIDL_INTERFACE"])

    all_missing["rpc.h"] = emit(
        "rpc.h", "shared/rpc.h",
        ["__RPC_API", "__RPC_USER", "__RPC_STUB", "__RPC_FAR"])

    all_missing["rpcsal.h"] = emit(
        "rpcsal.h", "shared/rpcsal.h",
        ["__RPC__in", "__RPC__out", "__RPC__deref_out", "__RPC__deref_out_opt"])

    all_missing["combaseapi.h"] = emit(
        "combaseapi.h", "um/combaseapi.h", ["BEGIN_INTERFACE", "END_INTERFACE"])

    all_missing["rpcdce.h"] = emit(
        "rpcdce.h", "shared/rpcdce.h", ["UUID"],
        prologue="\n#ifndef UUID_DEFINED\n#define UUID_DEFINED\n",
        epilogue="\n#endif // UUID_DEFINED\n")

    all_missing["dxgiformat.h"] = emit(
        "dxgiformat.h", "shared/dxgiformat.h", ["DXGI_FORMAT"])

    all_missing["d3d9types.h"] = emit(
        "d3d9types.h", "shared/d3d9types.h", ["D3DCOLORVALUE"],
        prologue="\n#ifndef D3DCOLORVALUE_DEFINED\n",
        epilogue="\n#define D3DCOLORVALUE_DEFINED\n"
                 "#endif // D3DCOLORVALUE_DEFINED\n")

    all_missing["d2dbasetypes.h"] = emit(
        "d2dbasetypes.h", "um/d2dbasetypes.h", ["D2D_COLOR_F"])

    all_missing["basetsd.h"] = emit(
        "basetsd.h", "shared/basetsd.h",
        ["INT8", "INT16", "INT32", "INT64", "UINT8", "UINT16", "UINT32", "UINT64",
         ])

    all_missing["minwindef.h"] = emit(
        "minwindef.h", "shared/minwindef.h",
        # DWORD is NOT mirrored: the SDK spells it
        # "unsigned long", which is 32-bit under Windows' LLP64 but 64-bit
        # under LP64. compat.h pins it to uint32_t instead.
        # Order is significant: emitted declarations appear in manifest order,
        # and minwindef.h spells LPVOID as "void far *" and LPCVOID with CONST.
        ["near", "far", "NEAR", "FAR", "CONST",
         "WINAPI", "APIENTRY", "CALLBACK",
         "BYTE", "WORD", "BOOL", "INT", "UINT", "FLOAT", "FILETIME", "MAX_PATH",
         "TRUE", "FALSE", "LPVOID", "LPCVOID"])

    all_missing["windef.h"] = emit(
        "windef.h", "shared/windef.h",
        ["RECT", "POINT", "SIZE", "HDC", "HFONT", "HMONITOR", "HBITMAP", "HGDIOBJ",
         "COLORREF"])

    all_missing["wingdi.h"] = emit(
        "wingdi.h", "um/wingdi.h",
        ["LF_FACESIZE", "LOGFONTW", "FONTSIGNATURE"])

    all_missing["winerror.h"] = emit(
        "winerror.h", "shared/winerror.h",
        ["S_OK", "S_FALSE", "E_NOTIMPL", "E_NOINTERFACE", "E_POINTER", "E_ABORT",
         "E_FAIL", "E_UNEXPECTED", "E_OUTOFMEMORY", "E_INVALIDARG", "E_HANDLE",
         "_HRESULT_TYPEDEF_", "SUCCEEDED", "FAILED", "MAKE_HRESULT",
         "SEVERITY_ERROR", "SEVERITY_SUCCESS",
         "HRESULT_FROM_WIN32", "DWRITE_E_FILEFORMAT"])

    all_missing["dcommon.h"] = emit(
        "dcommon.h", "um/dcommon.h",
        ["DWRITE_MEASURING_MODE", "DWRITE_GLYPH_IMAGE_FORMATS",
         "D2D1_ALPHA_MODE", "D2D1_PIXEL_FORMAT",
         "D2D_POINT_2U", "D2D_POINT_2F", "D2D_POINT_2L", "D2D_VECTOR_2F",
         "D2D_VECTOR_3F",
         "D2D_VECTOR_4F", "D2D_RECT_F", "D2D_RECT_U", "D2D_SIZE_F", "D2D_SIZE_U",
         "D2D_MATRIX_3X2_F", "D2D_MATRIX_4X3_F", "D2D_MATRIX_4X4_F",
         "D2D_MATRIX_5X4_F"],
        epilogue=(
            "\n/* The D2D1_* spellings the DirectWrite headers use are aliases\n"
            "   of the D2D_* structures above. */\n"
            "typedef D2D_POINT_2F D2D1_POINT_2F;\n"
            "typedef D2D_POINT_2L D2D1_POINT_2L;\n"
            "typedef D2D_RECT_F   D2D1_RECT_F;\n"
            "typedef D2D_SIZE_U   D2D1_SIZE_U;\n"
            "typedef D2D_SIZE_F   D2D1_SIZE_F;\n"
                        "typedef D2D_MATRIX_3X2_F D2D1_MATRIX_3X2_F;\n"))

    # unknwn.h: take the C++ IUnknown declaration verbatim. The MIDL wrapper
    # macros around it are supplied by compat.h.
    all_missing["unknwn.h"] = emit(
        "unknwn.h", "um/Unknwn.h", ["IUnknown"],
        prologue="\n#ifndef __IUnknown_INTERFACE_DEFINED__\n"
                 "#define __IUnknown_INTERFACE_DEFINED__\n",
        epilogue="\n#endif // __IUnknown_INTERFACE_DEFINED__\n")

    all_missing["specstrings.h"] = emit_specstrings()

    leftover = sorted({s for v in all_missing.values() for s in v})
    if leftover:
        print("\nNot found in this SDK subset - these stay in compat.h:")
        print("  " + ", ".join(leftover))
    print("done.")


if __name__ == "__main__":
    main()
