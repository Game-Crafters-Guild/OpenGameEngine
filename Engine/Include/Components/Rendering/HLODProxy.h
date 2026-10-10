#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Tag identifying an engine-spawned HLOD proxy entity (design v0.2 §5.1). A proxy
// is an ordinary MeshRenderer whose geometry is a cluster's members merged into
// one cluster-relative mesh per material; HlodRuntime spawns one proxy entity per
// (cluster, submesh) at scene-load reconcile and carries the runtime cluster
// index here so the HLODSelectSystem can toggle the whole cluster's proxies as a
// unit. A proxy also carries RuntimeOnlyEntity (never serialized, C2) and starts
// evicted (MeshGPUData.hlodEvicted = true) so members are the resident set until
// the cluster switches far.
//
// @ge-no-add        spawned by the HLOD runtime, never added by hand in the editor
// [DoNotSerialize]  proxies are reconstructed from the baked .gehlod on load
struct HLODProxy
{
    // Index into the runtime cluster table (HlodRuntime::GetClusters()). All of a
    // cluster's proxy submesh entities share the same ClusterId.
    uint32 ClusterId = 0xFFFFFFFFu;
};

} // namespace Components
} // namespace GameEngine
