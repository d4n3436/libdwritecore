#!/usr/bin/env python3
"""
gen_stress_page.py - a text rendering stress page, generated so it is exact.

    tools/testing/gen_stress_page.py <out.html>

The public multi-script pages are script-coverage pages: they exercise a lot of
Unicode and almost nothing else. Checked, none of them varies text color or
background at all - and those are the two axes that decide how Firefox
rasterizes, not merely what it draws:

  * text color is an input to rasterization, because WebRender's preblend is
    keyed on it and the gamma table is not linear in it;
  * background opacity decides which rasterizer runs at all, because a run that
    is not on an opaque background drops from subpixel to grayscale.

So this crosses them: background x text color x font x size x weight and style
x script, every cell labeled with what it is. Generated, not hand-written, so
the axes can be widened when a new gap turns up, and so the page is identical
on both machines - no scripts, no webfonts, no network, nothing that renders
differently the second time.

Every family named here is one Windows ships, so the same file can be resolved
on both sides; the generic families are included on purpose, since which file
they resolve to is itself part of what is being compared.
"""

import sys

# (label, lang, text). Real sentences instead of character soup: shaping,
# kerning and per-language font selection all key off them.
SCRIPTS = [
    ("Latin", "en", "The quick brown fox jumps over the lazy dog"),
    ("Latin accents", "cs", "Příliš žluťoučký kůň úpěl ďábelské ódy"),
    ("Greek", "el", "Ταχίστη αλώπηξ βαφής ψημένη γη"),
    ("Cyrillic", "ru", "Съешь же ещё этих мягких французских булок"),
    ("Hebrew", "he", "דג סקרן שט בים מאוכזב ולפתע מצא חברה"),
    ("Arabic", "ar", "نص حكيم له سر قاطع وذو شأن عظيم"),
    ("Devanagari", "hi", "ऋषियों को सताने वाले दुष्ट राक्षसों"),
    ("Thai", "th", "เป็นมนุษย์สุดประเสริฐเลิศคุณค่า"),
    ("Japanese", "ja", "いろはにほへと ちりぬるを 色は匂へど"),
    ("Korean", "ko", "다람쥐 헌 쳇바퀴에 타고파"),
    ("Chinese", "zh-CN", "视野无限宽，窗外有蓝天"),
]

FAMILIES = ["Arial", "Times New Roman", "Segoe UI", "Tahoma", "Verdana",
            "Georgia", "Consolas", "Courier New",
            "sans-serif", "serif", "monospace", "system-ui"]

SIZES = [11, 13, 14.5, 16, 20, 26]

# (label, background css, text color). Ordinary page colors, then the two that
# change the pipeline and not just the palette: a translucent layer and a
# gradient, both of which take the text off an opaque background.
SURFACES = [
    ("white on white", "#ffffff", "#000000"),
    ("near-black on near-white", "#f8f9fa", "#202122"),
    ("gray on white", "#ffffff", "#72777d"),
    ("link blue on white", "#ffffff", "#3366cc"),
    ("crimson on white", "#ffffff", "#b32424"),
    ("white on near-black", "#101418", "#ffffff"),
    ("light on dark", "#1e2430", "#dce3e6"),
    ("dark on mid gray", "#9aa0a6", "#101418"),
    ("on 60% translucent white", "rgba(255,255,255,0.6)", "#202122"),
    ("on 92% translucent dark", "rgba(16,20,24,0.92)", "#e8eef2"),
    ("on a gradient", "linear-gradient(90deg,#ffffff,#c8d4e0)", "#101418"),
]

STYLES = [("regular", "normal", "400"), ("bold", "normal", "700"),
          ("italic", "italic", "400"), ("bold italic", "italic", "700")]

CSS = """
:root { color-scheme: light }
* { margin: 0; padding: 0; box-sizing: border-box }
body { background: #ffffff; font: 13px "Courier New", monospace; color: #111 }
h2 { font: 700 15px Arial, sans-serif; padding: 6px 8px; background: #e8e8e8;
     border-top: 2px solid #444 }
.cap { font: 10px "Courier New", monospace; color: #555; padding: 1px 8px }
.surface { padding: 6px 8px 10px }
.row { white-space: nowrap; overflow: hidden }
.lbl { display: inline-block; width: 190px; font: 10px "Courier New", monospace;
       color: inherit; opacity: 0.75; vertical-align: middle }
.spec { display: inline-block; vertical-align: middle }
table.grid { border-collapse: collapse; width: 100% }
table.grid td, table.grid th { border: 1px solid #d0d0d0; padding: 2px 5px;
       font: 11px "Courier New", monospace; vertical-align: middle }
"""


# Looked up by label so a section can ask for one script by name.
SCRIPTS_BY_LABEL = {label: text for label, _, text in SCRIPTS}


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def surface_sections(out):
    """Every surface, each showing the same specimen in several families."""
    for name, background, color in SURFACES:
        out.append('<h2>surface: %s</h2>' % esc(name))
        out.append('<div class="cap">background:%s color:%s</div>'
                   % (esc(background), esc(color)))
        out.append('<div class="surface" style="background:%s;color:%s">'
                   % (background, color))
        for family in FAMILIES:
            for size in (13, 16):
                out.append(
                    '<div class="row"><span class="lbl">%s %spx</span>'
                    '<span class="spec" style="font-family:%s;font-size:%spx">%s</span></div>'
                    % (esc(family), size, esc(family), size, esc(SCRIPTS[0][2])))
        out.append('</div>')


def size_sections(out):
    """Sizes against families, including a fractional one."""
    out.append('<h2>size x family, near-black on near-white</h2>')
    out.append('<div class="surface" style="background:#f8f9fa;color:#202122">')
    for family in FAMILIES:
        for size in SIZES:
            out.append(
                '<div class="row"><span class="lbl">%s %spx</span>'
                '<span class="spec" style="font-family:%s;font-size:%spx">%s</span></div>'
                % (esc(family), size, esc(family), size, esc(SCRIPTS[0][2])))
    out.append('</div>')


def style_sections(out):
    """Weight and slant, including the families with no italic file, where the
    slant has to be synthesized."""
    out.append('<h2>weight and slant</h2>')
    out.append('<div class="surface" style="background:#ffffff;color:#000000">')
    for family in FAMILIES:
        for label, style, weight in STYLES:
            out.append(
                '<div class="row"><span class="lbl">%s %s</span>'
                '<span class="spec" style="font-family:%s;font-style:%s;'
                'font-weight:%s;font-size:16px">%s</span></div>'
                % (esc(family), esc(label), esc(family), style, weight,
                   esc(SCRIPTS[0][2])))
    out.append('</div>')


def script_sections(out):
    """Every script on every surface that changes the pipeline, plus a plain
    one, so a font picked for a script can be compared on each."""
    for name, background, color in SURFACES[:2] + SURFACES[5:6] + SURFACES[8:]:
        out.append('<h2>scripts on: %s</h2>' % esc(name))
        out.append('<div class="surface" style="background:%s;color:%s">'
                   % (background, color))
        for label, lang, text in SCRIPTS:
            for size in (14, 20):
                out.append(
                    '<div class="row"><span class="lbl">%s %spx</span>'
                    '<span class="spec" lang="%s" style="font-size:%spx">%s</span></div>'
                    % (esc(label), size, lang, size, esc(text)))
        out.append('</div>')


def default_size_sections(out):
    """Text with no font-size at all, and with a generic family.

    This section exists because its absence hid a whole-point error. Firefox's
    own defaults are per platform and per language group -
    font.size.monospace.x-western is 13 in modules/libpref/init/all.js and 12
    again in its "#if !ANDROID && !XP_MACOSX && XP_UNIX" block - so an unsized
    <pre> renders a point smaller on Linux than on Windows. Every block on this
    page named its family and its size, so the page scored 100.0000% while
    every <pre> on the web was wrong.

    Nothing here may name a size, and nothing may name a family: the generics
    and the defaults are the whole point.
    """
    out.append('<h2>no size, no family: the browser\'s own defaults</h2>')
    out.append('<div class="surface" style="background:#ffffff;color:#000000">')
    for generic in ("serif", "sans-serif", "monospace", "cursive", "fantasy",
                    "system-ui"):
        out.append('<div class="row"><span class="lbl">%s</span>'
                   '<span style="font-family:%s">%s</span></div>'
                   % (esc(generic), generic, esc(SCRIPTS[0][2])))
    out.append('<pre>unsized pre: Handgloves 0123456789 {|}~ ILil1 O0</pre>')
    out.append('<p>unsized <code>code</code> inside a paragraph, '
               '<kbd>kbd</kbd>, <samp>samp</samp> and <tt>tt</tt>.</p>')
    for label, lang, text in SCRIPTS:
        out.append('<div class="row"><span class="lbl">%s default</span>'
                   '<span lang="%s">%s</span></div>' % (esc(label), lang, esc(text)))
        out.append('<pre lang="%s">%s</pre>' % (lang, esc(text)))
    out.append('</div>')


def fractional_size_sections(out):
    """Sizes that are not whole pixels, made the way a page makes them.

    A CSS percentage of a percentage is the usual source - 110% of 110% of 15px
    is 18.15px - and fractional sizes are where the two platforms have the most
    room to disagree: the FreeType side receives the size quantized to 1/64 px,
    and a face whose head.flags has bit 3 set ("force ppem to integer values")
    gets it rounded to a whole pixel on top of that, which DirectWrite never
    does. Calibri, Cambria and Courier New have the bit; Arial and Times New
    Roman do not, so a page built only from the second kind sees nothing.
    """
    out.append('<h2>fractional sizes, from nested percentages</h2>')
    out.append('<div class="surface" style="background:#ffffff;color:#000000">')
    for family in FAMILIES:
        out.append('<div class="row"><span class="lbl">%s nested</span>'
                   '<span class="spec" style="font-family:%s;font-size:15px">'
                   '<span style="font-size:110%%"><span style="font-size:110%%">%s'
                   '</span></span></span></div>'
                   % (esc(family), esc(family), esc(SCRIPTS[0][2])))
        for size in ("13.33", "16.5", "20.5", "18.15"):
            out.append('<div class="row"><span class="lbl">%s %spx</span>'
                       '<span class="spec" style="font-family:%s;font-size:%spx">%s'
                       '</span></div>'
                       % (esc(family), size, esc(family), size,
                          esc(SCRIPTS[0][2])))
    out.append('</div>')


def subpixel_phase_sections(out):
    """The same text at eight subpixel offsets, in fonts that treat them
    differently.

    Firefox on Windows stops positioning a font's glyphs at subpixel offsets
    when it is a CJK font carrying an embedded bitmap strike at that size -
    gfxDWriteFonts.cpp turns mUseSubpixelPositions off, and WebRender's
    is_bitmap_font() then zeroes the offsets outright. Linux has no such rule,
    so a run whose glyphs land on fractional pixels rendered differently on the
    two machines while a run that happened to start on a whole pixel did not.
    That is invisible in any page that does not deliberately put the same text
    at several fractional offsets, which is what this does.

    MS Gothic has strikes at 7-22px and is the case; Consolas has none at these
    sizes and is the control, since it must keep varying with the offset. A
    machine where the two behave alike has lost the distinction in one
    direction or the other.
    """
    out.append('<h2>subpixel phase: snapping fonts against non-snapping ones</h2>')
    out.append('<div class="surface" style="background:#ffffff;color:#000000">')
    japanese = SCRIPTS_BY_LABEL.get("Japanese", "AaGg")
    for family, sample in (("MS Gothic", japanese), ("MS Mincho", japanese),
                           ("Consolas", "Handgloves 123"),
                           ("Arial", "Handgloves 123")):
        for size in (13, 14, 16):
            for step in range(8):
                offset = step * 0.125
                out.append(
                    '<div class="row"><span class="lbl">%s %dpx +%.3f</span>'
                    '<span class="spec" style="font-family:%s;font-size:%dpx;'
                    'margin-left:%.3fpx">%s</span></div>'
                    % (esc(family), size, offset, esc(family), size, offset,
                       esc(sample)))
    out.append('</div>')


def matrix_section(out):
    """A dense grid: every text color against every background, one short run
    each, so a whole-page diff has somewhere to point."""
    out.append('<h2>color x background matrix</h2>')
    out.append('<table class="grid"><tr><th>background \\ color</th>')
    colors = [c for _, _, c in SURFACES]
    for color in colors:
        out.append('<th>%s</th>' % esc(color))
    out.append('</tr>')
    for name, background, _ in SURFACES:
        out.append('<tr><th>%s</th>' % esc(name))
        for color in colors:
            out.append('<td style="background:%s;color:%s">Handgloves 123</td>'
                       % (background, color))
        out.append('</tr>')
    out.append('</table>')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())

    out = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
           '<title>text rendering stress page</title>',
           '<style>%s</style></head><body>' % CSS,
           '<h2>generated by tools/testing/gen_stress_page.py</h2>',
           '<div class="cap">no scripts, no webfonts, no network - every cell '
           'labeled with the parameters that produced it</div>']
    surface_sections(out)
    size_sections(out)
    style_sections(out)
    script_sections(out)
    default_size_sections(out)
    fractional_size_sections(out)
    subpixel_phase_sections(out)
    matrix_section(out)
    out.append('</body></html>')

    html = "\n".join(out)
    open(sys.argv[1], "w", encoding="utf-8").write(html)
    print("wrote %s, %d bytes" % (sys.argv[1], len(html.encode("utf-8"))))


if __name__ == "__main__":
    main()
