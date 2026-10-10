#pragma once

// RenderGraph barrier-IR layout ↔ engine ResourceState mapping. Used for (a) seeding
// imported resources at the pool's tracked state and (b) the end-of-frame
// write-back. Pool-backed resources form a CLOSED LOOP through these two
// functions, so every state the pool can hold round-trips exactly; the lossy
// entries (Common) only occur for externally-provided states, where the caller
// supplies the truth explicitly.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

namespace GameEngine::Rendering::RenderGraph
{

inline ResourceState ToResourceState(RGImageLayout l)
{
    switch (l)
    {
        case RGImageLayout::Undefined: return ResourceState::Undefined;
        case RGImageLayout::General: return ResourceState::UnorderedAccess;
        case RGImageLayout::ColorAttachment: return ResourceState::RenderTarget;
        case RGImageLayout::DepthAttachment: return ResourceState::DepthWrite;
        case RGImageLayout::DepthReadOnly: return ResourceState::DepthRead;
        case RGImageLayout::ShaderReadOnly: return ResourceState::ShaderResource;
        case RGImageLayout::TransferSrc: return ResourceState::CopySource;
        case RGImageLayout::TransferDst: return ResourceState::CopyDest;
        case RGImageLayout::Present: return ResourceState::Common;
    }
    return ResourceState::Undefined;
}

inline RGImageLayout ToImageLayout(ResourceState s)
{
    switch (s)
    {
        case ResourceState::ShaderResource: return RGImageLayout::ShaderReadOnly;
        case ResourceState::RenderTarget: return RGImageLayout::ColorAttachment;
        case ResourceState::DepthWrite: return RGImageLayout::DepthAttachment;
        case ResourceState::DepthRead: return RGImageLayout::DepthReadOnly;
        case ResourceState::DepthSampled: return RGImageLayout::DepthReadOnly;
        case ResourceState::UnorderedAccess: return RGImageLayout::General;
        case ResourceState::CopySource: return RGImageLayout::TransferSrc;
        case ResourceState::CopyDest: return RGImageLayout::TransferDst;
        default:
            // Unknown/Common/buffer-ish states: Undefined forces a discarding
            // transition — only correct because the pool loop never stores
            // these for textures whose content must survive.
            return RGImageLayout::Undefined;
    }
}

} // namespace GameEngine::Rendering::RenderGraph
