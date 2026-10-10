#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Types/StringUtils.h"
#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes::Detail
{
inline bool TryGetSet0BindingByName(const GameEngine::Rendering::ShaderMeta& meta,
                                   const std::string& name,
                                   uint32_t& outBinding,
                                   GameEngine::Rendering::DescriptorType& outType)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set != 0)
            continue;
        for (const auto& b : s.Bindings)
        {
            if (b.Name == name)
            {
                outBinding = b.Binding;
                outType = static_cast<GameEngine::Rendering::DescriptorType>(b.Type);
                return true;
            }
        }
        return false;
    }
    return false;
}

// Resolve a set0 binding's reflected name (for diagnostics) by binding index.
inline std::string Set0BindingName(const GameEngine::Rendering::ShaderMeta& meta, uint32_t binding)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set != 0)
            continue;
        for (const auto& b : s.Bindings)
        {
            if (b.Binding == binding)
                return b.Name;
        }
        return {};
    }
    return {};
}

// Bind a storage image (a compute/fullscreen write target) to its reflected
// set0 binding, resolving the binding index from the shader's reflected name
// instead of a hard-coded literal.
//
// The AO / AutoExposure / DepthResolve / CSM node families each declare their
// storage-image write target with a fixed GLSL name (uDepthResolved, uAO,
// uAOOut, uMoments). Resolving the index from reflection keeps the C++ bind in
// lockstep with a shader-side layout edit, where a hard-coded index would
// silently write the wrong binding.
//
// NamedDescriptorWriter has no storage-image path (DescriptorWriter models only
// buffers/textures/samplers), and the reflected binding TYPE cannot be trusted
// for images — ShaderMeta's kStorageImage(4)/kCombinedImageSampler(5) do not
// line up with DescriptorType's CombinedImageSampler(4)/StorageImage(5) — so the
// caller-known StorageImage type is written explicitly and only the index comes
// from reflection.
//
// Returns true when the name resolved in set0 and the write was issued.
inline bool BindStorageImageByName(GameEngine::Rendering::IDevice* device,
                                   GameEngine::Rendering::DescriptorSetHandle set0,
                                   const GameEngine::Rendering::ShaderMeta& meta,
                                   const std::string& name,
                                   GameEngine::Rendering::TextureHandle image)
{
    if (!device || !set0.IsValid() || !image.IsValid())
        return false;
    uint32_t binding = 0;
    GameEngine::Rendering::DescriptorType reflectedType{};
    if (!TryGetSet0BindingByName(meta, name, binding, reflectedType))
        return false;
    GameEngine::Rendering::DescriptorSetUpdate u{};
    u.binding = binding;
    u.type = GameEngine::Rendering::DescriptorType::StorageImage;
    u.textures = {image};
    device->UpdateDescriptorSet(set0, u);
    return true;
}

// Bind a safe default for every reflected set0 binding the node left unwritten.
//
// FullscreenShaderNode and ComputeShaderNode build set0 from shader reflection
// (the layout declares EVERY binding the shader references) but only write the
// bindings the render-graph node JSON wires. An unwired binding the shader
// statically references is an unbound descriptor — a VUID + undefined read on
// devices without VK_EXT_robustness2/nullDescriptor (this device). Filling the
// gap with a valid placeholder keeps the draw/dispatch legal.
//
// Real binds always win: only indices NOT in `writtenBindings` are touched.
// StorageImage write targets are never defaulted (a write target must be real),
// but a missing one is still reported as a genuine wiring error.
//
// Returns true if any binding was defaulted. `loggedOnce` gates a single
// per-node warning listing the auto-defaulted (and missing storage-image)
// bindings so the gap stays diagnosable.
inline bool DefaultFillUnwrittenSet0Bindings(
    GameEngine::Rendering::IDevice* device,
    GameEngine::Rendering::DescriptorSetHandle set0,
    const GameEngine::Rendering::DescriptorSetLayoutDesc& layout,
    const GameEngine::Rendering::ShaderMeta& meta,
    const std::unordered_set<uint32_t>& writtenBindings,
    GameEngine::Rendering::BufferHandle defaultBuffer,
    GameEngine::Rendering::TextureHandle defaultTexture,
    GameEngine::Rendering::SamplerHandle defaultSampler,
    const std::string& nodeId,
    bool& loggedOnce)
{
    using GameEngine::Rendering::DescriptorType;
    using GameEngine::Rendering::DescriptorSetUpdate;

    if (!device || !set0.IsValid())
        return false;

    bool anyDefaulted = false;
    std::string defaultedList;  // names of placeholder-bound descriptors
    std::string missingList;    // names of genuinely-missing write targets

    auto appendName = [&meta](std::string& list, uint32_t binding) {
        std::string name = Set0BindingName(meta, binding);
        if (name.empty())
            name = "binding#" + std::to_string(binding);
        if (!list.empty())
            list += ", ";
        list += name;
    };

    for (const auto& lb : layout.bindings)
    {
        if (writtenBindings.find(lb.binding) != writtenBindings.end())
            continue; // real bind already provided — never override it

        switch (lb.type)
        {
        case DescriptorType::StorageBuffer:
        case DescriptorType::UniformBuffer:
        {
            if (!defaultBuffer.IsValid())
            {
                appendName(missingList, lb.binding);
                break;
            }
            DescriptorSetUpdate u{};
            u.binding = lb.binding;
            u.type = lb.type;
            u.buffers = {defaultBuffer};
            device->UpdateDescriptorSet(set0, u);
            appendName(defaultedList, lb.binding);
            anyDefaulted = true;
            break;
        }
        case DescriptorType::CombinedImageSampler:
        {
            if (!defaultTexture.IsValid() || !defaultSampler.IsValid())
            {
                appendName(missingList, lb.binding);
                break;
            }
            DescriptorSetUpdate u{};
            u.binding = lb.binding;
            u.type = DescriptorType::CombinedImageSampler;
            u.textures = {defaultTexture};
            u.samplers = {defaultSampler};
            device->UpdateDescriptorSet(set0, u);
            appendName(defaultedList, lb.binding);
            anyDefaulted = true;
            break;
        }
        case DescriptorType::Texture:
        {
            if (!defaultTexture.IsValid())
            {
                appendName(missingList, lb.binding);
                break;
            }
            DescriptorSetUpdate u{};
            u.binding = lb.binding;
            u.type = DescriptorType::Texture;
            u.textures = {defaultTexture};
            device->UpdateDescriptorSet(set0, u);
            appendName(defaultedList, lb.binding);
            anyDefaulted = true;
            break;
        }
        case DescriptorType::Sampler:
        {
            if (!defaultSampler.IsValid())
            {
                appendName(missingList, lb.binding);
                break;
            }
            DescriptorSetUpdate u{};
            u.binding = lb.binding;
            u.type = DescriptorType::Sampler;
            u.samplers = {defaultSampler};
            device->UpdateDescriptorSet(set0, u);
            appendName(defaultedList, lb.binding);
            anyDefaulted = true;
            break;
        }
        case DescriptorType::StorageImage:
            // A storage-image write target must be real; defaulting it would
            // produce a useless dispatch. These are always explicitly provided,
            // so an unwired one is a genuine wiring error worth surfacing.
            appendName(missingList, lb.binding);
            break;
        case DescriptorType::AccelerationStructure:
            // There is no placeholder TLAS to fall back to, and a ray query
            // against an unwritten descriptor slot faults rather than shading
            // dark — report it as missing instead of pretending it is bound.
            appendName(missingList, lb.binding);
            break;
        }
    }

    if (!loggedOnce && (!defaultedList.empty() || !missingList.empty()))
    {
        loggedOnce = true;
        if (!defaultedList.empty())
        {
            Logger::Log::Warning(
                "RenderPipeline node '{}': auto-bound safe defaults for unwired set0 "
                "bindings [{}] — wire these in the rendergraph to silence this.",
                nodeId, defaultedList);
        }
        if (!missingList.empty())
        {
            Logger::Log::Warning(
                "RenderPipeline node '{}': set0 bindings [{}] are unwired and have no safe "
                "default (storage-image write targets must be explicitly provided).",
                nodeId, missingList);
        }
    }

    return anyDefaulted;
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes::Detail
