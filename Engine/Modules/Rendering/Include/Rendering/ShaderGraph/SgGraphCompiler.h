#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

namespace GameEngine::ShaderGraph
{

class SgGraphCompiler
{
  public:
    static SgCompileResult Compile(const SgGraphDocument& doc, const SgNodeLibraryIndex& library);
};

} // namespace GameEngine::ShaderGraph
