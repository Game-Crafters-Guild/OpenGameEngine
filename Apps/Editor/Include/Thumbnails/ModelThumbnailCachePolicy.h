#pragma once

#include "AssetCore/GUID.h"
#include <cstdint>

namespace GameEngine::ThumbnailCachePolicy
{
// Extraction consumes the prior frame's slot buffers. Keep a spawned model or
// material alive for three fully-ready renders so its cached image has a primed
// pipeline.
inline constexpr uint32_t kSpawnSettleFrames = 3;

inline bool IsSpawnSettling(const GUID& slot, const GUID& spawned, uint32_t settledFrames)
{
    return !spawned.IsNull() && slot == spawned && settledFrames < kSpawnSettleFrames;
}
}
