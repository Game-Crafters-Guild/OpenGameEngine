#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

#include <string>
#include <vector>

namespace GameEngine::ShaderGraph
{

std::vector<SgNodeOverload> ReadTaggedFunctionOverloads(const std::string& source,
                                                        const std::string& expectedBaseName);

} // namespace GameEngine::ShaderGraph
