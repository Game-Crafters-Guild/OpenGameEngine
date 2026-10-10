#include "Platform/SystemFonts.h"
#include "SystemFontGenerics.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

namespace GameEngine::Platform
{
namespace
{
static CFStringRef MakeCFString(std::string_view s)
{
    if (s.empty())
        return nullptr;
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(s.data()),
                                   (CFIndex)s.size(),
                                   kCFStringEncodingUTF8,
                                   false);
}

static std::string CFStringToUtf8(CFStringRef str)
{
    if (!str)
        return {};

    const CFIndex len = CFStringGetLength(str);
    if (len <= 0)
        return {};

    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8);
    if (maxBytes <= 0)
        return {};

    std::string out;
    out.resize((size_t)maxBytes);
    CFIndex used = 0;
    const Boolean ok = CFStringGetBytes(str,
                                       CFRangeMake(0, len),
                                       kCFStringEncodingUTF8,
                                       '?',
                                       false,
                                       reinterpret_cast<UInt8*>(out.data()),
                                       maxBytes,
                                       &used);
    if (!ok || used <= 0)
        return {};
    out.resize((size_t)used);
    return out;
}

} // namespace

constexpr GenericFontFamily kGenericFamilies[] = {
    {"sans-serif", "Helvetica Neue"},
    {"sansserif", "Helvetica Neue"},
    {"system-ui", "Helvetica Neue"},
    {"serif", "Times New Roman"},
    {"monospace", "Menlo"},
    {"ui-monospace", "Menlo"},
};

bool TryResolveSystemFontFile(std::string_view family, int weight, SystemFontStyle style, SystemFontFile& out)
{
    out = {};
    family = MapGenericFontFamily(family, kGenericFamilies);

    CFStringRef familyName = MakeCFString(family);
    if (!familyName)
        return false;

    // Create a descriptor by name. Note: this may map to a specific face within
    // a collection. We attempt to extract both URL and face index when available.
    // CoreText descriptors can include traits; apply requested weight/style best-effort.
    CTFontDescriptorRef base = CTFontDescriptorCreateWithNameAndSize(familyName, 12.0);
    CFRelease(familyName);
    if (!base)
        return false;

    // Build a traits dictionary: weight is a float in [-1, 1] (approx); slant is also float.
    // We keep this best-effort: not all fonts support all traits.
    double w = 0.0;
    if (weight <= 150) w = -0.8;
    else if (weight <= 250) w = -0.6;
    else if (weight <= 350) w = -0.3;
    else if (weight <= 450) w = 0.0;
    else if (weight <= 550) w = 0.2;
    else if (weight <= 650) w = 0.4;
    else if (weight <= 750) w = 0.6;
    else w = 0.8;

    double slant = 0.0;
    if (style == SystemFontStyle::Italic) slant = 1.0;
    else if (style == SystemFontStyle::Oblique) slant = 0.5;

    CFNumberRef weightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &w);
    CFNumberRef slantNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &slant);

    const void* traitKeys[] = { kCTFontWeightTrait, kCTFontSlantTrait };
    const void* traitVals[] = { weightNum, slantNum };
    CFDictionaryRef traits = CFDictionaryCreate(kCFAllocatorDefault, traitKeys, traitVals, 2,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    const void* attrKeys[] = { kCTFontTraitsAttribute };
    const void* attrVals[] = { traits };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, attrKeys, attrVals, 1,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CTFontDescriptorRef desc = CTFontDescriptorCreateCopyWithAttributes(base, attrs);

    if (attrs) CFRelease(attrs);
    if (traits) CFRelease(traits);
    if (weightNum) CFRelease(weightNum);
    if (slantNum) CFRelease(slantNum);
    CFRelease(base);

    if (!desc)
        return false;

    CTFontRef font = CTFontCreateWithFontDescriptor(desc, 12.0, nullptr);
    if (!font)
    {
        CFRelease(desc);
        return false;
    }

    CTFontDescriptorRef resolvedDesc = CTFontCopyFontDescriptor(font);

    // Extract file URL.
    CFURLRef url = nullptr;
    if (resolvedDesc)
    {
        url = (CFURLRef)CTFontDescriptorCopyAttribute(resolvedDesc, kCTFontURLAttribute);
    }

    if (url)
    {
        // Convert URL to a filesystem path.
        CFStringRef pathStr = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        if (pathStr)
        {
            out.path = std::filesystem::path(CFStringToUtf8(pathStr));
            CFRelease(pathStr);
        }
        CFRelease(url);
    }

    // NOTE: Some CoreText SDKs do not expose kCTFontIndexAttribute, and our current FontAtlas
    // implementation does not consume faceIndex anyway. Keep faceIndex at 0 for now.
    // If/when we add TTC face selection to FontAtlas, we can revisit this using a more portable API.

    // Best-effort resolved family name.
    {
        CFStringRef fam = CTFontCopyFamilyName(font);
        if (fam)
        {
            out.resolvedFamily = CFStringToUtf8(fam);
            CFRelease(fam);
        }
    }

    if (resolvedDesc)
        CFRelease(resolvedDesc);
    CFRelease(font);
    CFRelease(desc);

    return !out.path.empty();
}

} // namespace GameEngine::Platform

#else

namespace GameEngine::Platform
{
bool TryResolveSystemFontFile(std::string_view /*family*/, int /*weight*/, SystemFontStyle /*style*/, SystemFontFile& /*out*/) { return false; }
} // namespace GameEngine::Platform

#endif

