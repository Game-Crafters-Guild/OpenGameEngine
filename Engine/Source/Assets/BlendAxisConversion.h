#pragma once

// Blender source frame for MakeEngineConversion. Owned by the .blend loader
// (ModelAssetLoadBlend.cpp).

#include "ModelAxisConversion.h"

namespace GameEngine::BlendImport
{

// Blender world: +X right, +Y forward, +Z up. Engine: +X right, +Y up, +Z
// forward. This is the Y/Z swap (x,y,z)->(x,z,y). Default Mirror X is the
// Unity-equivalent RH→LH bake (x,y,z)->(-x,z,y).
inline ModelImport::AxisConversion SourceConversion()
{
    ModelImport::AxisConversion c{};
    c.m[0][0] = 1.0f; c.m[0][1] = 0.0f; c.m[0][2] = 0.0f;
    c.m[1][0] = 0.0f; c.m[1][1] = 0.0f; c.m[1][2] = 1.0f;
    c.m[2][0] = 0.0f; c.m[2][1] = 1.0f; c.m[2][2] = 0.0f;
    c.reverseWinding = ModelImport::Determinant3x3(c.m) < 0.0f;
    return c;
}

} // namespace GameEngine::BlendImport
