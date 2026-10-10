#pragma once

#include "Assets/Parsers/TextureParser.h"

namespace GameEngine
{

class JpegTextureParser final : public TextureParser
{
  public:
    std::vector<std::string> GetSupportedExtensions() const override { return {".jpg", ".jpeg"}; }

    std::string GetName() const override { return "JpegTextureParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
