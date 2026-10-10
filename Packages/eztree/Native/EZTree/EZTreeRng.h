#pragma once

// Native C++ port of @dgreenheck/ez-tree RNG.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "Types/Types.h"

namespace GameEngine::EZTree
{

class Rng
{
public:
    explicit Rng(uint32 seed = 0);

    float Random(float max = 1.0f, float min = 0.0f);

private:
    uint32 m_W = 0;
    uint32 m_Z = 0;
};

} // namespace GameEngine::EZTree
