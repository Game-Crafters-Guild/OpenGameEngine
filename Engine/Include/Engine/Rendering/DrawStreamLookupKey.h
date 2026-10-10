// DrawStreamLookupKey: the single derivation of the axes a batch key's draw
// range was published under. Every reader of GPUDrawStreamBuilder's range map
// — the world color pass, the depth/shadow recorder, and the debug server's
// draw-attribution surfaces — resolves its lookup here, so a change to the
// publisher's keying cannot leave one reader behind.

#pragma once

#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Engine/Rendering/WorldDrawBuilder.h"

#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"

#include <cstdint>
#include <span>

namespace GameEngine
{
namespace Engine
{
namespace Renderer
{

/// The two RESOLVED axes GPUDrawStreamBuilder keys a published range on.
/// Neither is a raw BatchKey field: the class axis carries the P2 color-class
/// id (or an R1.5 shared-depth sentinel on shadow slices), and the mesh axis
/// carries the pool-group id whenever draw consolidation is active.
struct DrawStreamLookupKey
{
    uint32_t classKey;
    uint32_t meshKey;
};

/// Resolve the (class, mesh) pair `key`'s range lives under.
///
/// `cascadeIndex` selects the slice family. kCascadeIndexNone covers the color
/// pass and the main-view depth prepass, which share one table and therefore
/// always key on the color class; shadow slices remap shared-depth-eligible
/// casters onto the per-side sentinels, so `depthClass` is consulted only
/// there. A Color-table consumer passes MaterialDepthClass::MaterialDependent
/// whatever its cascade index (cascade None, or a probe face's own index) and
/// keeps the color class.
///
/// `meshPoolGroups` is RenderServices::MeshPoolGroupSpanForDraws(): gpu mesh
/// index -> pool group. Empty means consolidation is off and the mesh axis
/// stays the meshIndex; a meshIndex past the span has no live plan entry and
/// resolves to kAbsentGroup, whose rows are scattered into but never drawn.
inline DrawStreamLookupKey ResolveDrawStreamLookupKey(
    const WorldDrawBuilder::BatchKey& key,
    uint8_t cascadeIndex,
    ::GameEngine::Rendering::MaterialDepthClass depthClass,
    std::span<const uint32_t> meshPoolGroups)
{
    using Streams = ::GameEngine::Rendering::GPUDrawStreamBuilder;
    using DepthClass = ::GameEngine::Rendering::MaterialDepthClass;

    DrawStreamLookupKey out{};

    out.classKey = key.colorClassId;
    if (cascadeIndex != Streams::kCascadeIndexNone)
    {
        if (depthClass == DepthClass::EligibleSingleSided)
            out.classKey = Streams::kSharedDepthSingleSidedSentinel;
        else if (depthClass == DepthClass::EligibleDoubleSided)
            out.classKey = Streams::kSharedDepthDoubleSidedSentinel;
    }

    if (meshPoolGroups.empty())
        out.meshKey = key.meshIndex;
    else
        out.meshKey = key.meshIndex < meshPoolGroups.size()
                          ? meshPoolGroups[key.meshIndex]
                          : ::GameEngine::Rendering::MeshPoolGroupPlan::kAbsentGroup;

    return out;
}

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
