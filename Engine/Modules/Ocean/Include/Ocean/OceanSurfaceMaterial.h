#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Ocean
{

/// The built-in ocean surface material, in one place for its two consumers:
/// OceanForwardContributor registers it at runtime, and MaterialVariantCook
/// cooks its variants offline so a compiler-less runtime (web) can still draw
/// the surface. Both must agree byte-for-byte on the document and keywords —
/// the shader cache is keyed on the composed source and the variant key.

/// Stable derived GUID for the ocean material (not a real asset GUID).
const GUID& OceanSurfaceMaterialGuid();

/// The material document the surface registers: StandardPBR lighting with the
/// ocean surface shader and clipmap vertex modifier.
MaterialDocument BuildOceanSurfaceMaterialDocument();

/// The pass keywords the surface registers with. IBL is included only when the
/// process is NOT on the compat shader profile — the compat fragment stage has
/// no sampler budget for the IBL trio (same budget that gates world materials).
Rendering::MaterialKeyword OceanSurfaceMaterialKeywords(bool compatProfile);

} // namespace GameEngine::Ocean
