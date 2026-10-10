#pragma once

#include "Assets/Parsers/TextureParser.h"

namespace GameEngine
{

// Covers the remaining supported texture extensions that don't have a dedicated parser yet.
class OtherTextureParser final : public TextureParser
{
  public:
    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".bmp", ".tga", ".dds", ".hdr", ".exr", ".ktx", ".svg"};
    }

    std::string GetName() const override { return "OtherTextureParser"; }

    int GetPriority() const override { return 95; }
};

} // namespace GameEngine
