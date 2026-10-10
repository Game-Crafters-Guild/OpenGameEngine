#pragma once

#include "AssetCore/AssetRegistry.h"

namespace GameEngine
{

// Registers editor-layer asset type factories (currently UI layout/style factories used by the editor UI).
void RegisterEditorAssetTypes(AssetTypeRegistry& typeRegistry);

} // namespace GameEngine
