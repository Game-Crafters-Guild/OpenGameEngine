#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <cstdint>

// Device-level command lists do not derive image layouts on their own: only an
// explicit barrier journals one. A mip that is never transitioned into the
// storage-image state therefore fails twice — the dispatch accesses it in a
// layout its descriptor does not declare (VUID-vkCmdDispatch-imageLayout-00344),
// and a later readback copy resolves oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
// which the driver may satisfy by DISCARDING the subresource. MoltenVK does so
// intermittently, so the mip reads back as all zeros on some runs and not others.
//
// Render-graph passes get these transitions from RGPassBuilder::Read/Write with
// RGTextureRead/Write::Storage; anything recording straight onto a CommandList
// owns them itself.
//
// `from` is the state the mips are really in — CopyDest immediately after an
// upload copy, Undefined for write-first mips whose prior contents are meant to
// be discarded.
inline void TransitionForStorageAccess(GameEngine::Rendering::CommandList* cl,
                                       GameEngine::Rendering::TextureHandle tex,
                                       GameEngine::Rendering::ResourceState from,
                                       uint32_t baseMip, uint32_t mipCount)
{
    using namespace GameEngine::Rendering;
    if (!cl || !tex.IsValid() || mipCount == 0u)
        return;
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, from, ResourceState::UnorderedAccess,
                                                      baseMip, mipCount));
}

// A freshly uploaded mip 0 plus `levels - 1` write-first mips: the exact shape
// every hand-rolled HZB/SPD pyramid build uses.
inline void TransitionPyramidForStorageAccess(GameEngine::Rendering::CommandList* cl,
                                              GameEngine::Rendering::TextureHandle tex,
                                              uint32_t levels)
{
    using namespace GameEngine::Rendering;
    TransitionForStorageAccess(cl, tex, ResourceState::CopyDest, 0u, 1u);
    if (levels > 1u)
        TransitionForStorageAccess(cl, tex, ResourceState::Undefined, 1u, levels - 1u);
}
