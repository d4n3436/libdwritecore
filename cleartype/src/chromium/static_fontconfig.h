//+--------------------------------------------------------------------------
//
//  static_fontconfig.h - asking for one family ahead of the rest.
//
//  A build with fontconfig compiled in is served a configuration instead of
//  the host's, and the order it states is settled once per locale. Windows
//  answers per character, so the configuration also carries one block per
//  family, reached by a tag appended to the locale. fallback_hook.cpp names
//  the family that way, a character at a time.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_STATIC_FONTCONFIG_H_INCLUDED
#define CHROMIUM_STATIC_FONTCONFIG_H_INCLUDED

namespace static_fontconfig {

// What a tagged locale reads like: this, then the family's two-digit index.
// Long enough that no real language tag holds it.
constexpr char kFamilyTag[] = "dwcfam";
// The same, for the pan-Unicode list on its own.
constexpr char kPanTag[] = "dwcpan";
// The same, for a measured IDWriteFontFallback row's two-digit index. That
// family is walked after the pan-Unicode list, which is where Windows reaches
// it, so the block states the list first and the family last.
constexpr char kDWriteTag[] = "dwcdw";
// The same for a row whose front family follows the run's language, then the
// row's two-digit index and the language's. A separate prefix so a tag of this
// kind never reads as one of the plain ones.
constexpr char kDWriteHanTag[] = "dwcdwh";
// Appended to any of the above when the run asking is bold. The lang test is
// a substring test, so it matches alongside whichever tag it was appended to
// and states the weight the sort should pick a face at. Windows resolves the
// fallback family at the run's real weight and gets the family's Bold face;
// Linux drops the weight before it asks, so without this the sort answers the
// regular face and Blink strokes it instead.
constexpr char kBoldTag[] = "dwcbold";
constexpr unsigned kMaxTaggedFamilies = 100;

// Which of the Han languages the configuration names this locale as, or -1
// when none of them, which is an unsettled USCRIPT_HAN.
int HanLocaleIndex(const char* locale);

// The tag index for a family, or -1 when the configuration names none, which
// is also the answer on a build that is not served one.
int TagFor(const char* family);

}  // namespace static_fontconfig

#endif  // CHROMIUM_STATIC_FONTCONFIG_H_INCLUDED
