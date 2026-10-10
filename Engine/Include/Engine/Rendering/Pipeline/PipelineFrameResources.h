#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer::Pipeline
{

// Per-view frame-local target VALUES for the RenderGraph path. ViewDesc.targets
// carries old-graph logical ids; the RenderGraph spine value-passes instead (the
// slice-3 contract: effective-target resolution belongs to the caller).
struct ViewTargetsRG
{
    ::GameEngine::Rendering::ViewId View = 0;
    ::GameEngine::Rendering::RenderGraph::RGTexture Color{};
    ::GameEngine::Rendering::RenderGraph::RGTexture Depth{};
    ::GameEngine::Rendering::RenderGraph::RGTexture Resolve{}; // invalid = none
};

// A pipeline buffer the way the declaration-time binding table consumes it:
// a physical {buffer, offset, size}. Graph is valid only for device-local
// pipeline buffers — those need declared Read/Write edges; upload-ring
// allocs are host-coherent and get none.
struct PipelineBufferBindingRG
{
    ::GameEngine::Rendering::BufferHandle Buffer{};
    uint64_t Offset = 0;
    uint64_t Size = 0;
    ::GameEngine::Rendering::RenderGraph::RGBuffer Graph{};
    bool IsValid() const { return Buffer.IsValid(); }
};

// The pipeline's per-frame blackboard, rebuilt from scratch on every Declare.
// Values are RGFrame-local; Frame is the identity guard (same pattern as
// RenderServices::ViewFrameRG — an entry from a dead frame must read as
// absent, never as a usable id).
struct PipelineFrameResources
{
    struct Key
    {
        ::GameEngine::Rendering::ViewId ViewId = 0; // 0 = frame scope
        std::string Name;
        bool operator==(const Key& o) const { return ViewId == o.ViewId && Name == o.Name; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(static_cast<uint32_t>(k.ViewId));
            h ^= std::hash<std::string>{}(k.Name) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };
    // Per-view scalars the pre-pass derives once per frame.
    struct ViewInfo
    {
        bool ResolveFromPipeline = false;
        // Internal (raster) extent — equals Output* except under an active
        // internal-resolution split.
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        // Display extent (the caller's targets). Post-upscale resources size
        // against this ("extent.basis": "output" in the blueprint).
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
    };

    ::GameEngine::Rendering::RenderGraph::RGFrame* Frame = nullptr;
    // BeginFrame index of the incarnation Declare ran against. RGFrame
    // pointers are reused across frames (stream identity), so pointer
    // equality alone validates a stale incarnation — consumers compare
    // (Frame, FrameIndex) via FrameResourcesFor.
    uint64_t FrameIndex = 0;
    std::unordered_map<Key, ::GameEngine::Rendering::RenderGraph::RGTexture, KeyHash> Textures;
    std::unordered_map<Key, PipelineBufferBindingRG, KeyHash> Buffers;
    std::unordered_map<::GameEngine::Rendering::ViewId, ViewInfo> Views;

    // Per-view entries first, then frame-scope — the enumeration shape the
    // old ForEachBufferRef had (per-view names shadow frame-scope ones at
    // the binding table's last-add-wins upsert).
    template <class Fn>
    void ForEachBufferBinding(::GameEngine::Rendering::ViewId viewId, Fn&& fn) const
    {
        if (viewId != 0)
        {
            for (const auto& [key, b] : Buffers)
            {
                if (key.ViewId == 0)
                    fn(key.Name, b);
            }
        }
        for (const auto& [key, b] : Buffers)
        {
            if (key.ViewId == viewId)
                fn(key.Name, b);
        }
    }
};

} // namespace GameEngine::Engine::Renderer::Pipeline
