#pragma once

// FBX emission as ImportedMaterialData::EmissiveColor carries it: the emission
// color times its factor, which the material bridge splits into a [0, 1] color
// and a luminance the surface multiplies with the emissive texture.

#include <ufbx.h>

namespace GameEngine::FbxImport
{

/// Writes the emission of `material` into `outRgb`, left untouched when the file
/// authors none. With an emissive texture bound, the texture is the emission
/// color and the constant `EmissiveColor` reads as white: exporters leave it at
/// an arbitrary default (gray 0.5) beside a texture, so only `EmissiveFactor`
/// scales the texture. Without a texture, the constant times the factor.
inline void ReadEmissiveColor(const ufbx_material& material, bool textureBound, float (&outRgb)[3])
{
    const bool pbr = material.pbr.emission_color.has_value;
    const ufbx_material_map& color = pbr ? material.pbr.emission_color : material.fbx.emission_color;
    const ufbx_material_map& factor = pbr ? material.pbr.emission_factor : material.fbx.emission_factor;
    if (!color.has_value && !textureBound)
        return;
    const float scale = factor.has_value ? static_cast<float>(factor.value_real) : 1.0f;
    for (int i = 0; i < 3; ++i)
        outRgb[i] = (textureBound ? 1.0f : static_cast<float>(color.value_vec3.v[i])) * scale;
}

} // namespace GameEngine::FbxImport
