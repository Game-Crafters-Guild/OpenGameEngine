#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

namespace GameEngine::ShaderGraph
{

void RegisterPseudoNodes(SgNodeLibraryIndex& index);

const SgNodeDefinition* FindPseudoNode(const SgNodeLibraryIndex& index, const std::string& typeId);
bool IsPseudoNodeType(const std::string& typeId);

std::string ResolveNodeTypeAlias(const std::string& typeId);

} // namespace GameEngine::ShaderGraph
