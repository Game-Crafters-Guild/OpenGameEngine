// Native C++ port of @dgreenheck/ez-tree RNG.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "EZTree/EZTreeRng.h"

namespace GameEngine::EZTree
{

Rng::Rng(uint32 seed)
    : m_W(123456789u + seed)
    , m_Z(987654321u - seed)
{
}

float Rng::Random(float max, float min)
{
    m_Z = 36969u * (m_Z & 65535u) + (m_Z >> 16u);
    m_W = 18000u * (m_W & 65535u) + (m_W >> 16u);

    const uint32 result = (m_Z << 16u) + (m_W & 65535u);
    const float unit = static_cast<float>(static_cast<double>(result) / 4294967296.0);
    return (max - min) * unit + min;
}

} // namespace GameEngine::EZTree
