#!/usr/bin/env python3
"""
Generate the test that holds the mirrored DirectWrite headers to account.

Callers use this library the way they use the retail one: create a factory and
call methods on the interfaces it returns. Those calls resolve to vtable slots,
and the slot each one reaches is the mirrored header's declaration order - the
same order the real library was built from. Nothing checks that by itself, so
this walks every interface and emits a call against a synthetic object whose
vtable records which slot was entered.

What it proves: the compiler agrees with the declaration order counted here.
That catches a method that stopped being virtual, a base that gained a second
inheritance path, a STDMETHOD this parser mis-read.

What it cannot prove: that the *binary* orders them the same way. Every number
available at compile time comes from these headers, so any assertion built from
them is circular. The check that does reach the implementation is
QueryInterface returning S_OK for an IID - the binary agreeing that it
implements the interface whose methods are about to be called - and after that,
the behavioral tests.
"""

import os
import re
import sys

from paths import ROOT, INCLUDE as INC

HEADERS = ["dwrite.h", "dwrite_1.h", "dwrite_2.h", "dwrite_3.h", "dwrite_core.h",
           "DWriteExperimental.h"]

IFACE_RE = re.compile(
    r'DWRITE_BEGIN_INTERFACE\(\s*(\w+)\s*,\s*"([0-9a-fA-F-]{36})"\s*\)\s*:\s*(\w+)')

# STDMETHOD(Name)( params ) PURE;   /   STDMETHOD_(Ret, Name)( params ) PURE;
METHOD_RE = re.compile(
    r'STDMETHOD(_)?\(\s*(?:([\w:\*\s]+?)\s*,\s*)?(\w+)\s*\)\s*\((.*?)\)\s*(?:CONST\s*)?PURE\s*;',
    re.S)

SAL_ARGS = re.compile(r'\b_[A-Z]\w*_\s*\([^()]*(?:\([^()]*\)[^()]*)*\)')
SAL_BARE = re.compile(r'\b_[A-Z]\w*_(?!\w)')


def split_params(text):
    """Split a parameter list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p for p in (x.strip() for x in out) if p]


def clean_param(p):
    """Return (declaration_without_default, argument_name)."""
    p = SAL_ARGS.sub(" ", p)
    p = SAL_BARE.sub(" ", p)
    # Drop a default value; only top-level '=' matters here.
    depth = 0
    for i, ch in enumerate(p):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "=" and depth == 0:
            p = p[:i]
            break
    p = re.sub(r'\s+', ' ', p).strip().rstrip(',')
    if not p or p == "void":
        return None, None
    m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$', p)
    if not m:
        return None, None
    return p, m.group(1)


def parse_header(path):
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    ifaces = []
    marks = [(m.start(), m.group(1), m.group(3)) for m in IFACE_RE.finditer(text)]
    for i, (pos, name, base) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        body = text[pos:end]
        methods = []
        for mm in METHOD_RE.finditer(body):
            ret = (mm.group(2) or "HRESULT").strip()
            mname = mm.group(3)
            params = []
            ok = True
            for p in split_params(mm.group(4)):
                decl, arg = clean_param(p)
                if decl is None:
                    ok = False
                    break
                params.append((decl, arg))
            if not ok:
                continue
            methods.append(dict(iface=name, name=mname, ret=ret, params=params))
        ifaces.append(dict(name=name, base=base, methods=methods))
    return ifaces


def flatten(ifaces):
    """
    Give each interface the methods it inherits, base-first, which is also
    vtable order: a derived interface's own slots follow its base's.

    Each method records the interface that declared it. The wrapper casts to
    that interface before dispatching, which both resolves C++ name hiding
    (IDWriteTextLayout::GetFontSize hides IDWriteTextFormat::GetFontSize) and
    keeps the call virtual - a Base::Method() qualified call would bind
    statically to a pure virtual instead.
    """
    by = {i["name"]: i for i in ifaces}
    cache = {}

    def chain(name):
        if name in cache:
            return cache[name]
        iface = by.get(name)
        if iface is None:
            r = []
        elif iface["base"] in ("IUnknown", ""):
            r = list(iface["methods"])
        else:
            r = chain(iface["base"]) + list(iface["methods"])
        cache[name] = r
        return r

    for i in ifaces:
        methods = []
        seen = {}
        for m in chain(i["name"]):
            seen[m["name"]] = seen.get(m["name"], 0) + 1
            n = seen[m["name"]]
            # Overloads, and base methods re-declared by a derived interface,
            # need distinct flat names.
            m = dict(m, suffix="" if n == 1 else "_%d" % n)
            methods.append(m)
        i["all"] = methods
    return ifaces


BANNER = """//+--------------------------------------------------------------------------
//
//  %s
//
//  GENERATED by tools/gen_vtable_test.py - do not edit by hand.
//
//  A call on a DirectWrite interface reaches a vtable slot, and which slot is
//  decided by the mirrored header's declaration order - the same order the
//  real library was built from. This walks every interface and calls every
//  method against a synthetic object whose vtable records the slot entered,
//  so a header that has drifted out of that order is caught here, and not by
//  a wrong-signature call into the implementation.
//
//  It proves the compiler agrees with the declaration order the generator
//  counted. It cannot prove the implementation orders them the same way:
//  every number available at compile time comes from these same headers, so
//  an assertion built from them is circular. What reaches the implementation
//  is QueryInterface returning S_OK for an IID, and the behavioral tests
//  beside this one.
//
//----------------------------------------------------------------------------
"""


CV_BEFORE = re.compile(r"\b([A-Za-z_]\w*(?:::\w+)*)\s+const\b(\s*[*&]?)")

def param_type(decl):
    """Strip the parameter name (and any array suffix) to leave the type."""
    d = re.sub(r'\[[^\]]*\]\s*$', '*', decl.strip())
    m = re.search(r'([A-Za-z_]\w*)\s*$', d)
    if not m:
        return None
    return d[:m.start(1)].strip()


def emit_test(ifaces):
    """
    Emit a test that checks every method lands on the vtable slot implied by
    the headers' declaration order (IUnknown occupies slots 0-2).

    The mock's vtable is an array of thunks that record their own index, so a
    call reaching the wrong slot is caught immediately. This is what holds the
    mirrored headers to account: it proves the compiler agrees with the
    declaration order this generator counted. It cannot prove the *binary*
    agrees - every number available here comes from the same headers the call
    does - and the check that does reach the binary is QueryInterface
    returning S_OK for an IID. FLOAT-returning
    methods are skipped: the thunks return in rax, not xmm0. Slots are
    sequential, so any drift still shows up on the following method.
    """
    lines = [
        BANNER % "vtable_test.cpp - checks each method reaches the expected vtable slot",
        "",
        # The experimental interfaces are checked too, and DWriteExperimental.h
        # is guarded by #if DWRITE_CORE, so dwrite_core.h has to come first.
        '#include "dwrite_core.h"',
        '#include "DWriteExperimental.h"',
        "",
        "#include <array>",
        "#include <cstdio>",
        "#include <utility>",
        "",
        "namespace {",
        "",
        "int g_slot = -1;",
        "int g_failures = 0;",
        "int g_checked = 0;",
        "",
        "template <int N>",
        "HRESULT SlotThunk(void*) { g_slot = N; return S_OK; }",
        "",
        "constexpr int kSlots = 256;",
        "",
        "template <std::size_t... I>",
        "// noexcept so the static initializer below is known not to throw: an",
        "// exception escaping it would terminate before main, and this only",
        "// takes the address of a function template instantiation.",
        "std::array<void*, sizeof...(I)> MakeVtable(std::index_sequence<I...>) noexcept",
        "{",
        "    return { reinterpret_cast<void*>(&SlotThunk<static_cast<int>(I)>)... };",
        "}",
        "",
        "std::array<void*, kSlots> g_vtable = MakeVtable(std::make_index_sequence<kSlots>{});",
        "",
        "// A COM object is a pointer to a vptr, not the vtable itself.",
        "auto* g_vptr = static_cast<void*>(g_vtable.data());",
        "auto* g_object = static_cast<void*>(&g_vptr);",
        "",
        "// A zero-valued argument of any parameter type.",
        "//",
        "// Reference parameters bind to a static object rather than to a temporary.",
        "// The thunks never read their arguments, so the value is irrelevant - but",
        "// `return T{}` for a reference T returns a reference to a temporary that",
        "// dies at the return, which is undefined behavior whether or not anyone",
        "// dereferences it. Only one method in the API takes a reference",
        "// (IDWritePaintReader::SetTextColor).",
        "template <typename T>",
        "struct ZeroArg",
        "{",
        "    // Every DirectWrite enum is declared with a fixed underlying type",
        "    // (`: INT32`), so zero is a representable and valid value even",
        "    // where no enumerator happens to be named for it - the rule about",
        "    // out-of-range enum values applies to enums without one. The",
        "    // thunks never read the argument in any case.",
        "    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)",
        "    static T Get() { return T{}; }",
        "};",
        "",
        "template <typename T>",
        "struct ZeroArg<T&>",
        "{",
        "    static T& Get()",
        "    {",
        "        static T value{};",
        "        return value;",
        "    }",
        "};",
        "",
        "void Check(const char* what, const int expected)",
        "{",
        "    ++g_checked;",
        "    if (g_slot != expected)",
        "    {",
        "        std::printf(\"FAIL %-64s expected slot %3d, dispatched to %3d\\n\",",
        "                    what, expected, g_slot);",
        "        ++g_failures;",
        "    }",
        "    g_slot = -1;",
        "}",
        "",
        "} // namespace",
        "",
        "int main()",
        "{",
    ]

    checked = 0
    for i in ifaces:
        iface = i["name"]
        if not i["all"]:
            continue
        lines.append("    // %s" % iface)
        for idx, m in enumerate(i["all"]):
            slot = 3 + idx  # QueryInterface, AddRef, Release occupy 0-2
            ret, params = m["ret"], m["params"]
            if ret.strip() == "FLOAT":
                continue
            types = [param_type(p) for p, _ in params]
            # cv-qualifier before the type, matching the rest of the tree
            types = [t if t is None else CV_BEFORE.sub(r"const \1\2", t)
                     for t in types]
            if any(t is None for t in types):
                continue
            what = "%s::%s%s" % (iface, m["name"], m["suffix"])
            args = ", ".join("ZeroArg<%s>::Get()" % t for t in types)
            cast = "" if ret == "void" else "(void)"
            # Called by name through the interface pointer, with one argument
            # per declared parameter. Taking the method's address instead would
            # name an overload set, and for every method whose name a derived
            # interface re-imports with `using Base::Name` that set spans two
            # classes. The base candidate has type Base::* and cannot match a
            # Derived::* target, so both `static_cast<sig>(&I::M)` and
            # `sig fn = &I::M;` are well-formed, and both compile clean.
            #
            # A call by name never forms an overload-set value at all. The
            # arguments are ZeroArg<T> for exactly the declared parameter
            # types, so the exact-match overload wins, which matters where a
            # method is overloaded (three GetMatchingFonts on IDWriteFontSet4,
            # two TranslateColorGlyphRun on IDWriteFactory8). If the wrong one
            # ever won, the call would land on a different slot and the check
            # below would fail loudly.
            #
            # The cast names the interface that *declared* the method, not the
            # one being tested, because a derived interface hides every base
            # overload of the same name. It stays unqualified through the
            # pointer so the call still dispatches through the vtable, which is
            # the thing being measured; a qualified I::M() call would bind
            # statically instead.
            owner = m["iface"]
            # static_cast, not reinterpret_cast: g_object is a void*, and
            # static_cast is the conversion defined for that - reinterpret_cast
            # is specified to fall back to exactly the same thing here, so this
            # is the same instruction with the narrower spelling.
            lines.append("    %sstatic_cast<%s*>(g_object)->%s(%s);"
                         % (cast, owner, m["name"], args))
            lines.append('    Check("%s", %d);' % (what, slot))
            checked += 1
        lines.append("")

    lines += [
        '    std::printf("\\nvtable slot check: %d/%d methods dispatched to the expected slot\\n",',
        "                g_checked - g_failures, g_checked);",
        "    return g_failures == 0 ? 0 : 1;",
        "}",
        "",
    ]

    os.makedirs(os.path.join(ROOT, "tests"), exist_ok=True)
    open(os.path.join(ROOT, "tests", "vtable_test.cpp"), "w").write("\n".join(lines))
    return checked


def main():
    ifaces = []
    for hname in HEADERS:
        ifaces += parse_header(os.path.join(INC, hname))
    ifaces = flatten(ifaces)
    t = emit_test(ifaces)
    print("Generated vtable test: %d interfaces, %d slot checks" % (len(ifaces), t))
    thin = [i["name"] for i in ifaces if not i["all"]]
    if thin:
        print("  (no methods parsed for: %s)" % ", ".join(thin))


if __name__ == "__main__":
    main()
