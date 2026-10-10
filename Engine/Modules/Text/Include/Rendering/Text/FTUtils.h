#pragma once

#include <string>
#include <vector>

#if defined(GE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#if defined(GE_HAVE_HARFBUZZ)
#include <hb.h>
#include <hb-ft.h>
#endif

namespace GameEngine { namespace Rendering { namespace Text {

#if defined(GE_HAVE_FREETYPE)
class FreeTypeLib {
public:
    FreeTypeLib() = default;
    ~FreeTypeLib();
    bool Init();
    FT_Library Lib() const { return m_Lib; }
private:
    FT_Library m_Lib = nullptr;
};

class FreeTypeFace {
public:
    FreeTypeFace() = default;
    ~FreeTypeFace();

    bool NewFace(FT_Library lib, const std::string& path);
    bool NewMemoryFace(FT_Library lib, const unsigned char* data, size_t size);
    // Like NewMemoryFace but does not copy: the caller guarantees `data`
    // outlives this face. Used for measure-face clones that alias the
    // primary face's owned buffer.
    bool NewMemoryFaceUnowned(FT_Library lib, const unsigned char* data, size_t size);
    bool SetPixelSizes(unsigned height);

    FT_Face Face() const { return m_Face; }
    bool IsValid() const { return m_Face != nullptr; }

    // Owned font bytes (empty for faces created via NewFace/NewMemoryFaceUnowned).
    const unsigned char* Data() const { return m_Buffer.data(); }
    size_t Size() const { return m_Buffer.size(); }

private:
    FT_Face m_Face = nullptr;
    // Keep memory buffer alive for FT_New_Memory_Face
    std::vector<unsigned char> m_Buffer;
};
#endif

#if defined(GE_HAVE_HARFBUZZ)
class HBFont {
public:
    HBFont() = default;
    ~HBFont();
    bool CreateFromFT(FT_Face face);
    void SetLoadFlags(int flags);
    hb_font_t* Get() const { return m_Font; }
private:
    hb_font_t* m_Font = nullptr;
};
#endif

}}} // namespaces

