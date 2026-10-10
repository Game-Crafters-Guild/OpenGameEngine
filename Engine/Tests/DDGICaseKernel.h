#pragma once

// A test-only DDGI kernel (Engine/Tests/Shaders, compiled with the Rendering
// module's shaders into <build>/Shaders as <name>.comp.spv) that runs one
// production shader function per invocation: it reads authored cases from
// storage binding 0 and writes one vec4 per case to storage binding 1.

#include "StagedTestPaths.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

class DDGICaseKernel
{
  public:
    using Result = std::array<float, 4>;

    // Loads the staged Shaders/<name>.comp.spv.
    bool Load(GameEngine::Rendering::IDevice& device, const std::string& name, std::string& error)
    {
        using namespace GameEngine::Rendering;
        m_Device = &device;
        std::ifstream input(GameEngine::TestPaths::StagedRoot() / "Shaders" / (name + ".comp.spv"),
                            std::ios::binary);
        if (!input.good())
        {
            error = name + ".comp.spv is not staged under <build>/Shaders";
            return false;
        }
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(input),
                                                                 std::istreambuf_iterator<char>());
        for (uint32_t binding = 0; binding < 2; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = bytes;
        desc.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(m_Layout));
        desc.DebugName = name;
        m_Pipeline = device.GetOrCreateComputePipeline(device.InternComputePipeline(desc));
        if (!m_Pipeline.IsValid())
        {
            error = name + ": pipeline creation failed";
            return false;
        }
        return true;
    }

    // Dispatches one invocation per case. Returns an empty vector on failure.
    template <typename Case>
    std::vector<Result> Run(const std::vector<Case>& cases)
    {
        using namespace GameEngine::Rendering;
        std::vector<Result> results(cases.size());
        const BufferHandle input = MakeBuffer(cases.data(), cases.size() * sizeof(Case));
        const BufferHandle output = MakeBuffer(results.data(), results.size() * sizeof(Result));
        bool ok = input.IsValid() && output.IsValid();
        if (ok)
        {
            DescriptorSetDesc setDesc{};
            setDesc.layout = m_Layout;
            const auto set = m_Device->CreateDescriptorSet(setDesc);
            m_Device->UpdateStorageBufferBinding(set, 0, input, 0, cases.size() * sizeof(Case));
            m_Device->UpdateStorageBufferBinding(set, 1, output, 0, results.size() * sizeof(Result));
            auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            commands->SetPipeline(m_Pipeline);
            commands->BindDescriptorSet(0, set, m_Pipeline);
            commands->Dispatch(static_cast<uint32_t>(cases.size()), 1, 1);
            commands->End();
            m_Device->ExecuteCommandLists({commands.get()});
            m_Device->WaitForIdle();
            const auto* mapped = m_Device->MapBuffer(output);
            ok = mapped != nullptr;
            if (ok)
            {
                std::memcpy(results.data(), mapped, results.size() * sizeof(Result));
                m_Device->UnmapBuffer(output);
            }
        }
        for (const BufferHandle buffer : {input, output})
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        return ok ? results : std::vector<Result>{};
    }

  private:
    GameEngine::Rendering::BufferHandle MakeBuffer(const void* data, size_t bytes)
    {
        using namespace GameEngine::Rendering;
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        desc.memoryUsage = BufferMemoryUsage::Readback;
        desc.debugName = "DDGI.CaseKernel.Buffer";
        const BufferHandle buffer = m_Device->CreateBuffer(desc);
        if (!buffer.IsValid())
            return buffer;
        if (auto* mapped = m_Device->MapBuffer(buffer))
        {
            std::memcpy(mapped, data, bytes);
            m_Device->UnmapBuffer(buffer);
            return buffer;
        }
        m_Device->DestroyBuffer(buffer);
        return {};
    }

    GameEngine::Rendering::IDevice* m_Device = nullptr;
    GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};
    GameEngine::Rendering::PipelineHandle m_Pipeline{};
};
