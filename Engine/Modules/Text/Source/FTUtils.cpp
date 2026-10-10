#include "Rendering/Text/FTUtils.h"

namespace GameEngine { namespace Rendering { namespace Text {

#if defined(GE_HAVE_FREETYPE)
FreeTypeLib::~FreeTypeLib() {
    if (m_Lib) { FT_Done_FreeType(m_Lib); m_Lib = nullptr; }
}

bool FreeTypeLib::Init() {
    if (m_Lib) return true;
    FT_Error err = FT_Init_FreeType(&m_Lib);
    return err == 0 && m_Lib != nullptr;
}

FreeTypeFace::~FreeTypeFace() {
    if (m_Face) { FT_Done_Face(m_Face); m_Face = nullptr; }
}

bool FreeTypeFace::NewFace(FT_Library lib, const std::string& path) {
    if (!lib) return false;
    if (m_Face) { FT_Done_Face(m_Face); m_Face = nullptr; }
    FT_Error err = FT_New_Face(lib, path.c_str(), 0, &m_Face);
    return err == 0 && m_Face != nullptr;
}

bool FreeTypeFace::NewMemoryFace(FT_Library lib, const unsigned char* data, size_t size) {
    if (!lib || !data || size == 0) return false;
    if (m_Face) { FT_Done_Face(m_Face); m_Face = nullptr; }
    m_Buffer.assign(data, data + size);
    FT_Error err = FT_New_Memory_Face(lib, m_Buffer.data(), static_cast<FT_Long>(m_Buffer.size()), 0, &m_Face);
    if (err != 0) {
        m_Buffer.clear();
        return false;
    }
    return true;
}

bool FreeTypeFace::NewMemoryFaceUnowned(FT_Library lib, const unsigned char* data, size_t size) {
    if (!lib || !data || size == 0) return false;
    if (m_Face) { FT_Done_Face(m_Face); m_Face = nullptr; }
    m_Buffer.clear();
    FT_Error err = FT_New_Memory_Face(lib, data, static_cast<FT_Long>(size), 0, &m_Face);
    return err == 0 && m_Face != nullptr;
}

bool FreeTypeFace::SetPixelSizes(unsigned height) {
    if (!m_Face) return false;
    FT_Error err = FT_Set_Pixel_Sizes(m_Face, 0, static_cast<FT_UInt>(height));
    return err == 0;
}
#endif

#if defined(GE_HAVE_HARFBUZZ)
HBFont::~HBFont() {
    if (m_Font) { hb_font_destroy(m_Font); m_Font = nullptr; }
}

bool HBFont::CreateFromFT(FT_Face face) {
    if (!face) return false;
    if (m_Font) { hb_font_destroy(m_Font); m_Font = nullptr; }
    m_Font = hb_ft_font_create_referenced(face);
    return m_Font != nullptr;
}

void HBFont::SetLoadFlags(int flags) {
    if (m_Font) hb_ft_font_set_load_flags(m_Font, flags);
}
#endif

}}} // namespaces

