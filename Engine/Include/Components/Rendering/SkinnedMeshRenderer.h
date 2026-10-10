#pragma once

#include "Types/Types.h"

namespace GameEngine
{
namespace Components
{

struct SkinnedMeshRenderer
{
    uint32 meshId = 0;     // mesh with bone weights
    uint32 skeletonId = 0; // [DoNotSerialize] process-local cache resolved from SkeletonRef/model source
    // Default to the primary scene layer (bit 0). Views can opt into
    // additional layers (e.g. thumbnails) via RenderServices::SetViewRenderLayerMask.
    uint32 renderLayerMask = 1u;
    bool castShadows = true;
    bool receiveShadows = true;
};

} // namespace Components
} // namespace GameEngine
