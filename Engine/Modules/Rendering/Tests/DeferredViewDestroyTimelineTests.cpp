// Compute-queue lifetime proofs on one heavy compute rig: the cross-queue
// retire-tag invariant for deferred image-view destruction, and the frame
// slot's compute fence.
//
// QueueDeferredTextureViewDestroy tags an entry with the value each queue's next
// submit will signal, and BeginFrame's drain destroys it once every queue has
// passed its own tag. The work below runs on the COMPUTE queue, which is where
// the HZB build passes run under async compute, so a scheme that keys retirement
// on the graphics timeline alone cannot prove this view's lifetime and frees it
// while the dispatches referencing it are still executing. The layer reports
// that as VUID-vkDestroyImageView-imageView-01026 ("currently in use by
// VkDescriptorSet").
//
// Destroy-before-submit must be exactly as safe as destroy-after-submit; the
// ordering is the only variable between the two cases below, and the
// destroy-after-submit case is the control that proves the rig is not simply
// always red. Under a graphics-only scheme BOTH go red, because the graphics
// timeline says nothing about compute work either side of the submit.
//
// The frame-slot case: BeginFrame must not reuse a slot (host-resetting its
// timestamp queries) while compute work submitted under it still runs, so the
// slot's compute fence must cover thread-pool lists that retire by timeline.

#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "TestUtils.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr const char* kViewInUseVuid = "VUID-vkDestroyImageView-imageView-01026";

// The race needs the recorded frame to still be executing when the NEXT
// BeginFrame runs its drain. The CPU covers submit -> drain in microseconds, so
// the frame is given enough GPU work to outlast that by orders of magnitude.
constexpr uint32_t kExtent = 2048u;
constexpr uint32_t kDispatchRepeats = 48u;
constexpr uint32_t kFrameCount = 6u;

struct HzbBuildPC
{
    uint32_t dstW;
    uint32_t dstH;
    uint32_t srcW;
    uint32_t srcH;
    uint32_t mode; // 0 = plain copy
};

void SetHeadlessValidationEnv()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
    // This suite deliberately provokes a validation ERROR to measure it; the
    // default-on validation assert would abort the process before the
    // assertions run.
    _putenv_s("GE_VK_VALIDATION_ASSERT", "0");
    // Forced, not incidental: on the descriptor-buffer path a storage-image
    // descriptor is an opaque blob in a buffer, so no VkDescriptorSet holds the
    // view and the layer has nothing to report an early destroy against. The
    // defect is identical there — it is just invisible, and a test left on that
    // path would pass without measuring anything.
    _putenv_s("GE_VK_USE_DESCRIPTOR_BUFFER", "0");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
    setenv("GE_VK_VALIDATION_ASSERT", "0", 1);
    setenv("GE_VK_USE_DESCRIPTOR_BUFFER", "0", 1);
#endif
}

uint64_t VuidCount(const IDevice& device, const char* vuid)
{
    const ValidationStats stats = device.GetValidationStats();
    for (const ValidationVuidStat& v : stats.Vuids)
    {
        if (v.Vuid == vuid)
            return v.Count;
    }
    return 0;
}

std::string VuidFirstMessage(const IDevice& device, const char* vuid)
{
    const ValidationStats stats = device.GetValidationStats();
    for (const ValidationVuidStat& v : stats.Vuids)
    {
        if (v.Vuid == vuid)
            return v.FirstMessage + " objects:[" + v.FirstObjects + "]";
    }
    return {};
}

DescriptorSetLayoutDesc TwoStorageImageLayout()
{
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "DeferredViewDestroy.SetLayout";
    for (uint32_t i = 0; i < 2u; ++i)
    {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageImage;
        b.count = 1u;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }
    return layout;
}

class DeferredViewDestroyTimelineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        SetHeadlessValidationEnv();

        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        dd.enableDebugLayer = true; // the invariant under test is layer-reported

        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
        {
            m_Device.reset();
            GTEST_SKIP() << "No Vulkan device available";
        }

        // Verify the instrument before trusting any reading from it: a zero VUID
        // count proves nothing unless the layer is actually running. The
        // VkInstance is process-wide, so a device created without the layer
        // earlier in the process would silently disarm every assertion here.
        if (!m_Device->GetValidationStats().Enabled)
        {
            m_Device->Shutdown();
            m_Device.reset();
            GTEST_SKIP() << "Vulkan validation layer not active — cannot observe the VUID";
        }

        std::vector<uint8_t> cs = Tests::ReadSpirvBytes("hzb_build.comp.spv");
        if (cs.empty())
        {
            m_Device->Shutdown();
            m_Device.reset();
            GTEST_SKIP() << "hzb_build.comp.spv not available";
        }

        PipelineDesc pd{};
        pd.type = PipelineType::Compute;
        pd.computeShader = std::move(cs);
        pd.descriptorSetLayouts.push_back(TwoStorageImageLayout());
        pd.pushConstantSize = sizeof(HzbBuildPC);
        pd.pushConstantStagesMask = kShaderStageCompute;
        pd.debugName = "DeferredViewDestroy.Copy";
        m_Pipeline = m_Device->CreatePipeline(pd);
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_SrcTex = CreateStorageImage("DeferredViewDestroy.Src");
        m_DstTex = CreateStorageImage("DeferredViewDestroy.Dst");
        ASSERT_TRUE(m_SrcTex.IsValid());
        ASSERT_TRUE(m_DstTex.IsValid());

        m_SrcView = MakeMipView(m_SrcTex, "DeferredViewDestroy.SrcView");
        ASSERT_TRUE(m_SrcView.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_SrcView.IsValid())
            m_Device->DestroyTextureView(m_SrcView);
        if (m_SrcTex.IsValid())
            m_Device->DestroyTexture(m_SrcTex);
        if (m_DstTex.IsValid())
            m_Device->DestroyTexture(m_DstTex);
        m_Device->Shutdown();
    }

    TextureHandle CreateStorageImage(const char* debugName)
    {
        TextureDesc td{};
        td.width = kExtent;
        td.height = kExtent;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                        TextureUsage::ShaderResource);
        td.debugName = debugName;
        return m_Device->CreateTexture(td);
    }

    TextureViewHandle MakeMipView(TextureHandle tex, const char* debugName)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.aspect = TextureAspect::Color;
        vd.baseMip = 0u;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = debugName;
        return m_Device->CreateTextureView(tex, vd);
    }

    DescriptorSetHandle CreateFrameSet(TextureViewHandle dstView)
    {
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = TwoStorageImageLayout();
        dsDesc.transient = true;
        dsDesc.debugName = "DeferredViewDestroy.Set0";
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        if (ds.IsValid())
        {
            m_Device->UpdateStorageImageBinding(ds, 0u, m_SrcView);
            m_Device->UpdateStorageImageBinding(ds, 1u, dstView);
        }
        return ds;
    }

    // Records a compute-queue command list that copies through `ds` enough times
    // that the frame is still running long after it is submitted.
    std::unique_ptr<CommandList> RecordComputeFrame(DescriptorSetHandle ds)
    {
        // HZB render-graph passes execute on the compute queue. Keep the
        // regression on that queue so a graphics-only lifetime scheme cannot
        // make this test pass accidentally.
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        if (!cl)
            return cl;
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        const HzbBuildPC pc{kExtent, kExtent, kExtent, kExtent, 0u};
        cl->SetPushConstants(pc);
        for (uint32_t i = 0; i < kDispatchRepeats; ++i)
        {
            cl->Dispatch((kExtent + 7u) / 8u, (kExtent + 7u) / 8u, 1u);
            // Serialize the repeats so the frame's GPU cost is the sum, not the
            // overlap — this is what keeps the frame in flight past its submit.
            const ResourceBarrier rb = ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
            cl->Barrier(rb);
        }
        cl->End();
        return cl;
    }

    // One frame: create a single-mip view of the destination, bind it through a
    // transient descriptor set, record enough dispatches that the frame is still
    // running after it is submitted, and destroy the view either side of the
    // submit as `destroyBeforeSubmit` selects.
    void RunFrame(bool destroyBeforeSubmit)
    {
        ASSERT_TRUE(m_Device->BeginFrame());

        const TextureViewHandle dstView = MakeMipView(m_DstTex, "DeferredViewDestroy.DstView");
        ASSERT_TRUE(dstView.IsValid());

        const DescriptorSetHandle ds = CreateFrameSet(dstView);
        ASSERT_TRUE(ds.IsValid());

        auto cl = RecordComputeFrame(ds);
        ASSERT_NE(cl, nullptr);

        if (destroyBeforeSubmit)
            m_Device->DestroyTextureView(dstView);

        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);

        if (!destroyBeforeSubmit)
            m_Device->DestroyTextureView(dstView);

        m_Device->FinalizeFrame();
    }

    // Runs the frame sequence and returns how many times the layer reported a
    // still-referenced image view being destroyed.
    uint64_t MeasureViewInUseErrors(bool destroyBeforeSubmit)
    {
        const uint64_t before = VuidCount(*m_Device, kViewInUseVuid);
        for (uint32_t f = 0; f < kFrameCount; ++f)
            RunFrame(destroyBeforeSubmit);
        // A final BeginFrame runs the drain for the last recorded frame, so the
        // last frame's destroy is measured like every other one.
        m_Device->BeginFrame();
        m_Device->FinalizeFrame();
        const uint64_t after = VuidCount(*m_Device, kViewInUseVuid);
        return after >= before ? after - before : 0;
    }

    std::unique_ptr<IDevice> m_Device;
    PipelineHandle m_Pipeline{};
    TextureHandle m_SrcTex{};
    TextureHandle m_DstTex{};
    TextureViewHandle m_SrcView{};
};

// The repro. A view bound into a compute descriptor set must stay alive through
// its frame even when destruction is requested before the compute command list
// is submitted.
TEST_F(DeferredViewDestroyTimelineTest, ComputeViewDestroyedBeforeItsFrameIsSubmittedOutlivesThatFrame)
{
    const uint64_t errors = MeasureViewInUseErrors(/*destroyBeforeSubmit=*/true);
    EXPECT_EQ(errors, 0u) << "a view destroyed while its frame was still being recorded was "
                             "freed before that frame completed: "
                          << VuidFirstMessage(*m_Device, kViewInUseVuid);
}

// The control: identical work, destroy moved after the submit. Red here would
// mean the rig itself is wrong rather than the retire tag.
TEST_F(DeferredViewDestroyTimelineTest, ComputeViewDestroyedAfterItsFrameIsSubmittedIsClean)
{
    const uint64_t errors = MeasureViewInUseErrors(/*destroyBeforeSubmit=*/false);
    EXPECT_EQ(errors, 0u) << VuidFirstMessage(*m_Device, kViewInUseVuid);
}

// BeginFrame resets a frame slot's timestamp queries on the host, which is legal
// only once every submission made under that slot has completed. Compute-queue
// lists retire by timeline rather than through the slot's command-buffer list, so
// the slot's compute fence must still cover them, or the reset races the dispatches
// that wrote those queries.
TEST_F(DeferredViewDestroyTimelineTest, ReusedFrameSlotHasFinishedItsComputeWork)
{
    const TextureViewHandle dstView = MakeMipView(m_DstTex, "DeferredViewDestroy.DstView");
    ASSERT_TRUE(dstView.IsValid());
    const SemaphoreHandle computeDone = m_Device->CreateTimelineSemaphore(0);
    ASSERT_TRUE(computeDone.IsValid());
    const uint32_t framesInFlight = m_Device->GetFramesInFlight();

    for (uint32_t f = 0; f < framesInFlight + kFrameCount; ++f)
    {
        ASSERT_TRUE(m_Device->BeginFrame());
        if (f >= framesInFlight)
        {
            uint64_t completed = 0;
            ASSERT_TRUE(m_Device->GetTimelineSemaphoreValue(computeDone, completed));
            const uint64_t slotPreviousFrame = f - framesInFlight + 1u;
            EXPECT_GE(completed, slotPreviousFrame)
                << "frame " << f << " reused the slot of frame " << (f - framesInFlight)
                << " before that frame's compute work completed";
        }

        const DescriptorSetHandle ds = CreateFrameSet(dstView);
        ASSERT_TRUE(ds.IsValid());
        auto cl = RecordComputeFrame(ds);
        ASSERT_NE(cl, nullptr);
        ASSERT_TRUE(m_Device->QueueSubmit(IDevice::QueueType::Compute, {cl.get()}, {},
                                          {{computeDone, static_cast<uint64_t>(f) + 1u}}));
        m_Device->FinalizeFrame();
    }

    m_Device->WaitForIdle();
    m_Device->DestroySemaphore(computeDone);
    m_Device->DestroyTextureView(dstView);
}

} // namespace
