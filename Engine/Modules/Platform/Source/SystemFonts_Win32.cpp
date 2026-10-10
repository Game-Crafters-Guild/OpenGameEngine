#include "Platform/SystemFonts.h"
#include "SystemFontGenerics.h"

#include "Logger/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <dwrite.h>
#include <wrl/client.h>

#include <vector>

namespace GameEngine::Platform
{
namespace
{
using Microsoft::WRL::ComPtr;

static std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty())
        return {};

    const int lenW = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (lenW <= 0)
        return {};

    std::wstring out;
    out.resize((size_t)lenW);
    const int written = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), lenW);
    if (written != lenW)
        return {};
    return out;
}

} // namespace

static DWRITE_FONT_WEIGHT DWriteWeightFromCss(int weight)
{
    if (weight >= 950) return DWRITE_FONT_WEIGHT_ULTRA_BLACK;
    if (weight >= 900) return DWRITE_FONT_WEIGHT_BLACK;
    if (weight >= 800) return DWRITE_FONT_WEIGHT_EXTRA_BOLD;
    if (weight >= 700) return DWRITE_FONT_WEIGHT_BOLD;
    if (weight >= 600) return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    if (weight >= 500) return DWRITE_FONT_WEIGHT_MEDIUM;
    if (weight >= 400) return DWRITE_FONT_WEIGHT_NORMAL;
    if (weight >= 300) return DWRITE_FONT_WEIGHT_LIGHT;
    if (weight >= 200) return DWRITE_FONT_WEIGHT_EXTRA_LIGHT;
    return DWRITE_FONT_WEIGHT_THIN;
}

static DWRITE_FONT_STYLE DWriteStyleFromSystem(SystemFontStyle style)
{
    switch (style)
    {
    case SystemFontStyle::Italic:  return DWRITE_FONT_STYLE_ITALIC;
    case SystemFontStyle::Oblique: return DWRITE_FONT_STYLE_OBLIQUE;
    default:                       return DWRITE_FONT_STYLE_NORMAL;
    }
}

constexpr GenericFontFamily kGenericFamilies[] = {
    {"sans-serif", "Segoe UI"},
    {"sansserif", "Segoe UI"},
    {"system-ui", "Segoe UI"},
    {"serif", "Times New Roman"},
    {"monospace", "Consolas"},
    {"ui-monospace", "Consolas"},
};

bool TryResolveSystemFontFile(std::string_view family, int weight, SystemFontStyle style, SystemFontFile& out)
{
    out = {};
    family = MapGenericFontFamily(family, kGenericFamilies);

    const std::wstring wFamily = Utf8ToWide(family);
    if (wFamily.empty())
        return false;

    // Best-effort COM init. DirectWrite often works without explicit init, but this
    // makes the call more robust when invoked early in a process lifetime.
    bool didCoInit = false;
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hrCo == S_OK || hrCo == S_FALSE)
        didCoInit = true;

    ComPtr<IDWriteFactory> factory;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteFontCollection> collection;
    hr = factory->GetSystemFontCollection(&collection);
    if (FAILED(hr) || !collection)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    UINT32 familyIndex = 0;
    BOOL exists = FALSE;
    hr = collection->FindFamilyName(wFamily.c_str(), &familyIndex, &exists);
    if (FAILED(hr) || !exists)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteFontFamily> fontFamily;
    hr = collection->GetFontFamily(familyIndex, &fontFamily);
    if (FAILED(hr) || !fontFamily)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteFont> font;
    hr = fontFamily->GetFirstMatchingFont(DWriteWeightFromCss(weight),
                                          DWRITE_FONT_STRETCH_NORMAL,
                                          DWriteStyleFromSystem(style),
                                          &font);
    if (FAILED(hr) || !font)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteFontFace> face;
    hr = font->CreateFontFace(&face);
    if (FAILED(hr) || !face)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    UINT32 fileCount = 0;
    hr = face->GetFiles(&fileCount, nullptr);
    if (FAILED(hr) || fileCount == 0)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    std::vector<IDWriteFontFile*> rawFiles;
    rawFiles.resize(fileCount);
    hr = face->GetFiles(&fileCount, rawFiles.data());
    if (FAILED(hr) || fileCount == 0 || !rawFiles[0])
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    // Adopt the first file (typical for most installed fonts).
    ComPtr<IDWriteFontFile> fontFile;
    fontFile.Attach(rawFiles[0]);
    // Release remaining raw pointers if any.
    for (UINT32 i = 1; i < fileCount; ++i)
    {
        if (rawFiles[i])
            rawFiles[i]->Release();
    }

    const void* refKey = nullptr;
    UINT32 refKeySize = 0;
    hr = fontFile->GetReferenceKey(&refKey, &refKeySize);
    if (FAILED(hr) || !refKey || refKeySize == 0)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteFontFileLoader> loader;
    hr = fontFile->GetLoader(&loader);
    if (FAILED(hr) || !loader)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    ComPtr<IDWriteLocalFontFileLoader> localLoader;
    hr = loader.As(&localLoader);
    if (FAILED(hr) || !localLoader)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    UINT32 pathLen = 0;
    hr = localLoader->GetFilePathLengthFromKey(refKey, refKeySize, &pathLen);
    if (FAILED(hr) || pathLen == 0)
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    std::wstring filePath;
    filePath.resize((size_t)pathLen + 1u); // include null
    hr = localLoader->GetFilePathFromKey(refKey, refKeySize, filePath.data(), pathLen + 1u);
    if (FAILED(hr))
    {
        if (didCoInit)
            CoUninitialize();
        return false;
    }

    // Remove trailing null that DirectWrite writes.
    if (!filePath.empty() && filePath.back() == L'\0')
        filePath.pop_back();

    out.path = std::filesystem::path(filePath);
    out.faceIndex = face->GetIndex();
    out.resolvedFamily = std::string(family);

    if (didCoInit)
        CoUninitialize();

    return !out.path.empty();
}

} // namespace GameEngine::Platform

