#pragma once

// A shipped DDGI compute kernel, loaded from its package exactly as
// DDGIProbeFeature::LoadKernel loads it, so GPU tests exercise the bytes the
// engine runs. Bindings are resolved by reflected name rather than by number.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct DDGIShippedKernel
{
    static constexpr uint32_t kMissingBinding = ~0u;

    GameEngine::Rendering::ShaderMeta Meta;
    GameEngine::Rendering::DescriptorSetLayoutDesc Layout;
    GameEngine::Rendering::PipelineHandle Pipeline;

    // Set-0 binding the kernel declares under `name`, or kMissingBinding.
    uint32_t Binding(std::string_view name) const
    {
        for (const auto& set : Meta.Sets)
            if (set.Set == 0)
                for (const auto& binding : set.Bindings)
                    if (binding.Name == name)
                        return binding.Binding;
        return kMissingBinding;
    }
};

// Loads "Shaders/<name>.shaderpkg". Returns false with `error` set when the
// package or its pipeline is unavailable.
inline bool LoadDDGIShippedKernel(GameEngine::Rendering::IDevice& device, const std::string& name,
                                  DDGIShippedKernel& out, std::string& error)
{
    using namespace GameEngine::Rendering;
    ShaderPackage package{};
    const std::string path = "Shaders/" + name + ".shaderpkg";
    if (!LoadShaderPkg(path, device.PreferredShaderSource(), package, &error))
        return false;
    const auto compute = package.stageBytes.find("cs");
    if (compute == package.stageBytes.end() || compute->second.empty())
    {
        error = path + " has no compute stage";
        return false;
    }
    out.Meta = std::move(package.meta);
    ComputePipelineDesc desc{};
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(compute->second));
    desc.DebugName = name;
    const auto keepSetZero = [&out](uint32_t set, DescriptorSetLayoutDesc& layout)
    {
        if (set == 0)
            out.Layout = layout;
    };
    MaterialHelper::ApplyShaderMetaToComputeDesc(device, out.Meta, desc, MaterialBuilder::MergeMode::Auto,
                                                 {true, 128}, keepSetZero, &error);
    out.Pipeline = device.GetOrCreateComputePipeline(device.InternComputePipeline(std::move(desc)));
    if (!out.Pipeline.IsValid())
    {
        error = path + ": pipeline creation failed " + error;
        return false;
    }
    return true;
}
