#pragma once

// BonePaletteLayout — single source of truth for the CPU side of the
// bone palette atlas storage layout. Mirrors the GLSL helper at
// Engine/Modules/Rendering/Shaders/Includes/bone_palette.glsl.
//
// Storage: each bone occupies 3 consecutive vec4 entries in the atlas
// SSBO. The 3 rows are the TOP 3 rows of a column-major mat4 (its
// bottom row is always (0,0,0,1) for an affine transform; we drop it
// because skinning produces only affine results — 25% bandwidth /
// memory saving with NO precision loss).

#include "Types/Types.h"

namespace GameEngine::Engine::Renderer::BonePaletteLayout
{
    // Number of float values stored per bone in the atlas.
    constexpr uint32 kFloatsPerBone = 12;

    // Number of vec4 rows stored per bone.
    constexpr uint32 kVec4sPerBone = 3;

    // Convert a column-major mat4 (16 floats) into 3 packed rows
    // (12 floats). The bottom row of `mat4Cm` is dropped — caller is
    // responsible for ensuring the matrix is affine. Layout matches
    // ge_StoreBonePalette in bone_palette.glsl exactly.
    inline void PackMat4ToRows(const float* mat4Cm /*16*/, float* rowsOut /*12*/)
    {
        // Column-major: mat4Cm[col*4 + row]. Output row R packs:
        //   (mat[0][R], mat[1][R], mat[2][R], mat[3][R]).
        for (uint32 r = 0; r < 3u; ++r)
        {
            rowsOut[r * 4u + 0u] = mat4Cm[0u * 4u + r];
            rowsOut[r * 4u + 1u] = mat4Cm[1u * 4u + r];
            rowsOut[r * 4u + 2u] = mat4Cm[2u * 4u + r];
            rowsOut[r * 4u + 3u] = mat4Cm[3u * 4u + r];
        }
    }

    // Inverse of PackMat4ToRows. Reconstructs a full column-major mat4
    // by appending the implied (0, 0, 0, 1) bottom row. Useful for tests.
    inline void UnpackRowsToMat4(const float* rowsIn /*12*/, float* mat4CmOut /*16*/)
    {
        for (uint32 c = 0; c < 4u; ++c)
        {
            mat4CmOut[c * 4u + 0u] = rowsIn[0u * 4u + c];
            mat4CmOut[c * 4u + 1u] = rowsIn[1u * 4u + c];
            mat4CmOut[c * 4u + 2u] = rowsIn[2u * 4u + c];
            mat4CmOut[c * 4u + 3u] = (c == 3u) ? 1.0f : 0.0f;
        }
    }
} // namespace GameEngine::Engine::Renderer::BonePaletteLayout
