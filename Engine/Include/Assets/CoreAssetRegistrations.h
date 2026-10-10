#pragma once

#include "AssetCore/AssetRegistry.h"

namespace GameEngine
{

// Registers core engine-owned asset type factories (textures, models, shaders, etc.)
// into the AssetTypeRegistry. This is called early during Engine/Editor bootstrap so
// startup indexing can assign correct types and assets can be instantiated.
void RegisterCoreAssetTypes(AssetTypeRegistry& typeRegistry);

} // namespace GameEngine
