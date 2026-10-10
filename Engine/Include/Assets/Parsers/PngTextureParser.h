#pragma once

#include "Assets/Parsers/TextureParser.h"

namespace GameEngine
{

class PngTextureParser final : public TextureParser
{
  public:
    std::vector<std::string> GetSupportedExtensions() const override { return {".png"}; }

    std::string GetName() const override { return "PngTextureParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
