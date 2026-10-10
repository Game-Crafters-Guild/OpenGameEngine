// GPU-executed coverage for the camera-relative sector decode. Pushes crafted
// GPUInstances (240 B, the SHIPPED std430 layout) through the real
// render_origin_decode_probe.comp — which reads sectorPacked at the shipped
// struct offset and runs the shipped ge_UnpackInstanceSector — and asserts the
// device-decoded ivec3 matches both the C++ PackSector input and the C++
// UnpackSector mirror. This closes the review's "zero device coverage for the
// GLSL decode" gap: a transposed mask / wrong shift / sign-extension slip / or a
// std430 offset drift of sectorPacked fails on real hardware here.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderOrigin.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
namespace Origin = GameEngine::Engine::Renderer;

namespace
{

struct DecodedSector
{
    int32_t x, y, z, w;
};

class RenderOriginDecodeComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/render_origin_decode_probe.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "render_origin_decode_probe.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout.debugName = "RenderOriginDecode_SetLayout";
        for (uint32_t i = 0; i < 2u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "RenderOriginDecodeProbe";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.debugName = name;
        BufferHandle h = m_Device->CreateBuffer(d);
        m_Buffers.push_back(h);
        return h;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

TEST_F(RenderOriginDecodeComputeTest, DeviceDecodeMatchesPack)
{
    // Cases span sign, the Y split (p0[21..31] / p1[0..9]) with non-zero Y, the
    // 21-bit extremes, and the live editor value (3595,0,3595).
    const std::array<Components::WorldSectorCoord, 8> cases = {{
        {0, 0, 0},
        {3595, 0, 3595},          // the live tagged-sphere value
        {1, 1, 1},
        {-1, -1, -1},
        {12, -2048, 4096},        // exercises the Y split with a non-zero signed Y
        {-3595, 1234, -6221},     // ~Earth radius, all axes signed + Y across the split
        {Origin::kSectorAxisMax, Origin::kSectorAxisMin, Origin::kSectorAxisMax},
        {Origin::kSectorAxisMin, Origin::kSectorAxisMax, Origin::kSectorAxisMin},
    }};

    const uint32_t n = static_cast<uint32_t>(cases.size());
    std::vector<GPUInstance> instances(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        instances[i] = GPUInstance{}; // zeroes everything incl. the two packed words
        Origin::PackSector(cases[i].x, cases[i].y, cases[i].z, instances[i].sectorPacked[0],
                           instances[i].sectorPacked[1]);
    }

    BufferHandle inBuf = MakeHostBuffer(n * sizeof(GPUInstance), "DecodeProbe.Instances");
    {
        void* mapped = m_Device->MapBuffer(inBuf);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, instances.data(), n * sizeof(GPUInstance));
        m_Device->UnmapBuffer(inBuf);
    }

    BufferHandle outBuf = MakeHostBuffer(n * sizeof(DecodedSector), "DecodeProbe.Sectors");
    {
        void* mapped = m_Device->MapBuffer(outBuf);
        ASSERT_NE(mapped, nullptr);
        std::memset(mapped, 0xEE, n * sizeof(DecodedSector)); // poison so a no-write is caught
        m_Device->UnmapBuffer(outBuf);
    }

    DescriptorSetDesc setDesc{};
    setDesc.layout = m_Layout;
    setDesc.transient = true;
    setDesc.debugName = "DecodeProbe.DS";
    DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);
    m_Device->UpdateStorageBufferBinding(ds, 0, inBuf, 0, n * sizeof(GPUInstance));
    m_Device->UpdateStorageBufferBinding(ds, 1, outBuf, 0, n * sizeof(DecodedSector));

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(m_Pipeline);
    cl->BindDescriptorSet(0, ds, m_Pipeline);
    cl->Dispatch(n, 1, 1);
    cl->End();
    m_Device->ExecuteCommandLists({cl.get()});
    m_Device->WaitForIdle();

    std::vector<DecodedSector> out(n);
    {
        void* mapped = m_Device->MapBuffer(outBuf);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(out.data(), mapped, n * sizeof(DecodedSector));
        m_Device->UnmapBuffer(outBuf);
    }

    for (uint32_t i = 0; i < n; ++i)
    {
        // C++ mirror decode of the same bytes (sanity that the pack is what we think).
        int32_t cx, cy, cz;
        Origin::UnpackSector(instances[i].sectorPacked[0], instances[i].sectorPacked[1], cx, cy, cz);
        ASSERT_EQ(cx, cases[i].x);
        ASSERT_EQ(cy, cases[i].y);
        ASSERT_EQ(cz, cases[i].z);

        // The DEVICE decode must equal the input — proves the shipped struct
        // offset + GLSL decode agree with the C++ pack on real hardware.
        EXPECT_EQ(out[i].x, cases[i].x) << "case " << i << " x";
        EXPECT_EQ(out[i].y, cases[i].y) << "case " << i << " y";
        EXPECT_EQ(out[i].z, cases[i].z) << "case " << i << " z";
        EXPECT_EQ(out[i].w, 1) << "case " << i << " sentinel (shader ran + wrote)";
    }
}
