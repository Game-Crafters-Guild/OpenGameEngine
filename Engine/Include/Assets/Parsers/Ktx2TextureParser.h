#pragma once

#include "Assets/Parsers/TextureParser.h"

namespace GameEngine
{

class Ktx2TextureParser final : public TextureParser
{
  public:
    std::vector<std::string> GetSupportedExtensions() const override { return {".ktx2"}; }

    std::string GetName() const override { return "Ktx2TextureParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
