#pragma once

// glTF 2.0 source frame for MakeEngineConversion. Owned by the glTF loader
// (ModelAsset::LoadGLTF) and the glTF clip path in AnimationClip.

#include "ModelAxisConversion.h"

namespace GameEngine::GltfImport
{

// +X right, +Y up, +Z toward the viewer — engine target axes, so this is
// identity. Default Mirror X is the Unity-equivalent RH→LH bake.
inline ModelImport::AxisConversion SourceConversion()
{
    return {};
}

} // namespace GameEngine::GltfImport
