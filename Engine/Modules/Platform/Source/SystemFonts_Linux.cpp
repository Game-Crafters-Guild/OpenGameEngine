#include "Platform/SystemFonts.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#if defined(GE_HAVE_FONTCONFIG) && GE_HAVE_FONTCONFIG
#include "SystemFontGenerics.h"

#include <fontconfig/fontconfig.h>
#endif

#include <cstring>

namespace GameEngine::Platform
{

#if defined(GE_HAVE_FONTCONFIG) && GE_HAVE_FONTCONFIG
static int FcWeightFromCss(int weight)
{
    // Map CSS weight (100..900) to Fontconfig weights (approx).
    if (weight >= 850) return FC_WEIGHT_BLACK;
    if (weight >= 750) return FC_WEIGHT_BOLD;
    if (weight >= 650) return FC_WEIGHT_DEMIBOLD;
    if (weight >= 550) return FC_WEIGHT_MEDIUM;
    if (weight >= 450) return FC_WEIGHT_REGULAR;
    if (weight >= 350) return FC_WEIGHT_LIGHT;
    return FC_WEIGHT_THIN;
}
#endif

#if defined(GE_HAVE_FONTCONFIG) && GE_HAVE_FONTCONFIG
constexpr GenericFontFamily kGenericFamilies[] = {
    {"sans-serif", "DejaVu Sans"},
    {"sansserif", "DejaVu Sans"},
    {"system-ui", "DejaVu Sans"},
    {"serif", "DejaVu Serif"},
    {"monospace", "DejaVu Sans Mono"},
    {"ui-monospace", "DejaVu Sans Mono"},
};

static int FcSlantFromStyle(SystemFontStyle style)
{
    switch (style)
    {
    case SystemFontStyle::Italic:  return FC_SLANT_ITALIC;
    case SystemFontStyle::Oblique: return FC_SLANT_OBLIQUE;
    default:                       return FC_SLANT_ROMAN;
    }
}
#endif

bool TryResolveSystemFontFile(std::string_view family, int weight, SystemFontStyle style, SystemFontFile& out)
{
    out = {};

#if !defined(GE_HAVE_FONTCONFIG) || !GE_HAVE_FONTCONFIG
    (void)family;
    (void)weight;
    (void)style;
    return false;
#else
    family = MapGenericFontFamily(family, kGenericFamilies);
    if (family.empty())
        return false;

    if (!FcInit())
        return false;

    FcPattern* pat = FcPatternCreate();
    if (!pat)
        return false;

    // Add family name (UTF-8).
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)std::string(family).c_str());
    // Weight/style requests.
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromCss(weight));
    FcPatternAddInteger(pat, FC_SLANT, FcSlantFromStyle(style));

    // Apply config/default substitutions.
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);

    if (!match || result != FcResultMatch)
    {
        if (match)
            FcPatternDestroy(match);
        return false;
    }

    FcChar8* file = nullptr;
    int index = 0;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file)
    {
        FcPatternDestroy(match);
        return false;
    }
    (void)FcPatternGetInteger(match, FC_INDEX, 0, &index);

    out.path = std::filesystem::path((const char*)file);
    out.faceIndex = (index >= 0) ? (uint32_t)index : 0u;
    out.resolvedFamily = std::string(family);

    FcPatternDestroy(match);

    return !out.path.empty();
#endif
}

} // namespace GameEngine::Platform

#endif
