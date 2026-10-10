#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::CBTTerrainECS
{

/// The built-in CBT terrain material, in one place for its two consumers:
/// CBTRenderFeature registers it at runtime, and MaterialVariantCook cooks its
/// variants offline so a compiler-less runtime (web) can still draw the terrain.
/// Both must agree byte-for-byte on the document and keywords — the shader cache
/// is keyed on the composed source and the variant key.

/// Stable derived GUID for the terrain material (not a real asset GUID).
const GUID& CBTTerrainMaterialGuid();

/// The material document the feature registers: StandardPBR lighting over the CBT
/// surface shader, with a fully procedural vertex stage (no vertex buffer — the
/// clamp turns customVertexShader into CustomVertexShader | HasVertexOutputMod and
/// forces vertexFlags = None, which is why the cook cannot use the default variant
/// table and must name its variants explicitly).
MaterialDocument BuildCBTTerrainMaterialDocument();

/// The pass keywords the feature registers with: lit Forward+ with shadows and IBL.
Rendering::MaterialKeyword CBTTerrainMaterialKeywords();

} // namespace GameEngine::CBTTerrainECS
