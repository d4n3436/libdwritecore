#!/usr/bin/env python3
"""
gen_firefox_parity.py - regenerate and check the Firefox parity layer's source
data against a Firefox source tree.

    tools/gen_firefox_parity.py --tree PATH [--check | --write]
    tools/gen_firefox_parity.py --fetch --tree PATH [--write]

The parity code in cleartype/src/freetype.cpp is a hand translation of
Firefox's Windows text pipeline, and two kinds of thing in it come straight
out of the Firefox tree:

  data    pref defaults, the bad-underline family list, WebRender's thread
          names, the Windows per-script fallback lists, the FontID pref names.
          These are regenerated here, into cleartype/src/firefox_parity_data.h and
          the prefs the shim hands Firefox through MOZ_DEFAULT_PREFS.

  logic   the arithmetic of gfxDWriteFont::ComputeMetrics, gfxFT2FontBase::
          InitMetrics, SanitizeMetrics, nsFontMetrics, platform/{windows,unix}/
          font.rs, SkScalerContext_DW and the rest. That cannot be generated -
          the shim is an inverse of two implementations - so each translated
          site is pinned to the verbatim source lines it was translated from
          (ANCHORS below). A tree in which an anchor no longer occurs has
          changed something the translation depends on, and this tool fails
          on it instead of letting the shim drift silently.

--check (the default) verifies the anchors and that the generated files on
disk match what the tree produces; --write regenerates them.

--fetch downloads the 27 files listed in SOURCE_FILES below and nothing else,
straight from raw.githubusercontent.com, into the --tree directory. That is
every file this tool opens: the reads all go through read() and there is no
globbing anywhere, so the set is closed and can be named.

Fetching is by commit, not by tag: a tag can be moved, a commit cannot, so
what comes down is the revision the shim was actually translated from. Which
commit that is comes from the generated header, which stamps its own
provenance - so the pin lives with the data it describes, and regenerating
against a newer Firefox moves both at once. The files land in the tree's own
layout, so --tree keeps working unchanged against a real Firefox checkout.

To ask whether Firefox has moved, name a newer revision: --fetch main, or
--fetch FIREFOX_155_0_RELEASE. Anchors that no longer occur are reported and
the exit status is 1, which means the hand translation needs looking at and
not that anything is broken.

Exit status: 0 when everything matches, 1 otherwise.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import typing
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM_DIR = os.path.join(ROOT, "cleartype", "src")
DATA_HEADER = os.path.join(SHIM_DIR, "firefox_parity_data.h")

# Only for a first generation, when no generated file carries a revision yet.
# Every other run takes the revision from the header it is checking - see
# pinned_revision() - so there is no second constant here to fall out of step
# with the data.
DEFAULT_TAG = "FIREFOX_154_0_RELEASE"
GITHUB_RAW = "https://raw.githubusercontent.com/mozilla-firefox/firefox"
GITHUB_API = "https://api.github.com/repos/mozilla-firefox/firefox"

# Every file this tool opens - the 23 that carry anchors, then the four the
# generators read and nothing anchors. --fetch downloads exactly these, and
# check_sources() below fails if an anchor ever names a file missing here.
SOURCE_FILES = [
    "gfx/thebes/gfxDWriteFonts.cpp",
    "gfx/thebes/gfxDWriteFontList.cpp",
    "gfx/thebes/gfxFT2FontBase.cpp",
    "gfx/thebes/gfxFont.cpp",
    "gfx/thebes/gfxHarfBuzzShaper.cpp",
    "gfx/src/nsFontMetrics.cpp",
    "gfx/thebes/gfxFcPlatformFontList.cpp",
    "gfx/2d/ScaledFontDWrite.cpp",
    "gfx/2d/ScaledFontFontconfig.cpp",
    "gfx/2d/DWriteSettings.cpp",
    "gfx/2d/DrawTargetSkia.cpp",
    "gfx/wr/wr_glyph_rasterizer/src/platform/windows/font.rs",
    "gfx/wr/wr_glyph_rasterizer/src/platform/unix/font.rs",
    "gfx/wr/wr_glyph_rasterizer/src/rasterizer.rs",
    "gfx/wr/wr_glyph_rasterizer/src/gamma_lut.rs",
    "gfx/wr/webrender_api/src/font.rs",
    "gfx/skia/skia/src/ports/SkScalerContext_win_dw.cpp",
    "gfx/skia/skia/src/ports/SkTypeface_win_dw.cpp",
    "gfx/skia/skia/src/ports/SkFontHost_cairo.cpp",
    "gfx/webrender_bindings/src/bindings.rs",
    "gfx/wr/webrender/src/renderer/init.rs",
    "widget/windows/nsLookAndFeel.cpp",
    "widget/nsXPLookAndFeel.cpp",
    # Read by the generators; no anchors, because what is extracted from them
    # is data and not translated logic.
    "gfx/thebes/gfxWindowsPlatform.cpp",
    "intl/components/src/UnicodeScriptCodes.h",
    "modules/libpref/init/StaticPrefList.yaml",
    "modules/libpref/init/all.js",
]

# Written into a fetched directory: which commit it holds, and the digest of
# every file, so a half-finished download is not silently reused.
FETCH_MARKER = ".firefox-parity-fetch.json"

# ---------------------------------------------------------------------------
# Anchors: (file, [verbatim fragments]). A fragment is matched as a substring
# of the file; each names, in the comment above it, the shim code that was
# translated from it. Keep them short enough to survive reformatting of the
# surrounding lines and specific enough to break when the arithmetic changes.
# ---------------------------------------------------------------------------
ANCHORS = [
    # ComputeWinInstanceLocked, WinMeasureGlyphWidth, WinGlyphAdvance,
    # ApplyWindowsAdvance, IsBitmapFontLocked, HasBitmapStrikeForSizeLocked
    ("gfx/thebes/gfxDWriteFonts.cpp", [
        "mFUnitsConvFactor = float(mAdjustedSize / fontMetrics.designUnitsPerEm);",
        "GetMeasuringMode() == DWRITE_MEASURING_MODE_NATURAL) ||",
        "if (fe->IsCJKFont() && HasBitmapStrikeForSize(NS_lround(mAdjustedSize))) {",
        "mAdjustedSize = NS_lround(mAdjustedSize);",
        "mMetrics.maxAscent = round(fontMetrics.ascent * mFUnitsConvFactor);",
        "mMetrics.maxDescent = round(fontMetrics.descent * mFUnitsConvFactor);",
        "mMetrics.emHeight * mMetrics.maxAscent / mMetrics.maxHeight;",
        "mMetrics.maxAdvance = uint16_t(hhea->advanceWidthMax) * mFUnitsConvFactor;",
        "std::max(mMetrics.maxHeight - mMetrics.emHeight, 0.0);",
        "mMetrics.externalLeading = ceil(fontMetrics.lineGap * mFUnitsConvFactor);",
        "mMetrics.aveCharWidth = int16_t(os2->xAvgCharWidth) * mFUnitsConvFactor;",
        "mMetrics.aveCharWidth = GetCharAdvance('x');",
        "mMetrics.underlineOffset = fontMetrics.underlinePosition * mFUnitsConvFactor;",
        "SanitizeMetrics(&mMetrics, GetFontEntry()->mIsBadUnderlineFont);",
        "hasStrike = (uint16_t(sizeTable->endGlyphIndex) >=",
        "uint16_t(sizeTable->startGlyphIndex) + 3);",
        "return !mUseSubpixelPositions ||",
        "return NS_lround(MeasureGlyphWidth(aGID) * 65536.0);",
        "return advance * mFUnitsConvFactor;",
        "return NS_lround(advance * mFUnitsConvFactor);",
        "GetMeasuringMode() == DWRITE_MEASURING_MODE_GDI_NATURAL, FALSE, 1,",
        "fe->IsCJKFont() && HasBitmapStrikeForSize(NS_lround(mAdjustedSize));",
        "return GetMeasuringMode() != DWRITE_MEASURING_MODE_NATURAL;",
        # UpdateClearTypeVars: the gamma/contrast defaults the prefs carry
        "float enhancedContrast = 1.0f;",
        "enhancedContrast = defaultRenderingParams->GetEnhancedContrast();",
        "gamma = defaultRenderingParams->GetGamma();",
        "renderingMode = defaultRenderingParams->GetRenderingMode();",
        # the forced Arial Black interception
        "mFontEntry->Name().EqualsLiteral(\"Arial Black\") &&",
        "style.weight = FontWeight::FromInt(700);",
        "nullptr, \"Arial\"_ns, &style);",
    ]),
    # gfxDWriteFontEntry::IsCJKFont, the bold-simulation choice, bad underline
    ("gfx/thebes/gfxDWriteFontList.cpp", [
        "(1 << 17) |  // codepage 932 - JIS/Japan",
        "(1 << 21);   // codepage 1361 - Korean Johab",
        "!mIsDataUserFont && !HasFontTable(TRUETYPE_TAG('C', 'O', 'L', 'R'));",
        "bool bad = mBadUnderlineFamilyNames.ContainsSorted(key);",
        "if (FAILED(GetDirectWriteFaceName(dwFont, PSNAME_ID, name)) ||",
        "\"gfx.font_rendering.cleartype_params.force_gdi_classic_max_size\",",
    ]),
    # The Linux consumer: ApplyWindowsMetrics, SubstituteOS2, SubstitutePost,
    # ApplyWindowsAdvance, LinuxUnderlineLocked
    ("gfx/thebes/gfxFT2FontBase.cpp", [
        "FT_Set_Char_Size(mFTFace->GetFace(), charSize, charSize, 0, 0);",
        "(int32_t)std::min(std::max(mFTSize * 64.0 + 0.5, 0.0),",
        "mMetrics.maxAscent = FLOAT_FROM_26_6(ftMetrics.ascender);",
        "mMetrics.maxDescent = -FLOAT_FROM_26_6(ftMetrics.descender);",
        "mMetrics.maxAdvance = FLOAT_FROM_26_6(ftMetrics.max_advance);",
        "yScale = FLOAT_FROM_26_6(FLOAT_FROM_16_16(ftMetrics.y_scale));",
        "mMetrics.emAscent = os2->sTypoAscender * yScale;",
        "os2->sTypoAscender - os2->sTypoDescender + os2->sTypoLineGap;",
        "const uint16_t kUseTypoMetricsMask = 1 << 7;",
        "mMetrics.maxAscent = NS_round(mMetrics.emAscent);",
        "if (face->underline_position && face->underline_thickness && yScale > 0.0) {",
        "mMetrics.underlineSize = face->underline_thickness * yScale;",
        "mMetrics.underlineOffset = post->underlinePosition * yScale;",
        "face->underline_position * yScale + 0.5 * mMetrics.underlineSize;",
        "mMetrics.underlineSize = emHeight / 14.0;",
        "ScaleRoundDesignUnits(os2->xAvgCharWidth, ftMetrics.x_scale);",
        "mMetrics.emHeight = floor(emHeight + 0.5);",
        "floor(mMetrics.maxHeight - mMetrics.emHeight + 0.5);",
        "lineHeight = floor(std::max(lineHeight, mMetrics.maxHeight) + 0.5);",
        "sum > 0.0 ? mMetrics.emAscent * mMetrics.emHeight / sum : 0.0;",
        "SanitizeMetrics(&mMetrics, false);",
        "advance = face.get()->glyph->linearHoriAdvance;",
        "*aAdvance = NS_lround(advance * extentsScale);",
        "!((mFTLoadFlags & FT_LOAD_NO_HINTING) ||",
    ]),
    # WinSanitizeMetrics
    ("gfx/thebes/gfxFont.cpp", [
        "aMetrics->underlineSize = std::max(1.0, aMetrics->underlineSize);",
        "aMetrics->underlineOffset = std::min(aMetrics->underlineOffset, -1.0);",
        "if (!mStyle.systemFont && aIsBadUnderlineFont) {",
        "aMetrics->underlineOffset = std::min(aMetrics->underlineOffset, -2.0);",
        "std::min(aMetrics->underlineOffset, -aMetrics->emDescent);",
        "aMetrics->underlineSize - aMetrics->emDescent);",
        "else if (aMetrics->underlineSize - aMetrics->underlineOffset >",
        "aMetrics->underlineOffset = aMetrics->underlineSize - aMetrics->maxDescent;",
        "gfxFloat halfOfStrikeoutSize = floor(aMetrics->strikeoutSize / 2.0 + 0.5);",
        "aMetrics->strikeoutOffset = std::max(halfOfStrikeoutSize, ascent / 2.0);",
        "if (aMetrics->underlineSize > aMetrics->maxAscent) {",
        "if (!AllowSubpixelAA()) {",
    ]),
    # WinGlyphAdvance, hb path
    ("gfx/thebes/gfxHarfBuzzShaper.cpp", [
        "return FloatToFixed(mFont->FUnitsToDevUnitsFactor() *",
    ]),
    # ComputeMaxDescent, the descent fold
    ("gfx/src/nsFontMetrics.cpp", [
        "gfxFloat offset = floor(-aFontGroup->GetUnderlineOffset() + 0.5);",
        "gfxFloat size = NS_round(aMetrics.underlineSize);",
        "return floor(std::max(minDescent, aMetrics.maxDescent) + 0.5);",
        "return floor(aMetrics.maxAscent + 0.5);",
        "gfxFloat(aFont.size.ToAppUnits()) / mP2A",
    ]),
    # Linux never sets the bad-underline flag
    ("gfx/thebes/gfxFcPlatformFontList.cpp", [
        "/*bundled*/ aAppFont, /*badUnderline*/ false));",
        "loadFlags = FT_LOAD_NO_HINTING;",
    ]),
    # The WebRender FontInstance Windows builds; the prefs' gamma/contrast
    ("gfx/2d/ScaledFontDWrite.cpp", [
        "options.flags |= wr::FontInstanceFlags::SYNTHETIC_BOLD;",
        "options.flags |= wr::FontInstanceFlags::MULTISTRIKE_BOLD;",
        "options.flags |= wr::FontInstanceFlags::EMBEDDED_BITMAPS;",
        "options.flags |= wr::FontInstanceFlags::SUBPIXEL_POSITION;",
        "options.flags |= wr::FontInstanceFlags::FORCE_SYMMETRIC;",
        "options.flags |= wr::FontInstanceFlags::NO_SYMMETRIC;",
        "platformOptions.gamma = uint16_t(std::round(settings.Gamma() * 100.0f));",
        "uint8_t(std::round(std::min(settings.EnhancedContrast(), 1.0f) * 100.0f));",
        "aFont.setEmbeddedBitmaps(UseEmbeddedBitmaps());",
        "aFont.setSubpixel(true);",
    ]),
    ("gfx/2d/ScaledFontFontconfig.cpp", [
        "(mInstanceData.mHinting == FontHinting::NONE ||",
        "int16_t(StaticPrefs::gfx_font_rendering_freetype_gamma());",
        "int16_t(StaticPrefs::gfx_font_rendering_freetype_enhanced_contrast());",
    ]),
    # DrawTargetSkia::UpdateSurfaceProps, the Linux counterpart of those
    # initialisers: one pair for every Skia consumer, and gamma and contrast
    # clamped to different bounds. GammaAsScalar and ContrastAsScalar in
    # cleartype/src/libxul_patch.cpp are these two lines.
    ("gfx/2d/DrawTargetSkia.cpp", [
        "gamma = SkScalar(std::min(gammaVal, 400)) / 100;",
        "contrast = SkScalar(std::min(contrastVal, 100)) / 100;",
    ]),
    ("gfx/2d/DWriteSettings.cpp", [
        "sMeasuringMode = DWRITE_MEASURING_MODE_NATURAL;",
        "return mUseGDISettings ? DWRITE_MEASURING_MODE_GDI_CLASSIC : sMeasuringMode;",
        # The initialisers, not only what overwrites them. A consumer reached
        # before gfxVars deliver the system values reads these instead.
        "static std::atomic<Float> sClearTypeLevel{1.0f};",
        "static std::atomic<Float> sEnhancedContrast{1.0f};",
        "static std::atomic<Float> sGamma{2.2f};",
        "static std::atomic<Float> sGDIGamma{1.4f};",
    ]),
    # RasterizeThroughDWrite, WebRender branch
    ("gfx/wr/wr_glyph_rasterizer/src/platform/windows/font.rs", [
        "font_face.get_recommended_rendering_mode_default_params(em_size, 1.0, measure_mode)",
        "return dwrote::DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;",
        "if bitmaps || font.flags.contains(FontInstanceFlags::FORCE_GDI) {",
        "font.render_mode != FontRenderMode::Mono &&",
        "font.flags.contains(FontInstanceFlags::EMBEDDED_BITMAPS)",
        "(FontTransform::identity(), (0.0, 0.0))",
        "let analysis2 = dwrote::GlyphRunAnalysis::create(",
        "// Only take the G channel, as its closest to D2D",
        "(_, FontRenderMode::Subpixel, false) => {",
        "gamma_lut.preblend_scaled(&mut bgra_pixels, font.color, cleartype_level);",
        "gamma_lut.preblend(&mut bgra_pixels, font.color);",
        "dwrote::DWRITE_FONT_SIMULATIONS_BOLD",
    ]),
    # The Linux consumer of the rasterizer's output
    ("gfx/wr/wr_glyph_rasterizer/src/platform/unix/font.rs", [
        "(req_size * x_scale * 64.0 + 0.5) as FT_F26Dot6,",
        "dx - ((cbox.xMin + dx) & !63),",
        "size_.metrics.y_scale) / 48 };",
        "final_buffer[dest + 3] = max(max(b, g), r);",
        "if gamma < 0 && enhanced_contrast <= 0 {",
        "gamma_lut.preblend_grayscale(pixels, font.color);",
        "(FT_Pixel_Mode::FT_PIXEL_MODE_LCD, _) |",
        "FT_Render_Mode::FT_RENDER_MODE_NORMAL,",
    ]),
    # WinExtraStrikes, BlendStrikePixel, subpixel quantization
    ("gfx/wr/wr_glyph_rasterizer/src/rasterizer.rs", [
        "let mut bold_offset = self.size.to_f64_px() / 48.0;",
        "bold_offset = 0.25 + 0.75 * bold_offset;",
        "(bold_offset * x_scale).max(1.0).round() as usize",
        "let x = src * 255 + dest as u32 * (255 - src_alpha) + 128;",
        "let apos = ((pos - pos.floor()) * 8.0) as i32;",
    ]),
    ("gfx/wr/wr_glyph_rasterizer/src/gamma_lut.rs", [
        "pixel[3] = max(max(b, g), r);",
        "if percent >= 100 {",
        "let val: u32 = r as u32 * 54 + g as u32 * 183 + b as u32 * 19;",
    ]),
    ("gfx/wr/webrender_api/src/font.rs", [
        "gamma: 180, // Default DWrite gamma",
        "gamma: -1,",
        "pub const ANGLE_SCALE: f32 = 256.0;",
    ]),
    # SkiaDWParamsLocked
    ("gfx/skia/skia/src/ports/SkScalerContext_win_dw.cpp", [
        "SkScalar gdiTextSize = SkScalarRoundToScalar(realTextSize * 64.0f) / 64.0f;",
        "} else if (realTextSize > SkIntToScalar(20) ||",
        "get_gasp_range(typeface, SkScalarRoundToInt(gdiTextSize), &range) &&",
        "fRenderingMode = !range.fFlags.field.SymmetricSmoothing ?",
        "fAntiAliasMode = DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;",
        "fGridFitMode = DWRITE_GRID_FIT_MODE_ENABLED;",
        "return (0 != maxp->version.tt.maxSizeOfInstructions);",
        "if (sizeTable->endGlyphIndex >= sizeTable->startGlyphIndex + 3) {",
        "// Ignore the R, B channels. It looks the closest to what",
    ]),
    ("gfx/skia/skia/src/ports/SkTypeface_win_dw.cpp", [
        "h = SkFontHinting::kNormal;",
    ]),
    ("gfx/skia/skia/src/ports/SkFontHost_cairo.cpp", [
        "FT_Set_Char_Size(fFTFace, FT_F26Dot6(fScaleX * 64.0f + 0.5f),",
        "mozilla_glyphslot_embolden_less(glyph);",
        # the forced FT_Outline_Get_CBox interception
        "FT_Outline_Get_CBox(&fFTFace->glyph->outline, &bbox);",
        "bbox.xMax = (bbox.xMax + 63) & ~63;",
        "bounds.outset(1, 0);",
    ]),
    # CurrentRasterCaller
    ("gfx/webrender_bindings/src/bindings.rs", [
        "format!(\"WRWorker{}#{}\", priority_tag, idx)",
        "gecko_profiler::register_thread(\"WrGlyphRasterizer\");",
    ]),
    ("gfx/wr/webrender/src/renderer/init.rs", [
        "format!(\"WRRenderBackend#{}\"",
    ]),
    # The prefs' ui.font values and names
    ("widget/windows/nsLookAndFeel.cpp", [
        "float pixelHeight = -aLogFont.lfHeight;",
        "result.name() = u\"MS Shell Dlg 2\"_ns;",
        "result = GetLookAndFeelFontInternal(ncm.lfMessageFont, true);",
    ]),
    ("widget/nsXPLookAndFeel.cpp", [
        "font.size() = StyleFONT_MEDIUM_PX;",
        "static const char sFontPrefs[][41] = {",
    ]),
]

# Thread names whose first seven characters survive NS_SetCurrentThreadName's
# 15-byte abbreviation; the literals are anchored above.
WEBRENDER_THREAD_NAMES = ["WRWorker", "WrGlyphRasterizer", "WRRenderBackend"]

# widget/windows/nsLookAndFeel.cpp GetLookAndFeelFont on a Windows 11 install
# at 100% scale with the default NONCLIENTMETRICS (Segoe UI, lfHeight -12,
# FW_NORMAL). These are system values, not Firefox source.
SYSTEM_FONT_NAME = "Segoe UI"
SYSTEM_FONT_SIZE = "12"
SYSTEM_FONT_WEIGHT = "400"
SHELL_DLG_IDS = ("-moz-button", "-moz-list", "-moz-field")
SHELL_DLG_NAME = "MS Shell Dlg 2"

# gfxDWriteFont::UpdateClearTypeVars on a machine without ClearType Tuner
# registry overrides: gamma from IDWriteFactory::CreateRenderingParams (the
# DirectWrite default, 1.8), enhanced contrast the hard-coded 1.0 (the
# DISPLAY1 EnhancedContrastLevel value is absent), on
# ScaledFontDWrite::GetWRFontInstanceOptions' percent scale.
FREETYPE_GAMMA = 180
FREETYPE_ENHANCED_CONTRAST = 100

# The initialisers in gfx/2d/DWriteSettings.cpp, which UpdateGamma and
# UpdateEnhancedContrast replace from gfxVars. A consumer reached after that
# sees the pair above; Firefox's blob rasterizer is reached before it and sees
# this one, which is why SVG text is not the gamma the rest of a page is.
DWRITE_SETTINGS_GAMMA = 220
DWRITE_SETTINGS_ENHANCED_CONTRAST = 100


def die(msg) -> typing.NoReturn:
    print("gen_firefox_parity: " + msg, file=sys.stderr)
    sys.exit(1)


def read(tree, rel):
    path = os.path.join(tree, rel)
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return text


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

def marker_revision(tree):
    """The revision a --fetch left behind, in the same shape git reports."""
    try:
        with open(os.path.join(tree, FETCH_MARKER), encoding="utf-8") as f:
            got = json.load(f)
    except (OSError, ValueError):
        return None
    tag, rev = got.get("tag", ""), got.get("rev", "")
    if not rev:
        return None
    return ("%s (%s)" % (tag, rev)) if tag else rev


def sourcestamp_revision(tree):
    """The revision a release source tarball names in sourcestamp.txt.

    A tarball carries no VCS metadata, so the changeset the file records is
    the only provenance it has. It is a Mercurial changeset and the marker a
    --fetch leaves is a git commit, so the branch is kept in the text to say
    which of the two a reader is looking at.
    """
    try:
        with open(os.path.join(tree, "sourcestamp.txt"), encoding="utf-8") as f:
            lines = f.read().split()
    except OSError:
        return None
    for line in lines:
        m = re.match(r"https://hg\.mozilla\.org/(?:releases/)?([^/]+)/rev/([0-9a-f]{12,})",
                     line)
        if m:
            return "hg %s %s" % (m.group(1), m.group(2))
    return None


def tree_revision(tree):
    from_marker = marker_revision(tree)
    if from_marker:
        return from_marker
    from_stamp = sourcestamp_revision(tree)
    if from_stamp:
        return from_stamp
    try:
        sha = subprocess.run(["git", "-C", tree, "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        tags = subprocess.run(["git", "-C", tree, "tag", "--points-at", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.split()
        release = [t for t in tags if re.match(r"FIREFOX_\d+_\d+(_\d+)?_RELEASE$", t)]
        name = release[0] if release else (tags[0] if tags else "")
        return ("%s (%s)" % (name, sha)) if name else sha
    except Exception:
        return "unknown revision"


# What a font name list says when it names no family at all.
#
# gfxFontUtils::ParseFontList splits on commas and drops every name that is
# empty once whitespace is compressed, so a lone comma parses to an empty
# family list. An empty string does not work here. gfxFcPlatformFontList::
# AddGenericFonts reads an empty value as "ask fontconfig for this generic"
# and answers with whatever fontconfig offers, where Windows runs the base
# class and adds nothing. Any non-empty value other than serif, sans-serif,
# monospace or math sends Linux down that same base-class path.
EMPTY_FONT_LIST = ","


# The platform macros all.js branches on, as each build would define them.
PLATFORM_DEFINES = {
    "win": {"XP_WIN": True},
    "linux": {"XP_UNIX": True, "MOZ_WIDGET_GTK": True},
}


# Conditions that are not about the platform - build channel, and the like.
# They read the same on both builds, so they cannot create a difference between
# them; taken as false for both, and fatal only if a font pref turns out to be
# inside one, which would mean this is guessing about something that matters.
UNREADABLE = object()


def _eval_cpp(expr, defines):
    """A preprocessor condition, over defined()/!/&&/||/parentheses. Anything
    else comes back as UNREADABLE, never as a guess."""
    tokens = re.findall(r'defined\s*\(\s*(\w+)\s*\)|(\w+)|(&&|\|\||!|\(|\))|(\S)', expr)
    out = []
    for name, bare, op, junk in tokens:
        if name:
            out.append("True" if defines.get(name) else "False")
        elif bare:
            # A bare identifier in these conditions is a macro test too.
            out.append("True" if defines.get(bare) else "False")
        elif op:
            out.append({"&&": " and ", "||": " or ", "!": " not "}.get(op, op))
        else:
            return UNREADABLE
    try:
        return bool(eval("".join(out), {"__builtins__": {}}, {}))
    except Exception:
        return UNREADABLE


def font_prefs_by_platform(all_js):
    """Every font.* pref all.js sets, evaluated the way each build's
    preprocessor would, and returned where the two disagree.

    Reading only the `#ifdef XP_WIN` blocks is not enough: some of what Windows
    uses is the file's unguarded default, which the Linux block then overrides -
    font.size.monospace.x-western is 13 at the top of the file and 12 inside
    `#if !defined(ANDROID) && !defined(XP_MACOSX) && defined(XP_UNIX)`. A pref
    like that appears in no Windows block at all, and is exactly the kind of
    difference this file exists to undo.

    A pref Linux sets and Windows never sets is the same kind of difference.
    ResolveGenericFontNames looks the name up and adds nothing when it is
    missing, so on Windows that generic resolves to no family and the text
    falls through to the default font. The Linux value has to say the same
    thing, and it is written as a lone comma, EMPTY_FONT_LIST below.

    Only the name lists are given a value this way. A Linux-only pref of any
    other kind would need a real one and stops the build.
    """
    values = {"win": {}, "linux": {}}
    order = []
    stacks = {"win": [], "linux": []}
    unreadable = []

    for raw in all_js.splitlines():
        line = raw.strip()
        if line.startswith("#"):
            directive = line.split(None, 1)
            kind = directive[0]
            rest = directive[1] if len(directive) > 1 else ""
            rest = re.sub(r'//.*$', "", rest).strip()
            for platform, defines in PLATFORM_DEFINES.items():
                stack = stacks[platform]
                if kind == "#ifdef":
                    stack.append(bool(defines.get(rest)))
                elif kind == "#ifndef":
                    stack.append(not defines.get(rest))
                elif kind == "#if":
                    v = _eval_cpp(rest, defines)
                    if v is UNREADABLE:
                        unreadable.append(rest)
                        v = False
                    stack.append(v)
                elif kind == "#elif":
                    v = _eval_cpp(rest, defines)
                    if v is UNREADABLE:
                        unreadable.append(rest)
                        v = False
                    if stack:
                        stack[-1] = (not stack[-1]) and v
                elif kind == "#else":
                    if stack:
                        stack[-1] = not stack[-1]
                elif kind == "#endif":
                    if stack:
                        stack.pop()
                        if unreadable:
                            unreadable.pop()
            continue

        m = re.match(r'(?:sticky_)?pref\("(font\.[^"]+)"\s*,\s*(.*?)\);\s*(?://.*)?$', line)
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip()
        if unreadable:
            die("all.js: %s is inside a condition this cannot read (%r)"
                % (name, unreadable[-1]))
        for platform in values:
            if all(stacks[platform]):
                if name not in values[platform]:
                    order.append(name)
                values[platform][name] = value

    if not values["win"]:
        die("all.js: no font prefs found at all")

    seen = set()
    differing = []
    for name in order:
        if name in seen:
            continue
        seen.add(name)
        win = values["win"].get(name)
        linux = values["linux"].get(name)
        if win is not None:
            if win != linux:
                # An empty list Windows writes itself means the same thing as
                # one it never wrote, and reads the same way on Linux.
                if win == '""' and name.startswith(("font.name.", "font.name-list.")):
                    win = '"%s"' % EMPTY_FONT_LIST
                differing.append('"%s", %s' % (name, win))
        elif linux is not None:
            if not name.startswith(("font.name.", "font.name-list.")):
                die("all.js: %s is set on Linux and not on Windows, and it is "
                    "not a font name list, so there is no empty value to give "
                    "it" % name)
            differing.append('"%s", "%s"' % (name, EMPTY_FONT_LIST))
    if not differing:
        die("all.js: Windows and Linux agree on every font pref, which cannot be right")
    return differing


def bad_underline_families(all_js):
    m = re.search(r'pref\("font\.blacklist\.underline_offset",\s*"([^"]*)"\)', all_js)
    if not m:
        die("all.js: font.blacklist.underline_offset not found")
    return [s.strip() for s in m.group(1).split(",") if s.strip()]


def font_pref_ids(xp_lookandfeel):
    m = re.search(r'static const char sFontPrefs\[\]\[41\] = \{(.*?)\};', xp_lookandfeel, re.S)
    if not m:
        die("nsXPLookAndFeel.cpp: sFontPrefs not found")
    names = re.findall(r'"ui\.font\.([^"]+)"', m.group(1))
    if not names:
        die("nsXPLookAndFeel.cpp: no ui.font names")
    return names


def static_pref_value(yaml, name):
    m = re.search(r'- name: %s\n(.*?)\n\n' % re.escape(name), yaml, re.S)
    if not m:
        die("StaticPrefList.yaml: %s not found" % name)
    v = re.search(r'value: (\S+)', m.group(1))
    if not v:
        die("StaticPrefList.yaml: %s has no value" % name)
    return v.group(1)


def windows_static_pref(yaml, name):
    """A StaticPrefList default as a Windows build would compile it.

    static_pref_value above takes the one value in a block, which serves a
    pref with a single default. This one is for a default written per platform,
    where the first value in the block is whichever branch the file puts first.

    One level of conditional, which is all any block in the file uses today.
    A nested one is fatal.
    """
    m = re.search(r'- name: %s\n(.*?)\n\n' % re.escape(name), yaml, re.S)
    if not m:
        die("StaticPrefList.yaml: %s not found" % name)
    keep = True
    depth = 0
    found = None
    for line in m.group(1).splitlines():
        head = line.split(None, 1)
        word = head[0] if head else ""
        if word in ("#if", "#ifdef", "#ifndef"):
            depth += 1
            if depth > 1:
                die("StaticPrefList.yaml: %s is inside nested conditions, and "
                    "this reads one level" % name)
            expr = head[1] if len(head) > 1 else ""
            if word == "#ifdef":
                expr = "defined(%s)" % expr
            elif word == "#ifndef":
                expr = "!defined(%s)" % expr
            keep = _eval_cpp(expr, PLATFORM_DEFINES["win"])
            if keep is UNREADABLE:
                die("StaticPrefList.yaml: %s is inside a condition this cannot "
                    "read (%r)" % (name, expr))
        elif word == "#else":
            keep = not keep
        elif word == "#endif":
            depth -= 1
            keep = True
        elif keep:
            v = re.match(r"\s*value: (\S+)", line)
            if v:
                found = v.group(1)
    if found is None:
        die("StaticPrefList.yaml: %s has no value a Windows build would take" % name)
    return found


def script_codes(header):
    """Script:: name -> value, from the generated UnicodeScriptCodes.h. The
    numbering is ICU's and only ever grows, which is what lets the patched
    code compare against a Firefox built from a different tree."""
    codes = {}
    for name, value in re.findall(r'^\s*([A-Z][A-Z0-9_]*)\s*=\s*(\d+),', header, re.M):
        codes[name] = int(value)
    if codes.get("COMMON") != 0 or "HAN" not in codes:
        die("UnicodeScriptCodes.h: not the Script enum this expects")
    return codes


# The conditions gfxWindowsPlatform::GetCommonFallbackFonts guards its
# AppendElement calls with. Each is recognized verbatim; an unrecognized one
# is fatal, so a Firefox that adds a condition breaks here instead of being
# silently flattened into an unconditional list.
CONDITIONS = {
    "": 0,                                            # kFallbackAlways
    "PrefersColor(aPresentation)": 1,                 # kFallbackColor
    "!PrefersColor(aPresentation)": 2,                # kFallbackNotColor
    "aCh > 0xFFFF": 3,                                # kFallbackSupplementary
}
COND_PUNCT_OR_COMMON = 4    # the aRunScript == COMMON || symbol || punctuation test

PUNCT_CONDITION = "aRunScript == Script::COMMON"


def _appends(text):
    """(family, condition) for each AppendElement in source order, tracking the
    `if` each one sits inside. Conditions that run over several lines are
    joined first, so a statement on the line after one is not swallowed."""
    lines = []
    pending = ""
    for raw in text.splitlines():
        line = re.sub(r'//.*$', "", raw).strip()
        if not line:
            continue
        pending = (pending + " " + line).strip() if pending else line
        if pending.count("(") > pending.count(")"):
            continue                      # a condition still open
        lines.append(pending)
        pending = ""

    out = []
    stack = []          # (condition tag, brace depth it opened at)
    depth = 0
    for line in lines:
        m = re.match(r'if \((.*)\)\s*\{$', line)
        if m:
            cond = re.sub(r'\s+', " ", m.group(1)).strip()
            if cond.startswith(PUNCT_CONDITION):
                tag = COND_PUNCT_OR_COMMON
            elif cond in CONDITIONS:
                tag = CONDITIONS[cond]
            else:
                die("GetCommonFallbackFonts: unrecognized condition %r" % cond)
            depth += 1
            stack.append((tag, depth))
            continue
        am = re.match(r'aFontList\.AppendElement\("([^"]+)"\);', line)
        if am:
            out.append((am.group(1), stack[-1][0] if stack else 0))
            continue
        depth += line.count("{") - line.count("}")
        while stack and depth < stack[-1][1]:
            stack.pop()
    return out


def windows_common_fallback(win_platform):
    """gfxWindowsPlatform::GetCommonFallbackFonts: the per-script lists and the
    entries appended around the switch, each with the condition it is under."""
    m = re.search(r'void gfxWindowsPlatform::GetCommonFallbackFonts\((.*?)\n\}\n', win_platform, re.S)
    if not m:
        die("gfxWindowsPlatform.cpp: GetCommonFallbackFonts not found")
    body = m.group(1)
    before, rest = body.split("switch (aRunScript) {", 1)
    depth = 1
    i = 0
    while depth and i < len(rest):
        if rest[i] == "{":
            depth += 1
        elif rest[i] == "}":
            depth -= 1
        i += 1
    switch_body, tail = rest[:i - 1], rest[i:]

    entries = []
    scripts = []
    chunk = []
    for line in switch_body.splitlines():
        s = re.sub(r'//.*$', "", line).strip()
        cm = re.match(r'case Script::([A-Z0-9_]+):', s)
        if cm:
            if chunk:
                entries.append((tuple(scripts), _appends("\n".join(chunk))))
                chunk = []
                scripts = []
            scripts.append(cm.group(1))
            continue
        if s.startswith("break;"):
            if chunk:
                entries.append((tuple(scripts), _appends("\n".join(chunk))))
            chunk = []
            scripts = []
            continue
        chunk.append(s)
    if chunk:
        entries.append((tuple(scripts), _appends("\n".join(chunk))))
    entries = [(sc, fams) for sc, fams in entries if fams]
    return _appends(before), entries, _appends(tail)


# ---------------------------------------------------------------------------
# Generated outputs
# ---------------------------------------------------------------------------

def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def prefs_as_c_string(prefs_text):
    """The pref lines of the generated prefs file, as a C string literal.

    MOZ_DEFAULT_PREFS is parsed as pref *data*, not a file name
    (modules/libpref/Preferences.cpp: parsePrefData(nsCString(getenv(...)),
    PrefValueKind::Default)), so the shim can hand Firefox the whole set
    through the environment and no file has to be installed anywhere. It is a
    defaults load, which takes `pref` rather than `user_pref`
    (modules/libpref/parser/src/lib.rs).
    """
    lines = [l for l in prefs_text.splitlines() if l.startswith("user_pref(")]
    if not lines:
        die("no prefs to embed")
    out = []
    for line in lines:
        line = "pref(" + line[len("user_pref("):]
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "' + escaped + '\\n"')
    return "\n".join(out)


def gen_header(rev, bad_underline, thread_names, bold_sim, fallback, codes, prefs_text):
    before, entries, tail = fallback
    # Built up by appending, because what follows is interleaved with the
    # loops that read the Firefox tree.
    # noinspection PyListCreation
    out = []
    out.append("// firefox_parity_data.h - GENERATED by tools/gen_firefox_parity.py from the")
    out.append("// Firefox source tree at %s. Do not edit by hand." % rev)
    out.append("//")
    out.append("// Data the parity layer of freetype.cpp takes verbatim from")
    out.append("// Firefox; the file naming each table is given above it.")
    out.append("")
    out.append("#ifndef FIREFOX_PARITY_DATA_H_INCLUDED")
    out.append("#define FIREFOX_PARITY_DATA_H_INCLUDED")
    out.append("")
    out.append("namespace firefox_parity {")
    out.append("")
    out.append("// modules/libpref/init/all.js font.blacklist.underline_offset, the list")
    out.append("// gfxPlatformFontList::LoadBadUnderlineList loads and gfxDWriteFontList")
    out.append("// applies to a family (gfxFcPlatformFontList never does).")
    out.append("constexpr const char* kBadUnderlineFamilies[] = {")
    for fam in bad_underline:
        out.append("    %s," % c_string(fam))
    out.append("};")
    out.append("constexpr unsigned kBadUnderlineFamilyCount = %d;" % len(bad_underline))
    out.append("")
    out.append("// The threads WebRender rasterizes glyphs on: gfx/webrender_bindings/src/")
    out.append("// bindings.rs (\"WRWorker<tag>#<n>\", \"WrGlyphRasterizer\") and gfx/wr/webrender/")
    out.append("// src/renderer/init.rs (\"WRRenderBackend#<n>\"). Gecko's NS_SetCurrentThreadName")
    out.append("// keeps the first seven characters of a name longer than 15 bytes, so the")
    out.append("// first seven are what a thread's comm is matched on.")
    out.append("constexpr const char* kWebRenderThreadPrefixes[] = {")
    for name in thread_names:
        out.append("    %s," % c_string(name[:7]))
    out.append("};")
    out.append("constexpr unsigned kWebRenderThreadPrefixCount = %d;" % len(thread_names))
    out.append("")
    out.append("// The two gamma pairs a page is drawn with, on")
    out.append("// ScaledFontDWrite::GetWRFontInstanceOptions' percent scale.")
    out.append("//")
    out.append("// kPage is the system values, which reach WebRender's glyph rasterizer and")
    out.append("// every Skia consumer through gfx.font_rendering.freetype.*. kBlob is")
    out.append("// gfx/2d/DWriteSettings.cpp's own initialisers, sGamma{2.2f} and")
    out.append("// sEnhancedContrast{1.0f}, which Firefox's blob rasterizer is reached")
    out.append("// before gfxVars replace. DrawTargetSkia::UpdateSurfaceProps has no such")
    out.append("// split on Linux, where the prefs are in place before anything draws.")
    out.append("constexpr int kPageGamma = %d;" % FREETYPE_GAMMA)
    out.append("constexpr int kPageEnhancedContrast = %d;" % FREETYPE_ENHANCED_CONTRAST)
    out.append("constexpr int kBlobGamma = %d;" % DWRITE_SETTINGS_GAMMA)
    out.append("constexpr int kBlobEnhancedContrast = %d;" % DWRITE_SETTINGS_ENHANCED_CONTRAST)
    out.append("")
    out.append("// modules/libpref/init/StaticPrefList.yaml")
    out.append("// gfx.font_rendering.directwrite.bold_simulation: 0 never the DirectWrite")
    out.append("// bold simulation, 1 installed fonts only (not webfonts, not COLR), 2 all")
    out.append("// but COLR (gfxDWriteFontEntry::CreateFontInstance).")
    out.append("constexpr int kDirectWriteBoldSimulation = %s;" % bold_sim)
    out.append("")
    out.append("// gfx/thebes/gfxWindowsPlatform.cpp gfxWindowsPlatform::GetCommonFallbackFonts:")
    out.append("// the families Windows tries for a script after the font group's own and the")
    out.append("// pref fonts, before DirectWrite's system fallback. Script codes are the")
    out.append("// values of mozilla::intl::Script in intl/components/src/UnicodeScriptCodes.h.")
    out.append("// The conditions each AppendElement sits under are carried rather than")
    out.append("// flattened, so the replacement can evaluate them the way the source does.")
    out.append("enum CommonFallbackCondition")
    out.append("{")
    out.append("    kFallbackAlways = 0,")
    out.append("    kFallbackColor = 1,          // PrefersColor(aPresentation)")
    out.append("    kFallbackNotColor = 2,       // !PrefersColor(aPresentation)")
    out.append("    kFallbackSupplementary = 3,  // aCh > 0xFFFF")
    out.append("    kFallbackPunctOrCommon = 4,  // COMMON, or the symbol/punctuation test")
    out.append("};")
    out.append("struct CommonFallbackFamily")
    out.append("{")
    out.append("    const char* name;")
    out.append("    int condition;")
    out.append("};")
    out.append("struct CommonFallbackRule")
    out.append("{")
    out.append("    unsigned script_first, script_count, family_first, family_count;")
    out.append("};")

    families = []
    def add(names):
        first = len(families)
        families.extend(names)
        return first, len(names)

    head_first, head_count = add(before)
    script_pool = []
    rules = []
    for scripts, fams in entries:
        sf = len(script_pool)
        for name in scripts:
            if name not in codes:
                die("GetCommonFallbackFonts: Script::%s is not in UnicodeScriptCodes.h" % name)
            script_pool.append((name, codes[name]))
        ff, fc = add(fams)
        rules.append((sf, len(scripts), ff, fc))
    tail_first, tail_count = add(tail)

    out.append("// Script::COMMON, which the tail's condition tests for by name.")
    out.append("[[maybe_unused]] constexpr short kScriptCommon = %d;" % codes["COMMON"])
    out.append("[[maybe_unused]] const short kWindowsCommonFallbackScripts[] = {")
    for i in range(0, len(script_pool), 4):
        row = script_pool[i:i + 4]
        out.append("    " + " ".join("%d," % v for _n, v in row) +
                   "   // " + ", ".join(n for n, _v in row))
    out.append("};")
    out.append("[[maybe_unused]] const CommonFallbackFamily kWindowsCommonFallbackFamilies[] = {")
    for name, cond in families:
        out.append("    { .name = %s, .condition = %d }," % (c_string(name), cond))
    out.append("};")
    out.append("[[maybe_unused]] const CommonFallbackRule kWindowsCommonFallbackRules[] = {")
    for sf, sc, ff, fc in rules:
        out.append("    { .script_first = %d, .script_count = %d,"
                   " .family_first = %d, .family_count = %d }," % (sf, sc, ff, fc))
    out.append("};")
    out.append("[[maybe_unused]] constexpr unsigned kWindowsCommonFallbackRuleCount = %d;" % len(rules))
    out.append("[[maybe_unused]] constexpr unsigned kWindowsCommonFallbackHeadFirst = %d;" % head_first)
    out.append("[[maybe_unused]] constexpr unsigned kWindowsCommonFallbackHeadCount = %d;" % head_count)
    out.append("[[maybe_unused]] constexpr unsigned kWindowsCommonFallbackTailFirst = %d;" % tail_first)
    out.append("[[maybe_unused]] constexpr unsigned kWindowsCommonFallbackTailCount = %d;" % tail_count)
    out.append("")
    out.append("// modules/libpref/init/all.js, as MOZ_DEFAULT_PREFS data. Firefox parses")
    out.append("// the contents of that variable as default prefs, so cleartype/src/prefs.cpp")
    out.append("// hands it this and nothing has to be installed into a profile.")
    out.append("[[maybe_unused]] const auto* const kWindowsPrefs =")
    out.append(prefs_as_c_string(prefs_text))
    out.append("    ;")
    out.append("")
    out.append("}  // namespace firefox_parity")
    out.append("")
    out.append("#endif  // FIREFOX_PARITY_DATA_H_INCLUDED")
    return "\n".join(out) + "\n"


def gen_prefs(rev, win_prefs, font_ids, theme_accent):
    # noinspection PyListCreation
    out = []
    out.append("// The Firefox prefs half of Windows parity, embedded in")
    out.append("// firefox_parity_data.h and handed over through MOZ_DEFAULT_PREFS.")
    out.append("// GENERATED by tools/gen_firefox_parity.py from the Firefox source tree at")
    out.append("// %s. Do not edit by hand." % rev)
    out.append("//")
    out.append("// Only the pref lines are kept; see prefs_as_c_string.")
    out.append("")
    out.append("// --- Glyph preblend -------------------------------------------------------")
    out.append("//")
    out.append("// gfx/wr/wr_glyph_rasterizer/src/platform/windows/font.rs rasterize_glyph:")
    out.append("//   GammaLut::new(contrast / 100.0, gamma / 100.0, gamma / 100.0)")
    out.append("//   preblend_scaled(pixels, color, cleartype_level) / preblend(pixels, color)")
    out.append("// gfx/wr/wr_glyph_rasterizer/src/platform/unix/font.rs gamma_correct_pixels:")
    out.append("//   if gamma < 0 && enhanced_contrast <= 0 { return; }")
    out.append("//   GammaLut::new(enhanced_contrast / 100.0, gamma / 100.0, gamma / 100.0)")
    out.append("//   preblend_grayscale(pixels, color) / preblend(pixels, color)")
    out.append("// with gfx/wr/webrender_api/src/font.rs FontInstancePlatformOptions defaulting")
    out.append("// to gamma -1 / enhanced_contrast 0 on FreeType (StaticPrefList.yaml")
    out.append("// gfx.font_rendering.freetype.gamma = -1, .enhanced_contrast = 0), and on")
    out.append("// Windows set by gfx/2d/ScaledFontDWrite.cpp GetWRFontInstanceOptions:")
    out.append("//   gamma    = round(DWriteSettings().Gamma() * 100)")
    out.append("//   contrast = round(min(DWriteSettings().EnhancedContrast(), 1) * 100)")
    out.append("//   cleartype_level = round(min(ClearTypeLevel(), 1) * 100)")
    out.append("// DWriteSettings (gfx/2d/DWriteSettings.cpp) answers with its own")
    out.append("// initialisers, sGamma{2.2f} and sEnhancedContrast{1.0f}, until the gfxVars")
    out.append("// below replace them. The blob rasterizer that draws SVG text is reached")
    out.append("// before that and sees 220 and 100; everything else is reached after and")
    out.append("// sees the gfxVars")
    out.append("// gfx/thebes/gfxDWriteFonts.cpp gfxDWriteFont::UpdateClearTypeVars sets:")
    out.append("// gamma = IDWriteFactory::CreateRenderingParams()->GetGamma(), which is")
    out.append("// DirectWrite's default of 1.8 on a machine without ClearType Tuner registry")
    out.append("// overrides (the shim logs the same value from DWriteCore's factory);")
    out.append("// enhanced contrast = 1.0, the hard-coded default, because the DISPLAY1")
    out.append("// EnhancedContrastLevel registry value is only present after tuning; ClearType")
    out.append("// level = 1.0; and the gfx.font_rendering.cleartype_params.* prefs are all")
    out.append("// -1 in modules/libpref/init/all.js. With equal inputs the two preblend")
    out.append("// paths are the same arithmetic: preblend_grayscale equals preblend for the")
    out.append("// luminance color prepare_font() uses in Alpha mode, and preblend_scaled at")
    out.append("// level 100 is preblend.")
    out.append('user_pref("gfx.font_rendering.freetype.gamma", %d);' % FREETYPE_GAMMA)
    out.append('user_pref("gfx.font_rendering.freetype.enhanced_contrast", %d);' % FREETYPE_ENHANCED_CONTRAST)
    out.append("")
    out.append("// --- Widget theme ---------------------------------------------------------")
    out.append("//")
    out.append("// The one pref here that is not about text. Form controls are drawn by")
    out.append("// Gecko's own theme on both platforms, and its accent color comes from the")
    out.append("// desktop on every platform except Windows, where")
    out.append("// modules/libpref/init/StaticPrefList.yaml turns that off and the built-in")
    out.append("// blue is used instead. widget/ThemeColors.cpp GetAccentColor and")
    out.append("// widget/nsXPLookAndFeel.cpp ShouldUseStandinsForNativeColorForNonNativeTheme")
    out.append("// both read it. Without it a checkbox or a radio button carries the")
    out.append("// desktop's accent here and Firefox's blue there, the same shape in two")
    out.append("// hues.")
    out.append('user_pref("widget.non-native-theme.use-theme-accent", %s);' % theme_accent)
    out.append("")
    out.append("// --- Font prefs -----------------------------------------------------------")
    out.append("//")
    out.append("// modules/libpref/init/all.js: every font pref a Windows build would")
    out.append("// have and this one would not, whether Windows sets it in an XP_WIN block")
    out.append("// or leaves it at the file's default for the Linux block to override.")
    out.append("// gfx/thebes/gfxFcPlatformFontList.cpp PrefFontListsUseOnlyGenerics() sees")
    out.append("// non-generic values here and so FindAndAddFamiliesLocked resolves generics")
    out.append("// through these lists rather than through fontconfig")
    out.append("// (mAlwaysUseFontconfigGenerics false), which is how")
    out.append("// gfx/thebes/gfxPlatformFontList.cpp ResolveGenericFontNames resolves them")
    out.append("// on Windows. Only the families installed on both machines can match; a name")
    out.append("// the Windows list has and this machine lacks falls through to the next, as")
    out.append("// it would on Windows.")
    for p in win_prefs:
        out.append("user_pref(%s);" % p)
    out.append("")
    out.append("// --- System fonts ---------------------------------------------------------")
    out.append("//")
    out.append("// widget/nsXPLookAndFeel.cpp GetFontValue reads ui.font.<id>, .size, .weight")
    out.append("// and .italic before asking the platform, for the FontIDs sFontPrefs names.")
    out.append("// The values are what widget/windows/nsLookAndFeel.cpp GetLookAndFeelFont")
    out.append("// returns on a Windows 11 install at 100% scale with the default")
    out.append("// NONCLIENTMETRICS (every lf*Font and the icon title font are Segoe UI,")
    out.append("// lfHeight -12, lfWeight FW_NORMAL): GetLookAndFeelFontInternal makes size =")
    out.append("// -lfHeight * pixelScale with pixelScale = 1 / SystemScaleFactor() /")
    out.append("// GetTextScaleFactor(), weight NORMAL unless FW_BOLD, and for MozButton,")
    out.append("// MozField and MozList the name \"MS Shell Dlg 2\" with lfMessageFont's size.")
    out.append("// layout/style/res/forms.css uses -moz-field (input), -moz-list (select) and")
    out.append("// -moz-button (button); gfx/thebes/gfxPlatformFontList.cpp")
    out.append("// GetSystemUIFontFamilies resolves system-ui through the menu font.")
    for fid in font_ids:
        name = SHELL_DLG_NAME if fid in SHELL_DLG_IDS else SYSTEM_FONT_NAME
        out.append('user_pref("ui.font.%s", "%s");' % (fid, name))
        out.append('user_pref("ui.font.%s.size", "%s");' % (fid, SYSTEM_FONT_SIZE))
        out.append('user_pref("ui.font.%s.weight", "%s");' % (fid, SYSTEM_FONT_WEIGHT))
        out.append('user_pref("ui.font.%s.italic", false);' % fid)
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

# The stamp gen_header writes at the top of the generated file.
STAMP_RE = re.compile(r"Firefox source tree at (.+?)\. Do not edit by hand\.")
# "TAG (commit)", as tree_revision reports it, or a bare commit.
REVISION_RE = re.compile(r"^(?:(\S+) )?\(?([0-9a-f]{7,40})\)?$")


def pinned_revision():
    """(tag, commit) the generated header says it came from.

    This is the pin. It lives in the generated file, not in a constant here,
    so that regenerating against a newer Firefox moves the data and the
    revision it claims together, and neither can be bumped without the other.
    Returns ("", "") when nothing has been generated yet.
    """
    try:
        with open(DATA_HEADER, encoding="utf-8") as f:
            stamp = STAMP_RE.search(f.read(4096))
    except OSError:
        return "", ""
    if stamp is None:
        return "", ""
    found = REVISION_RE.match(stamp.group(1).strip())
    if found is None:
        # A tree with no commit to name - a tarball, say. Not fetchable, but
        # it is still what the file claims.
        return stamp.group(1).strip(), ""
    return found.group(1) or "", found.group(2)


def resolve(ref):
    """A ref to the commit it names, asking the server only when it must."""
    tag, rev = pinned_revision()
    if not ref:
        if rev:
            return tag, rev
        # Nothing generated yet, so there is nothing to reproduce: start from
        # the revision the shim was translated from.
        ref = tag or DEFAULT_TAG
    # The pinned pair was recorded together by whatever produced the header,
    # so answering from it cannot put a tag against the wrong commit - and it
    # is the reproducible answer even if the tag has since been moved.
    if rev and ref in (tag, rev):
        return tag, rev
    url = "%s/commits/%s" % (GITHUB_API, ref)
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            sha = json.load(response)["sha"]
    except (urllib.error.URLError, ValueError, KeyError) as exc:
        die("cannot resolve %s: %s" % (ref, exc))
    # A ref that is already a commit names itself, and has no tag to report.
    return ("" if sha.startswith(ref) else ref), sha


def download(rev, rel):
    url = "%s/%s/%s" % (GITHUB_RAW, rev, rel)
    try:
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
            declared = response.headers.get("Content-Length")
    except urllib.error.HTTPError as exc:
        die("%s: HTTP %d - the file is not in this revision, or has moved"
            % (rel, exc.code))
    except urllib.error.URLError as exc:
        die("%s: %s" % (rel, exc.reason))
    # A short read arrives as a truncated body, not as an error.
    if declared is not None and len(data) != int(declared):
        die("%s: got %d bytes, the server said %s" % (rel, len(data), declared))
    return data


def fetch(ref, dest):
    """Put exactly SOURCE_FILES into `dest`, in the tree's own layout.

    Files already present with the digest the marker recorded are left alone,
    so a re-run costs nothing and an interrupted one is repaired instead of
    trusted.
    """
    tag, rev = resolve(ref)
    marker_path = os.path.join(dest, FETCH_MARKER)
    have = {}
    known = marker_revision(dest)
    if known == (("%s (%s)" % (tag, rev)) if tag else rev):
        try:
            with open(marker_path, encoding="utf-8") as f:
                have = json.load(f).get("files", {})
        except (OSError, ValueError):
            have = {}

    digests = {}
    fetched = downloaded = 0
    for rel in SOURCE_FILES:
        path = os.path.join(dest, rel)
        want = have.get(rel)
        if want is not None and os.path.isfile(path):
            with open(path, "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
            if got == want:
                digests[rel] = got
                fetched += 1
                continue
        data = download(rev, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        digests[rel] = hashlib.sha256(data).hexdigest()
        fetched += 1
        downloaded += 1

    with open(marker_path, "w", encoding="utf-8") as f:
        json.dump({"tag": tag, "rev": rev, "files": digests}, f, indent=1, sort_keys=True)
        f.write("\n")
    print("fetch: %d files at %s, %d downloaded, %d already present"
          % (fetched, tag or rev, downloaded, fetched - downloaded))


def check_sources():
    """Every anchored file has to be one of the files --fetch brings down."""
    named = {rel for rel, _ in ANCHORS}
    missing = sorted(named - set(SOURCE_FILES))
    if missing:
        die("SOURCE_FILES is missing anchored file(s): " + ", ".join(missing))


def check_anchors(tree):
    missing = 0
    for rel, fragments in ANCHORS:
        try:
            text = read(tree, rel)
        except OSError:
            print("MISSING FILE  %s" % rel)
            missing += len(fragments)
            continue
        for frag in fragments:
            if frag not in text:
                print("MISSING       %s: %s" % (rel, frag))
                missing += 1
    total = sum(len(f) for _, f in ANCHORS)
    print("anchors: %d of %d present" % (total - missing, total))
    return missing == 0


def compare_or_write(path, content, write):
    rel = os.path.relpath(path, ROOT)
    try:
        with open(path, encoding="utf-8") as f:
            current = f.read()
    except OSError:
        current = None
    if current == content:
        print("up to date    %s" % rel)
        return True
    if write:
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        print("written       %s" % rel)
        return True
    print("DRIFT         %s (run with --write to regenerate)" % rel)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tree", help="Firefox source tree (sparse checkout is enough)")
    ap.add_argument("--fetch", metavar="REF", nargs="?", const="",
                    help="download the %d files this tool reads, at REF - a tag, "
                         "branch or commit - into --tree. With no REF, the "
                         "revision the generated header records, which is the "
                         "one that reproduces it" % len(SOURCE_FILES))
    ap.add_argument("--write", action="store_true", help="regenerate the outputs")
    ap.add_argument("--check", action="store_true", help="only check (the default)")
    args = ap.parse_args()

    tree = args.tree or os.environ.get("FIREFOX_TREE")
    if not tree:
        die("--tree PATH (or FIREFOX_TREE) is required")
    check_sources()
    if args.fetch is not None:
        fetch(args.fetch, tree)
    absent = [rel for rel in SOURCE_FILES
              if not os.path.isfile(os.path.join(tree, rel))]
    if absent:
        die("%s is missing %d of the %d files this reads, starting with %s"
            "%s" % (tree, len(absent), len(SOURCE_FILES), absent[0],
                    "" if args.fetch is not None
                    else " - run with --fetch to download them"))

    rev = tree_revision(tree)
    print("tree: %s at %s" % (tree, rev))

    ok = check_anchors(tree)

    all_js = read(tree, "modules/libpref/init/all.js")
    yaml = read(tree, "modules/libpref/init/StaticPrefList.yaml")
    prefs = gen_prefs(rev, font_prefs_by_platform(all_js),
                      font_pref_ids(read(tree, "widget/nsXPLookAndFeel.cpp")),
                      windows_static_pref(
                          yaml, "widget.non-native-theme.use-theme-accent"))
    header = gen_header(
        rev,
        bad_underline_families(all_js),
        WEBRENDER_THREAD_NAMES,
        static_pref_value(yaml, "gfx.font_rendering.directwrite.bold_simulation"),
        windows_common_fallback(read(tree, "gfx/thebes/gfxWindowsPlatform.cpp")),
        script_codes(read(tree, "intl/components/src/UnicodeScriptCodes.h")),
        prefs)

    # Generated files carry the revision they came from, so a check against
    # the same tree is byte-exact and a different tree shows as drift.
    ok = compare_or_write(DATA_HEADER, header, args.write) and ok

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
