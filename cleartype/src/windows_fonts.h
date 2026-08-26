//+--------------------------------------------------------------------------
//
//  windows_fonts.h - the families a stock Windows 11 ships.
//
//  Transcribed from Microsoft's Windows 11 font list, first table:
//  https://learn.microsoft.com/en-us/typography/fonts/windows_11_font_list
//
//  The base image only. The same page's second half lists the families kept
//  in Feature-on-Demand packages - Meiryo, MS Mincho, Yu Mincho, Batang,
//  Gulim, MingLiU, SimHei and the rest - which are absent here, so that what
//  this reorders does not depend on which optional packages a machine's fonts
//  were extracted from. The Ext-B fonts invert that: MingLiU-ExtB is in the
//  base image, plain MingLiU is not.
//
//  Sub-family names that a caller asks for by name are listed in their own
//  right - MS PGothic and MS UI Gothic alongside MS Gothic, NSimSun and
//  SimSun-ExtB alongside SimSun - because a font.name-list entry names one of
//  those, not the family the documentation groups them under. Segoe UI
//  Variable and Sitka are the same case for a different reason: both ship as
//  one variable file, and DirectWrite presents them only as their optical-size
//  families, so the bare name the documentation uses matches nothing. The names come
//  from a Windows 11 install's own font registry, which carries names the
//  documentation page leaves out. Sans Serif Collection is one of those, and
//  DirectWrite falls back to it for a dozen scripts.
//
//----------------------------------------------------------------------------

#ifndef CLEARTYPE_WINDOWS_FONTS_H_INCLUDED
#define CLEARTYPE_WINDOWS_FONTS_H_INCLUDED

namespace windows_fonts
{

inline constexpr const char* kBaseInstall[] = {
    "Arial",
    "Arial Black",
    "Bahnschrift",
    "Calibri",
    "Cambria",
    "Cambria Math",
    "Candara",
    "Cascadia Code",
    "Cascadia Mono",
    "Comic Sans MS",
    "Consolas",
    "Constantia",
    "Corbel",
    "Courier New",
    "Ebrima",
    "Franklin Gothic Medium",
    "Gabriola",
    "Gadugi",
    "Georgia",
    "HoloLens MDL2 Assets",
    "Impact",
    "Ink Free",
    "Javanese Text",
    "Leelawadee UI",
    "Lucida Console",
    "Lucida Sans Unicode",
    "Malgun Gothic",
    "Marlett",
    "Microsoft Himalaya",
    "Microsoft JhengHei",
    "Microsoft JhengHei UI",
    "Microsoft New Tai Lue",
    "Microsoft PhagsPa",
    "Microsoft Sans Serif",
    "Microsoft Tai Le",
    "Microsoft YaHei",
    "Microsoft YaHei UI",
    "Microsoft Yi Baiti",
    "MingLiU-ExtB",
    "MingLiU_HKSCS-ExtB",
    "Mongolian Baiti",
    "MS Gothic",
    "MS PGothic",
    "MS UI Gothic",
    "MV Boli",
    "Myanmar Text",
    "Nirmala Text",
    "Nirmala UI",
    "NSimSun",
    "Palatino Linotype",
    "PMingLiU-ExtB",
    "Sans Serif Collection",
    "Segoe Fluent Icons",
    "Segoe MDL2 Assets",
    "Segoe Print",
    "Segoe Script",
    "Segoe UI",
    "Segoe UI Emoji",
    "Segoe UI Historic",
    "Segoe UI Symbol",
    "Segoe UI Variable",
    "Segoe UI Variable Display",
    "Segoe UI Variable Small",
    "Segoe UI Variable Text",
    "SimSun",
    "SimSun-ExtB",
    "SimSun-ExtG",
    "Sitka",
    "Sitka Banner",
    "Sitka Display",
    "Sitka Heading",
    "Sitka Small",
    "Sitka Subheading",
    "Sitka Text",
    "Sitka Text",
    "Sylfaen",
    "Symbol",
    "Tahoma",
    "Times New Roman",
    "Trebuchet MS",
    "Verdana",
    "Webdings",
    "Wingdings",
    "Yu Gothic",
    "Yu Gothic UI",
};

}  // namespace windows_fonts

#endif  // CLEARTYPE_WINDOWS_FONTS_H_INCLUDED
