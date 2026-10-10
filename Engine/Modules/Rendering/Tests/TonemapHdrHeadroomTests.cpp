// HDR highlight-extension contract for tonemap.frag, measured on the real GPU path.
//
// THE REGRESSION THIS FILE EXISTS FOR
// -----------------------------------
// A tone mapper does two jobs: it compresses brightness AND it renders colour
// (desaturates highlights, holds hue, compresses out-of-gamut). Under HDR output
// the pass must extend the SELECTED operator's own display-referred result into
// the panel's headroom. It must never crossfade OUT of the operator toward a
// generic scene-derived roll, because that keeps the brightness compression and
// discards the colour rendering exactly where saturated sources live — and it
// makes every SDR-fit operator produce byte-identical pixels above the crossfade
// threshold, so the operator choice stops meaning anything.
//
// `OperatorsRemainDistinctAboveHeadroomThreshold` is that gate. It is the first
// test in the file because it is the reason the file exists.
//
// WHY A GPU TEST AND NOT A CPU MODEL
// ----------------------------------
// The claims are about what tonemap.frag computes, so the instrument is the
// compiled SPIR-V running through MaterialBuilder's pipeline, driven by the same
// push constants the render passes write. A CPU reimplementation would be a model
// of the shader and could agree with a broken shader. The render target is
// R32G32B32A32_FLOAT so values above paper white survive readback exactly; an
// 8-bit or FP16 target cannot represent what is being asserted.
//
// INSTRUMENT VERIFICATION (this suite is built to be able to fail)
// ---------------------------------------------------------------
//  * The push-constant writer is BY NAME off SPIR-V reflection and ASSERTs that
//    every name it needs was found. A renamed field fails loudly instead of
//    silently writing zeros into a field the shader then reads as "SDR".
//  * `HdrHeadroomIsActuallyAvailable` runs first and asserts the fixture's own
//    headroom premise, so a paper-white/max-nits plumbing change cannot make the
//    rest of the suite vacuously green.
//  * Every distinctness assertion carries a margin far above FP32 noise and is
//    stated pairwise with the operator names in the failure message.
//
// MUTATION TABLE — MEASURED, not predicted. Each mutation was applied to
// tonemap.frag, the .spv and the test exe deleted and confirmed absent, the
// shader recompiled, the .spv hash checked to differ from baseline (so a
// mutation that never reached the GPU could not masquerade as a weak test), and
// the suite re-run. Columns are the tests below, in file order:
//
//   0 HdrHeadroomIsActuallyAvailable        4 OperatorCeilingsMapOntoTheDisplay
//   1 OperatorsRemainDistinct...            5 DiffuseWhiteStaysAtOrBelowPaperWhite
//   2 SaturatedHighlightKeepsOperator...    6 HighlightsAboveKneeAreExtended
//   3 NeutralGainsHdrHeadroom               7 UnchangedPathsStayUnchanged
//
//   M1  restore the old scene-derived crossfade      RED: 1, 2, 4
//   M2  feed the extension `sceneLinear`             RED: 1, 2, 4, 5, 7
//   M3  route Neutral back to `return sdrTonemapped` RED: 1, 3, 4
//   M4  clamp the extended result to 1.0             RED: 1, 2, 3, 4
//   M5  knee 0.75 -> 0.70                            RED: 4, 5
//   M6  knee 0.75 -> 0.95                            RED: 1, 3, 4, 6
//   M7  drop the outEncoding guard (extend SDR too)  RED: 0, 3, 4, 6, 7
//
// Every test in the file goes RED under at least one mutation, including the
// instrument-premise test (M7 breaks it by extending Linear's SDR arm). M1 does
// NOT turn 3 red, because the historical defect was two separate faults — the
// crossfade for six modes AND Neutral's hard clamp — and M3 is the second one.
//
// Tests 9-11 (the ACES near-black span) were added later and measured the same
// way — .spv deleted and confirmed ABSENT, rebuilt, hash confirmed to differ from
// both the baseline and the other mutation, suite re-run:
//
//   M8  restore upstream's -0.000090537 numerator constant   RED: 9, 10
//   M9  reintroduce the rejected "gamut floor" (pin every
//       colour's min channel to 0.10 * its own luminance)    RED: 11
//
// M9 is deliberately kept in the table: it is the shape that was tried first and
// rejected, and test 11 exists to keep it out. Under M9 saturated green returns
// (0.025900, 0.350473, 0.039238) against the shipped (0.0, 0.367173, 0.0).
//
// The mutation table was measured before the ACES 2 peak-luminance tier ladder.
// ACES2 has since been excluded from tests 2 and 7(b) (recorded decision at
// those tests) and gained its own contract test (8, outside the table); the
// RED columns for the remaining modes are unaffected.
//
// Numbers in the failure messages are shader units where 1.0 == paper white.
// At the fixture's 203-nit paper white / 1000-nit panel, outputMax == 4.9261.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <Rendering/Common/Utils.h>
#include <Rendering/Core/CommandList.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Materials/MaterialBuilder.h>
#include <Rendering/Materials/ShaderReflection.h>

#include "Source/Passes/TonemapAces2Tables.h"

using namespace GameEngine::Rendering;

namespace
{

// tonemap.frag's `tonemapMode` push constant.
enum class Mode : int
{
    ACES = 0,
    Reinhard = 1,
    AgX = 2,
    Filmic = 3,
    Neutral = 4,
    Linear = 5,
    GranTurismo7 = 6,
    ACES2 = 7,
};

const char* ModeName(Mode m)
{
    switch (m)
    {
        case Mode::ACES: return "ACES";
        case Mode::Reinhard: return "Reinhard";
        case Mode::AgX: return "AgX";
        case Mode::Filmic: return "Filmic";
        case Mode::Neutral: return "Neutral";
        case Mode::Linear: return "Linear";
        case Mode::GranTurismo7: return "GranTurismo7";
        case Mode::ACES2: return "ACES2";
    }
    return "?";
}

// `outEncoding` push constant. 4 = HDR display-referred linear (headroom).
//
// The SDR arm is 1, NOT 0. Both take the same branch out of
// ApplyOutputRangeTonemap, but 0 is the hardware-sRGB swapchain path and falls
// through to this pass's Bayer dither, which biases a 1x1 probe by a fixed
// -0.492 LSB (measured: 0.001183 at ACES/scene-0.18, exactly
// 0.4921875 * lsbInLinear at Bayer8x8(0,0)). Comparing that against an
// undithered HDR result makes every "unchanged" assertion fail on a difference
// this suite is not about. Encoding 1 emits undithered display-referred linear
// and is what the main SDR chain actually uses (TonemapPass.h defaults to it).
constexpr int kEncodingHdrLinear = 4;
constexpr int kEncodingSdrLinear = 1;

// The fixture's display. 1.0 in shader units is paper white by construction
// (encode_srgb.frag multiplies by paperWhiteNits before PQ), so outputMax is
// 1000/203 == 4.9261 and "nits" below == value * 203.
constexpr float kPaperWhiteNits = 203.0f;
constexpr float kPanelPeakNits = 1000.0f;
constexpr float kOutputMax = kPanelPeakNits / kPaperWhiteNits;

struct Rgb
{
    float R = 0.0f, G = 0.0f, B = 0.0f;

    float Peak() const { return std::max(R, std::max(G, B)); }
};

std::string Fmt(const Rgb& c)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.6f, %.6f, %.6f)", c.R, c.G, c.B);
    return buf;
}

struct TonemapParams
{
    float Exposure = 1.0f;
    Mode TonemapMode = Mode::ACES;
    int DitherMode = 0;
    int OutEncoding = kEncodingHdrLinear;
    float PaperWhiteNits = kPaperWhiteNits;
    float MaxOutputNits = kPanelPeakNits;
    int PreserveAlpha = 0;
    int UseAutoExposure = 0;
};

// Writes the tonemap push-constant block BY NAME from the reflected layout.
// Returns false if any field the suite drives is missing, so a rename fails the
// test rather than leaving a zero the shader reads as a different mode.
bool BuildPushConstants(const PushConstantRangeMeta& pcr, const TonemapParams& p,
                        std::vector<uint8_t>& out, std::string& missing)
{
    out.assign(pcr.Size, 0);

    struct FloatField { const char* Name; float Value; bool Seen; };
    struct IntField { const char* Name; int32_t Value; bool Seen; };

    FloatField floats[] = {
        {"exposure", p.Exposure, false},
        {"paperWhiteNits", p.PaperWhiteNits, false},
        {"maxOutputNits", p.MaxOutputNits, false},
    };
    IntField ints[] = {
        {"tonemapMode", static_cast<int32_t>(p.TonemapMode), false},
        {"ditherMode", p.DitherMode, false},
        {"outEncoding", p.OutEncoding, false},
        {"preserveAlpha", p.PreserveAlpha, false},
        {"useAutoExposure", p.UseAutoExposure, false},
    };

    for (const auto& m : pcr.Block.Members)
    {
        for (auto& f : floats)
            if (m.Name == f.Name)
            {
                if (m.Offset + sizeof(float) > out.size())
                    return false;
                std::memcpy(out.data() + m.Offset, &f.Value, sizeof(float));
                f.Seen = true;
            }
        for (auto& i : ints)
            if (m.Name == i.Name)
            {
                if (m.Offset + sizeof(int32_t) > out.size())
                    return false;
                std::memcpy(out.data() + m.Offset, &i.Value, sizeof(int32_t));
                i.Seen = true;
            }
    }

    missing.clear();
    for (const auto& f : floats)
        if (!f.Seen)
            missing += std::string(f.Name) + " ";
    for (const auto& i : ints)
        if (!i.Seen)
            missing += std::string(i.Name) + " ";
    return missing.empty();
}

int OrdinalOf(const ShaderMeta& meta, uint32_t setIndex)
{
    std::vector<uint32_t> ord;
    ord.reserve(meta.Sets.size());
    for (const auto& s : meta.Sets)
        ord.push_back(s.Set);
    std::sort(ord.begin(), ord.end());
    ord.erase(std::unique(ord.begin(), ord.end()), ord.end());
    for (size_t i = 0; i < ord.size(); ++i)
        if (ord[i] == setIndex)
            return static_cast<int>(i);
    return -1;
}

// One device + one pipeline for the whole suite; every case is a 1x1 draw with a
// different scene colour and push-constant block.
class TonemapRig
{
public:
    // `fatal` distinguishes "this machine has no GPU" (a legitimate skip) from
    // "the thing under test is missing or broken" (never a skip — a filter or a
    // missing shader that exits PASSED is exactly how this suite would lie).
    bool Init(std::string& why, bool& fatal)
    {
        fatal = true;
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif

#ifdef RENDERING_SHADER_OUTPUT_DIR
        Utils::SetShaderPathResolver(
            +[](const std::filesystem::path& rel) -> std::filesystem::path
            {
                const std::filesystem::path outDir(RENDERING_SHADER_OUTPUT_DIR);
                std::error_code ec;
                std::filesystem::path direct = outDir / rel;
                if (std::filesystem::exists(direct, ec))
                    return direct;
                return outDir.parent_path() / rel;
            });
#endif

        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
        {
            why = "Vulkan device init failed";
            fatal = false; // no GPU on this machine
            return false;
        }

        std::vector<uint8_t> vs, fs;
        try
        {
            vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
            fs = Utils::LoadShaderFile("tonemap.frag.spv");
        }
        catch (const std::exception& e)
        {
            why = std::string("shader load threw: ") + e.what();
            return false;
        }
        if (vs.empty() || fs.empty())
        {
            why = "fullscreen_noinput.vert.spv / tonemap.frag.spv not found — shaders not compiled";
            return false;
        }

        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions ro{};
        if (!ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()),
                          vs.size() / 4, ro, rvs, nullptr) ||
            !ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()),
                          fs.size() / 4, ro, rfs, nullptr))
        {
            why = "SPIR-V reflection failed";
            return false;
        }
        m_Meta = MergeStages({rvs, rfs});

        if (m_Meta.PushConstants.empty() || m_Meta.PushConstants[0].Block.Members.empty())
        {
            why = "tonemap.frag exposes no reflected push-constant block";
            return false;
        }
        m_Pcr = m_Meta.PushConstants[0];

        PipelineDesc pd{};
        pd.type = PipelineType::Graphics;
        pd.vertexShader = vs;
        pd.pixelShader = fs;
        pd.debugName = "TonemapHdrHeadroom";
        MaterialBuilder::FormatsHint fh{};
        fh.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string err;
        if (!MaterialBuilder::BuildPipelineDescFromMeta(m_Meta, pd, fh, MaterialBuilder::MergeMode::Auto,
                                                        MaterialBuilder::PushConstantPolicy{}, &err))
        {
            why = "BuildPipelineDescFromMeta: " + err;
            return false;
        }
        m_Pipeline = m_Device->CreatePipeline(pd);
        if (m_Pipeline == INVALID_HANDLE)
        {
            why = "CreatePipeline failed (R32G32B32A32_FLOAT colour target unsupported?)";
            return false;
        }

        TextureDesc srcD{};
        srcD.width = srcD.height = srcD.depth = 1;
        srcD.mipLevels = srcD.arrayLayers = srcD.sampleCount = 1;
        srcD.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        srcD.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        srcD.debugName = "TonemapHdrHeadroom.Scene";
        m_Src = m_Device->CreateTexture(srcD);

        TextureDesc dstD{};
        dstD.width = dstD.height = dstD.depth = 1;
        dstD.mipLevels = dstD.arrayLayers = dstD.sampleCount = 1;
        dstD.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        dstD.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        dstD.debugName = "TonemapHdrHeadroom.Out";
        m_Dst = m_Device->CreateTexture(dstD);

        m_Upload = m_Device->CreateUploadBuffer(16, "TonemapHdrHeadroom.Upload");
        m_Readback = m_Device->CreateReadbackBuffer(16, "TonemapHdrHeadroom.Readback");
        if (!m_Src.IsValid() || !m_Dst.IsValid() || !m_Upload.IsValid() || !m_Readback.IsValid())
        {
            why = "resource creation failed";
            return false;
        }

        SamplerDesc sd{};
        m_Sampler = m_Device->CreateSampler(sd);
        if (!m_Sampler.IsValid())
        {
            why = "CreateSampler failed";
            return false;
        }

        if (!BindDescriptors(pd, why))
            return false;

        return true;
    }

    // Runs tonemap.frag once on a 1x1 scene-linear colour and returns its RGB output.
    Rgb Run(const Rgb& scene, const TonemapParams& params)
    {
        const float texel[4] = {scene.R, scene.G, scene.B, 1.0f};
        m_Device->UpdateBuffer(m_Upload, 0, sizeof(texel), texel);

        std::vector<uint8_t> pc;
        std::string missing;
        if (!BuildPushConstants(m_Pcr, params, pc, missing))
        {
            ADD_FAILURE() << "push-constant block missing reflected fields: " << missing;
            return {};
        }

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Src, m_SrcState, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(m_Upload, m_Src, 0, 0, 1, 1, 0, sizeof(texel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Src, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        m_SrcState = ResourceState::ShaderResource;

        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Dst, m_DstState, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = m_Dst;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0, 0, 1, 1);
        cl->SetScissor(0, 0, 1, 1);
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(m_TexSet, m_DsTex, m_Pipeline);
        if (m_SamplerSet != m_TexSet && m_DsSampler.IsValid())
            cl->BindDescriptorSet(m_SamplerSet, m_DsSampler, m_Pipeline);
        cl->SetPushConstantsById(m_PcRangeId, pc.data(), pc.size(), 0);
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Dst, ResourceState::RenderTarget,
                                                          ResourceState::CopySource));
        m_DstState = ResourceState::CopySource;
        cl->CopyTextureToBuffer(m_Dst, m_Readback, 1, 1, 0, 0, 0, 0);
        cl->End();

        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        Rgb out{};
        if (const void* p = m_Device->MapBuffer(m_Readback))
        {
            float v[4] = {0, 0, 0, 0};
            std::memcpy(v, p, sizeof(v));
            out = Rgb{v[0], v[1], v[2]};
            m_Device->UnmapBuffer(m_Readback);
        }
        else
        {
            ADD_FAILURE() << "readback map failed";
        }
        return out;
    }

    IDevice* Device() const { return m_Device.get(); }

private:
    bool BindDescriptors(const PipelineDesc& pd, std::string& why)
    {
        bool hasCombined = false, hasImage = false, hasSampler = false;
        uint32_t combinedSet = 0, combinedBinding = 0, imageSet = 0, imageBinding = 0;
        uint32_t samplerSet = 0, samplerBinding = 0, ssboSet = 0, ssboBinding = 0;
        bool hasSsbo = false;
        bool hasTables = false;
        uint32_t tablesSet = 0, tablesBinding = 0;
        for (const auto& s : m_Meta.Sets)
            for (const auto& b : s.Bindings)
            {
                if (!hasCombined && b.Type == ShaderMetaBindingType::kCombinedImageSampler)
                { hasCombined = true; combinedSet = s.Set; combinedBinding = b.Binding; }
                if (!hasImage && b.Type == ShaderMetaBindingType::kSampledImage)
                { hasImage = true; imageSet = s.Set; imageBinding = b.Binding; }
                if (!hasSampler && b.Type == ShaderMetaBindingType::kSampler)
                { hasSampler = true; samplerSet = s.Set; samplerBinding = b.Binding; }
                if (b.Type == ShaderMetaBindingType::kStorageBuffer && b.Name == "Aces2Tables")
                { hasTables = true; tablesSet = s.Set; tablesBinding = b.Binding; }
                else if (!hasSsbo && b.Type == ShaderMetaBindingType::kStorageBuffer)
                { hasSsbo = true; ssboSet = s.Set; ssboBinding = b.Binding; }
            }

        auto makeSet = [&](uint32_t set, const char* name) -> DescriptorSetHandle
        {
            const int ord = OrdinalOf(m_Meta, set);
            if (ord < 0 || static_cast<size_t>(ord) >= pd.descriptorSetLayouts.size())
                return {};
            DescriptorSetDesc d{};
            d.layout = pd.descriptorSetLayouts[static_cast<size_t>(ord)];
            d.transient = false;
            d.debugName = name;
            return m_Device->CreateDescriptorSet(d);
        };

        if (hasCombined)
        {
            m_TexSet = combinedSet;
            m_SamplerSet = combinedSet;
            m_DsTex = makeSet(combinedSet, "TonemapHdrHeadroom.Combined");
            if (!m_DsTex.IsValid()) { why = "combined descriptor set failed"; return false; }
            m_Device->UpdateCombinedImageSamplerBinding(m_DsTex, combinedBinding, m_Src, m_Sampler);
        }
        else if (hasImage && hasSampler)
        {
            m_TexSet = imageSet;
            m_SamplerSet = samplerSet;
            m_DsTex = makeSet(imageSet, "TonemapHdrHeadroom.Image");
            if (!m_DsTex.IsValid()) { why = "image descriptor set failed"; return false; }
            m_Device->UpdateImageBinding(m_DsTex, imageBinding, m_Src);
            if (samplerSet == imageSet)
                m_DsSampler = m_DsTex;
            else
            {
                m_DsSampler = makeSet(samplerSet, "TonemapHdrHeadroom.Sampler");
                if (!m_DsSampler.IsValid()) { why = "sampler descriptor set failed"; return false; }
            }
            m_Device->UpdateSamplerBinding(m_DsSampler, samplerBinding, m_Sampler);
        }
        else
        {
            why = "tonemap.frag exposes neither a combined nor a separate image+sampler binding";
            return false;
        }

        // tonemap.frag statically references the auto-exposure SSBO, so it must be bound
        // even with useAutoExposure == 0. exposureScale is left at 1.0 and unused.
        if (hasSsbo)
        {
            DescriptorSetHandle target =
                (ssboSet == m_TexSet) ? m_DsTex : ((ssboSet == m_SamplerSet) ? m_DsSampler : DescriptorSetHandle{});
            if (!target.IsValid())
            {
                why = "uExposure lives in a set the rig does not bind";
                return false;
            }
            BufferDesc sbd{};
            sbd.size = 16;
            sbd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            sbd.memoryUsage = BufferMemoryUsage::Upload;
            sbd.flags = BufferCreateFlags::PersistentlyMapped;
            m_Exposure = m_Device->CreateBuffer(sbd);
            if (!m_Exposure.IsValid()) { why = "exposure SSBO failed"; return false; }
            if (void* p = m_Device->MapBuffer(m_Exposure))
            {
                const float scale = 1.0f;
                const uint32_t valid = 1u;
                std::memcpy(p, &scale, 4);
                std::memcpy(static_cast<uint8_t*>(p) + 4, &valid, 4);
                m_Device->UnmapBuffer(m_Exposure);
            }
            m_Device->UpdateStorageBufferBinding(target, ssboBinding, m_Exposure, 0, 16);
        }

        // tonemap.frag's ACES 2 tier tables (Aces2Tables SSBO): upload the
        // generated payload the engine's TonemapPass uploads in production —
        // an unbound/zeroed block makes ACES2 render black in the fixture.
        if (hasTables)
        {
            DescriptorSetHandle target =
                (tablesSet == m_TexSet) ? m_DsTex : ((tablesSet == m_SamplerSet) ? m_DsSampler : DescriptorSetHandle{});
            if (!target.IsValid())
            {
                why = "Aces2Tables lives in a set the rig does not bind";
                return false;
            }
            BufferDesc tbd{};
            tbd.size = Passes::kAces2TablesBytes;
            tbd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            tbd.memoryUsage = BufferMemoryUsage::Upload;
            tbd.flags = BufferCreateFlags::PersistentlyMapped;
            m_Aces2Tables = m_Device->CreateBuffer(tbd);
            if (!m_Aces2Tables.IsValid()) { why = "Aces2Tables SSBO failed"; return false; }
            if (void* p = m_Device->MapBuffer(m_Aces2Tables))
            {
                std::memcpy(p, Passes::kAces2Tables, Passes::kAces2TablesBytes);
                m_Device->UnmapBuffer(m_Aces2Tables);
            }
            m_Device->UpdateStorageBufferBinding(target, tablesBinding, m_Aces2Tables, 0,
                                                 Passes::kAces2TablesBytes);
        }

        m_PcRangeId = 0;
        const uint32_t rangeCount = m_Device->GetPipelinePushConstantRangeCount(m_Pipeline);
        for (uint32_t rid = 0; rid < rangeCount; ++rid)
        {
            PushConstantRangeInfo info{};
            if (m_Device->GetPipelinePushConstantRangeInfo(m_Pipeline, rid, info) && info.size >= m_Pcr.Size)
            {
                m_PcRangeId = rid;
                break;
            }
        }
        return true;
    }

    std::unique_ptr<IDevice> m_Device;
    ShaderMeta m_Meta{};
    PushConstantRangeMeta m_Pcr{};
    PipelineHandle m_Pipeline = INVALID_HANDLE;
    TextureHandle m_Src{}, m_Dst{};
    BufferHandle m_Upload{}, m_Readback{}, m_Exposure{}, m_Aces2Tables{};
    SamplerHandle m_Sampler{};
    DescriptorSetHandle m_DsTex{}, m_DsSampler{};
    uint32_t m_TexSet = 0, m_SamplerSet = 0, m_PcRangeId = 0;
    ResourceState m_SrcState = ResourceState::Undefined;
    ResourceState m_DstState = ResourceState::Undefined;
};

TonemapRig& Rig()
{
    static TonemapRig rig;
    return rig;
}

bool& RigReady()
{
    static bool ready = false;
    return ready;
}

std::string& RigWhy()
{
    static std::string why;
    return why;
}

bool& RigFatal()
{
    static bool fatal = true;
    return fatal;
}

class TonemapHdrHeadroom : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        static bool attempted = false;
        if (!attempted)
        {
            attempted = true;
            RigReady() = Rig().Init(RigWhy(), RigFatal());
        }
    }

    void SetUp() override
    {
        if (RigReady())
            return;
        // Only "no GPU here" skips. Anything else — missing SPIR-V, reflection
        // failure, unsupported target format — FAILS, because a suite that
        // reports PASSED without running is the failure mode this file is
        // written against.
        if (RigFatal())
            FAIL() << "tonemap rig could not be built: " << RigWhy();
        GTEST_SKIP() << "no usable Vulkan device on this machine: " << RigWhy();
    }

    static Rgb Hdr(const Rgb& scene, Mode mode)
    {
        TonemapParams p{};
        p.TonemapMode = mode;
        p.OutEncoding = kEncodingHdrLinear;
        return Rig().Run(scene, p);
    }

    static Rgb Sdr(const Rgb& scene, Mode mode)
    {
        TonemapParams p{};
        p.TonemapMode = mode;
        p.OutEncoding = kEncodingSdrLinear;
        return Rig().Run(scene, p);
    }
};

// The six operators that the removed crossfade collapsed onto one curve. Linear
// (5) is excluded by design: it performs no colour rendering, so it keeps the
// scene-derived shoulder. Neutral (4) is here too — it was the opposite failure
// (hard clamp at paper white) and now extends like the rest.
constexpr Mode kColourRenderingModes[] = {Mode::ACES,    Mode::Reinhard,     Mode::AgX,
                                          Mode::Filmic,  Mode::Neutral,      Mode::GranTurismo7,
                                          Mode::ACES2};

// --- 0. Instrument premise -------------------------------------------------
//
// Everything below is a claim about behaviour ABOVE paper white. If the fixture
// has no headroom, every one of those claims is vacuous. Assert the premise
// before trusting any reading taken with it.
TEST_F(TonemapHdrHeadroom, HdrHeadroomIsActuallyAvailable)
{
    ASSERT_GT(kOutputMax, 2.0f) << "fixture headroom too small to distinguish the shaders";

    // Linear under HDR is the pass's own report of the headroom: it rolls the raw
    // scene value to at most outputMax and is unaffected by this change. Its
    // shoulder only APPROACHES outputMax, so the probe has to be far above it.
    const Rgb veryBright = Hdr({4096.0f, 4096.0f, 4096.0f}, Mode::Linear);
    EXPECT_NEAR(veryBright.Peak(), kOutputMax, 0.05f)
        << "Linear/HDR should saturate at outputMax " << kOutputMax << ", got " << Fmt(veryBright)
        << " — the push constants are not reaching the shader, or paper-white plumbing changed";

    // And the SDR path must NOT have headroom, or the HDR assertions prove nothing.
    const Rgb sdr = Sdr({64.0f, 64.0f, 64.0f}, Mode::ACES);
    EXPECT_LE(sdr.Peak(), 1.0001f) << "SDR ACES exceeded paper white: " << Fmt(sdr);
}

// --- 1. THE REGRESSION GATE ------------------------------------------------
//
// Above the old crossfade's upper edge (peak >= min(outputMax, 4)) the result was
// 100% a scene-derived shoulder and 0% the selected operator, so every operator
// returned the same pixel. Pre-registered failure against the unmodified shader:
// all six of ACES/Reinhard/AgX/Filmic/GT7/ACES2 return (3.515329, 3.515329,
// 3.515329) at scene 8.0.
TEST_F(TonemapHdrHeadroom, OperatorsRemainDistinctAboveHeadroomThreshold)
{
    const Rgb scene{8.0f, 8.0f, 8.0f};

    std::vector<std::pair<Mode, Rgb>> results;
    for (Mode m : kColourRenderingModes)
        results.emplace_back(m, Hdr(scene, m));

    for (const auto& [m, c] : results)
        EXPECT_GT(c.Peak(), 1.0f) << ModeName(m) << " produced no HDR highlight at scene 8.0: " << Fmt(c);

    // Pairwise distinctness. The collapse this gates makes the separation exactly
    // zero, so the margin only has to clear readback noise — the target is
    // R32G32B32A32_FLOAT, so that is ~1e-7. 0.002 (0.4 nits) is three orders above
    // it and still below the smallest true separation at this probe, which is
    // Reinhard (1.041) vs ACES 2 (1.027).
    constexpr float kMinSeparation = 0.002f;
    int collapsed = 0;
    for (size_t i = 0; i < results.size(); ++i)
        for (size_t j = i + 1; j < results.size(); ++j)
        {
            const float d = std::fabs(results[i].second.Peak() - results[j].second.Peak());
            if (d < kMinSeparation)
            {
                ++collapsed;
                ADD_FAILURE() << "operators collapsed onto one curve above the headroom threshold: "
                              << ModeName(results[i].first) << " " << Fmt(results[i].second) << " vs "
                              << ModeName(results[j].first) << " " << Fmt(results[j].second)
                              << " (separation " << d << " < " << kMinSeparation << ")";
            }
        }
    EXPECT_EQ(collapsed, 0) << "the HDR path is discarding the selected operator again";

    // Spread, stated separately from the pairwise check: a partial regression that
    // collapsed only some modes would still leave a spread, but a full one leaves none.
    float lo = results.front().second.Peak(), hi = lo;
    for (const auto& [m, c] : results)
    {
        lo = std::min(lo, c.Peak());
        hi = std::max(hi, c.Peak());
    }
    EXPECT_GT(hi - lo, 1.0f) << "the operators span only " << (hi - lo)
                             << " at scene 8.0 — they are rendering one curve, not eight";
}

// --- 2. The colour rendering survives --------------------------------------
//
// The extension is a positive scalar on the operator's OWN output, so the HDR
// pixel's chromaticity must equal the operator's SDR chromaticity — the hue and
// highlight desaturation the operator computed. The old blend replaced it with
// the raw scene chromaticity instead. Probe: a saturated blue highlight, where
// the two differ most.
TEST_F(TonemapHdrHeadroom, SaturatedHighlightKeepsOperatorColourRendering)
{
    const Rgb scene{0.8f, 1.6f, 8.0f}; // saturated blue, bright enough to clear the old threshold

    auto normalize = [](const Rgb& c) -> Rgb
    {
        const float s = c.R + c.G + c.B;
        return (s > 1e-6f) ? Rgb{c.R / s, c.G / s, c.B / s} : Rgb{};
    };
    const Rgb sceneChroma = normalize(scene);

    // ACES2 is deliberately absent — RECORDED DECISION (2026-08-04): mode 7 is
    // peak-parameterised and bypasses the extension. Its HDR arm runs the
    // Academy output transform matched to the panel's headroom, which performs
    // its own colour rendering at that peak; the SDR arm's chromaticity is a
    // different peak's answer, not this contract. Its HDR behaviour is pinned
    // in Aces2PeakParameterisedContract instead.
    for (Mode m : {Mode::ACES, Mode::AgX, Mode::Neutral})
    {
        const Rgb sdr = Sdr(scene, m);
        const Rgb hdr = Hdr(scene, m);
        const Rgb sdrChroma = normalize(sdr);
        const Rgb hdrChroma = normalize(hdr);

        const float dOperator = std::fabs(hdrChroma.R - sdrChroma.R) +
                                std::fabs(hdrChroma.G - sdrChroma.G) +
                                std::fabs(hdrChroma.B - sdrChroma.B);
        const float dScene = std::fabs(hdrChroma.R - sceneChroma.R) +
                             std::fabs(hdrChroma.G - sceneChroma.G) +
                             std::fabs(hdrChroma.B - sceneChroma.B);

        EXPECT_LT(dOperator, 0.02f)
            << ModeName(m) << ": HDR chromaticity " << Fmt(hdrChroma) << " drifted from the operator's own "
            << Fmt(sdrChroma) << " — the colour rendering was replaced, not extended";
        EXPECT_GT(dScene, dOperator)
            << ModeName(m) << ": HDR chromaticity " << Fmt(hdrChroma) << " is closer to the RAW SCENE "
            << Fmt(sceneChroma) << " than to the operator's " << Fmt(sdrChroma)
            << " — the operator's gamut/desaturation work was discarded";
    }
}

// --- 3. Neutral is no longer the opposite failure --------------------------
//
// Mode 4 took `default: return sdrTonemapped` and hard-clamped at paper white
// (~203 nits) on a 1000-nit panel. Pre-registered failure against the unmodified
// shader: peak == 0.869 at scene 8.0, i.e. below 1.0.
TEST_F(TonemapHdrHeadroom, NeutralGainsHdrHeadroom)
{
    const Rgb hdr = Hdr({8.0f, 8.0f, 8.0f}, Mode::Neutral);
    EXPECT_GT(hdr.Peak(), 1.25f)
        << "Neutral under HDR is still clamped at paper white: " << Fmt(hdr)
        << " (" << hdr.Peak() * kPaperWhiteNits << " nits on a " << kPanelPeakNits << "-nit panel)";

    const Rgb sdr = Sdr({8.0f, 8.0f, 8.0f}, Mode::Neutral);
    EXPECT_LE(sdr.Peak(), 1.0001f) << "Neutral must still clamp on SDR output: " << Fmt(sdr);
}

// --- 4. Each operator's own ceiling maps onto the display -------------------
//
// The extension maps operator output 1.0 exactly onto outputMax, so an operator
// that CLAMPS at 1.0 reaches the panel's peak on a saturating source — the "real
// HDR highlights" the removed blend existed to produce. An operator that only
// approaches 1.0 lands short, which is its own curve's answer and the correct one.
//
// Which operators are in which group is MEASURED here, not hardcoded: the SDR arm
// at the same scene value IS the operator's ceiling, and the HDR arm must be that
// ceiling put through the extension. Measured ceilings at scene 4096:
//
//   ACES     1.000000 -> 4.9261 (1000 nits) — clamps
//   ACES 2   1.000000 -> 4.9261 (1000 nits) — clamps (min(vec3(1.)))
//   GT7      1.000000 -> 4.9261 (1000 nits) — clamps
//   AgX      0.997119 -> 4.2443 ( 862 nits) — does NOT reach 1.0; its 6th-order
//            polynomial sigmoid tops out ~0.14% short, and near the top the
//            extension's slope is ((outputMax-knee)/(1-knee))^2 ~ 279, so that
//            0.3% shortfall costs 14% of peak luminance. Inherent to the
//            published fit, not an engine defect — but it is why AgX's brightest
//            possible pixel is dimmer than ACES's, and it belongs on the record.
//   Reinhard/Filmic/Neutral — asymptotic by construction, land short by design.
TEST_F(TonemapHdrHeadroom, OperatorCeilingsMapOntoTheDisplay)
{
    // The contract the shader implements, stated independently here. This is not
    // a reimplementation of the operator — the operator's output is measured from
    // the shader — only of the scalar the extension is defined to apply to it.
    auto extensionRef = [](float ceiling, float outputMax)
    {
        constexpr float kKnee = 0.75f;
        constexpr float kOperatorHeadroom = 1.0f - kKnee;
        if (ceiling <= kKnee)
            return ceiling;
        const float excess = std::min(ceiling - kKnee, kOperatorHeadroom);
        const float c = 1.0f / kOperatorHeadroom - 1.0f / (outputMax - kKnee);
        return kKnee + excess / (1.0f - c * excess);
    };

    const Rgb scene{4096.0f, 4096.0f, 4096.0f};
    int reachedPanelPeak = 0;
    for (Mode m : kColourRenderingModes)
    {
        const float ceiling = Sdr(scene, m).Peak();
        const Rgb hdr = Hdr(scene, m);

        ASSERT_GT(ceiling, 0.75f) << ModeName(m) << ": saturating source did not clear the knee ("
                                  << ceiling << ") — this probe proves nothing";
        EXPECT_LE(hdr.Peak(), kOutputMax + 1e-3f)
            << ModeName(m) << " exceeded the display's max linear value: " << Fmt(hdr);
        EXPECT_NEAR(hdr.Peak(), extensionRef(ceiling, kOutputMax), 3e-3f)
            << ModeName(m) << ": ceiling " << ceiling << " landed at " << hdr.Peak() << " ("
            << hdr.Peak() * kPaperWhiteNits << " nits), not where the extension puts it";

        if (ceiling >= 0.9999f)
        {
            ++reachedPanelPeak;
            EXPECT_NEAR(hdr.Peak(), kOutputMax, 5e-3f)
                << ModeName(m) << " clamps at 1.0 but did not land on panel peak: " << Fmt(hdr)
                << " (" << hdr.Peak() * kPaperWhiteNits << " nits, expected " << kPanelPeakNits << ")";
        }
    }
    // Without this the clause above is vacuous if every operator stopped clamping.
    EXPECT_GE(reachedPanelPeak, 2) << "no operator reached the panel's peak luminance — the whole "
                                   << "point of the extension is that the clamping ones do";
}

// --- 5. Knee pinned from below ---------------------------------------------
//
// The knee is the operator-output level where the extension starts. Set it too
// low and diffuse white — the operator's own rendering of scene 1.0 — is pushed
// above BT.2408 reference white, so the whole image glares. Neutral is the binding
// operator: it renders scene 1.0 at 0.869, the highest of the seven extended operators (Linear maps 1.0 to 1.0). At knee 0.75
// that lands at 196 nits; at knee 0.70 it lands at 214 nits and this test goes RED.
TEST_F(TonemapHdrHeadroom, DiffuseWhiteStaysAtOrBelowPaperWhite)
{
    for (Mode m : kColourRenderingModes)
    {
        const Rgb hdr = Hdr({1.0f, 1.0f, 1.0f}, m);
        EXPECT_LE(hdr.Peak(), 1.0f + 1e-3f)
            << ModeName(m) << " pushed diffuse white above paper white: " << Fmt(hdr) << " ("
            << hdr.Peak() * kPaperWhiteNits << " nits) — the extension knee is too low";
    }
}

// --- 6. Knee pinned from above ---------------------------------------------
//
// Set the knee too high and the extension only fires on pixels that already
// saturate the operator, so ordinary specular highlights get no headroom at all
// and the HDR display shows an SDR image. ACES renders scene 4.0 at 0.909, which
// is above a 0.75 knee and below a 0.95 one.
TEST_F(TonemapHdrHeadroom, HighlightsAboveKneeAreExtended)
{
    for (Mode m : {Mode::ACES, Mode::AgX, Mode::Filmic})
    {
        const Rgb sdr = Sdr({4.0f, 4.0f, 4.0f}, m);
        const Rgb hdr = Hdr({4.0f, 4.0f, 4.0f}, m);
        // Probe validity: this scene must land the operator strictly BETWEEN the
        // knee under test (0.75) and the mutation's knee (0.95), or the test cannot
        // tell the two apart and its green result would mean nothing.
        ASSERT_GT(sdr.Peak(), 0.75f) << ModeName(m) << ": probe is below the knee, "
                                     << Fmt(sdr) << " — it cannot discriminate";
        ASSERT_LT(sdr.Peak(), 0.95f) << ModeName(m) << ": probe is above a 0.95 knee, "
                                     << Fmt(sdr) << " — it cannot discriminate";
        EXPECT_GT(hdr.Peak(), sdr.Peak() * 1.05f)
            << ModeName(m) << ": a scene-4.0 highlight got no headroom — SDR " << Fmt(sdr) << " vs HDR "
            << Fmt(hdr) << " (" << hdr.Peak() * kPaperWhiteNits << " nits); the extension knee is too high";
        EXPECT_LE(hdr.Peak(), kOutputMax + 1e-3f)
            << ModeName(m) << " exceeded the display's max linear value: " << Fmt(hdr);
    }
}

// --- 7. Paths that must not move -------------------------------------------
//
// SDR output is byte-identical to the operator (no extension, no clamp change),
// Linear keeps its scene-derived shoulder, and an HDR surface with no headroom
// (max nits == paper white) collapses to the SDR result.
TEST_F(TonemapHdrHeadroom, UnchangedPathsStayUnchanged)
{
    // (a) SDR: below the operator ceiling nothing may move, and nothing may exceed
    //     paper white regardless of how bright the source is.
    for (Mode m : kColourRenderingModes)
    {
        const Rgb dim = Sdr({0.18f, 0.18f, 0.18f}, m);
        EXPECT_GT(dim.Peak(), 0.0f) << ModeName(m) << ": SDR mid-grey was black";
        EXPECT_LT(dim.Peak(), 0.75f)
            << ModeName(m) << ": SDR mid-grey " << Fmt(dim) << " is at or above the extension knee — "
            << "this probe can no longer tell the SDR path apart from an extended one";
        const Rgb bright = Sdr({64.0f, 64.0f, 64.0f}, m);
        EXPECT_LE(bright.Peak(), 1.0001f)
            << ModeName(m) << ": the SDR path gained headroom: " << Fmt(bright);
    }

    // (b) Midtones below the knee are identical on both paths for the EXTENDED
    //     operators — the extension is strictly a highlight operation. ACES2 is
    //     deliberately absent — RECORDED DECISION (2026-08-04): peak-matched
    //     ACES 2 output transforms lift mid-grey by Academy design, and the
    //     paper-white-relative ladder
    //     gives this fixture's 203-nit-paper-white/1000-nit panel the ~493-nit
    //     curve, so its midtones legitimately move under HDR. The lift is
    //     bounded by Aces2PeakParameterisedContract.
    for (Mode m : kColourRenderingModes)
    {
        if (m == Mode::ACES2)
            continue;
        const Rgb sdr = Sdr({0.18f, 0.18f, 0.18f}, m);
        const Rgb hdr = Hdr({0.18f, 0.18f, 0.18f}, m);
        EXPECT_NEAR(hdr.R, sdr.R, 1e-5f) << ModeName(m) << ": midtone moved under HDR, " << Fmt(sdr)
                                         << " -> " << Fmt(hdr);
        EXPECT_NEAR(hdr.G, sdr.G, 1e-5f) << ModeName(m);
        EXPECT_NEAR(hdr.B, sdr.B, 1e-5f) << ModeName(m);
    }

    // (c) Linear keeps the scene-derived shoulder: unlike the colour-rendering
    //     operators it never compressed anything, so there is nothing to preserve.
    //     Its scene-1.0 output stays at 1.0 (the shoulder's start), which no
    //     extended operator does.
    const Rgb linear1 = Hdr({1.0f, 1.0f, 1.0f}, Mode::Linear);
    EXPECT_NEAR(linear1.Peak(), 1.0f, 1e-3f) << "Linear/HDR at scene 1.0: " << Fmt(linear1);
    const Rgb linear4 = Hdr({4.0f, 4.0f, 4.0f}, Mode::Linear);
    EXPECT_GT(linear4.Peak(), 2.0f) << "Linear/HDR lost its shoulder: " << Fmt(linear4);

    // (d) An HDR surface with no headroom must equal the SDR result. ACES2 is
    //     included: outputMax == 1.0 selects tier 0 at t = 0 exactly.
    for (Mode m : {Mode::ACES, Mode::Neutral, Mode::AgX, Mode::ACES2})
    {
        TonemapParams p{};
        p.TonemapMode = m;
        p.OutEncoding = kEncodingHdrLinear;
        p.MaxOutputNits = kPaperWhiteNits; // outputMax == 1.0
        const Rgb flat = Rig().Run({8.0f, 8.0f, 8.0f}, p);
        EXPECT_LE(flat.Peak(), 1.0001f)
            << ModeName(m) << ": zero-headroom HDR surface produced " << Fmt(flat);
    }
}

// --- 8. ACES 2's peak-parameterised contract --------------------------------
//
// Mode 7 bypasses the highlight extension: its tier ladder produces
// display-referred output for the panel's headroom directly, and the routing
// only clamps it. RECORDED DECISION (2026-08-04): mid-grey moves with peak by
// Academy design — the paper-white-relative ladder gives this fixture the
// ~493-nit curve — so
// the HDR arm is pinned against the OCIO-derived expectation at that effective
// peak, never against the SDR arm.
TEST_F(TonemapHdrHeadroom, Aces2PeakParameterisedContract)
{
    // SDR: tier 0 exactly — clamped at paper white, deterministic to the byte.
    const Rgb sdrMidA = Sdr({0.18f, 0.18f, 0.18f}, Mode::ACES2);
    const Rgb sdrMidB = Sdr({0.18f, 0.18f, 0.18f}, Mode::ACES2);
    EXPECT_EQ(sdrMidA.R, sdrMidB.R) << "SDR ACES2 is not deterministic";
    EXPECT_EQ(sdrMidA.G, sdrMidB.G) << "SDR ACES2 is not deterministic";
    EXPECT_EQ(sdrMidA.B, sdrMidB.B) << "SDR ACES2 is not deterministic";
    // Tier-0 mid-grey: the 100-nit transform maps scene 0.18 to 10 nits
    // (0.100 paper-white-relative) — the Academy's stated anchor.
    EXPECT_NEAR(sdrMidA.Peak(), 0.1000f, 2e-3f)
        << "SDR ACES2 mid-grey left the 100-nit tier: " << Fmt(sdrMidA);
    const Rgb sdrBright = Sdr({4096.0f, 4096.0f, 4096.0f}, Mode::ACES2);
    EXPECT_LE(sdrBright.Peak(), 1.0001f) << "SDR ACES2 gained headroom: " << Fmt(sdrBright);

    // HDR: output never exceeds the display's max linear value.
    const Rgb hdrBright = Hdr({4096.0f, 4096.0f, 4096.0f}, Mode::ACES2);
    EXPECT_LE(hdrBright.Peak(), kOutputMax + 1e-3f)
        << "HDR ACES2 exceeded outputMax: " << Fmt(hdrBright);

    // Mid-grey lift is BOUNDED and matches the peak-matched transform, not SDR.
    // Expectation derived from the OCIO reference via the tier validator
    // (tier_at(log2(4.9261)) at scene 0.18 -> 0.131735, +31.7% over tier 0;
    // Tools/ShaderGen/validate_aces2_tiers.py). GPU-vs-validator agreement
    // measured at <= 5e-5; the 2e-3 tolerance covers driver transcendental
    // variance without admitting the SDR value (0.100) or a runaway tier.
    constexpr float kMidGreyAtFixturePeak = 0.131735f;
    const Rgb hdrMid = Hdr({0.18f, 0.18f, 0.18f}, Mode::ACES2);
    EXPECT_NEAR(hdrMid.Peak(), kMidGreyAtFixturePeak, 2e-3f)
        << "ACES2 mid-grey is not the peak-matched transform's value: " << Fmt(hdrMid)
        << " (tier 0 would be 0.100; the " << kPanelPeakNits << "-nit fixture expects "
        << kMidGreyAtFixturePeak << ")";
    EXPECT_LT(hdrMid.Peak(), 0.75f)
        << "ACES2 mid-grey reached the extension knee — the tier selection is runaway: " << Fmt(hdrMid);

    // Zero headroom == the SDR result to the byte (tier 0, t = 0, same clamp).
    TonemapParams p{};
    p.TonemapMode = Mode::ACES2;
    p.OutEncoding = kEncodingHdrLinear;
    p.MaxOutputNits = kPaperWhiteNits;
    const Rgb flat = Rig().Run({0.18f, 0.18f, 0.18f}, p);
    EXPECT_EQ(flat.R, sdrMidA.R) << "zero-headroom HDR ACES2 != SDR: " << Fmt(flat);
    EXPECT_EQ(flat.G, sdrMidA.G);
    EXPECT_EQ(flat.B, sdrMidA.B);
}

// --- 9. ACES near-black: gradation, not a floor ----------------------------
//
// The RRT/ODT fit's numerator constant put the curve below zero for working-space
// values under 0.003253, so the terminal clamp collapsed that whole span onto a
// single value. Two consequences, one test each: the span stops resolving at all
// (here), and a channel that enters the span while its neighbours have not is
// deleted rather than darkened (test 10).
//
// Grey is deliberate: kAcesIn/kAcesOut are row-normalised, so a grey scene value
// passes the matrices unchanged and this probes the CURVE alone, with no
// colour-mixing confound.
TEST_F(TonemapHdrHeadroom, AcesNearBlackKeepsGradationInsteadOfFlooring)
{
    // Every step is below the old zero-crossing, so the pre-fix shader returns
    // exactly 0 for all five and BOTH assertions below fail on it.
    constexpr float kRamp[] = {2.0e-3f, 1.0e-3f, 5.0e-4f, 2.5e-4f, 1.25e-4f};

    float previous = std::numeric_limits<float>::max();
    for (float scene : kRamp)
    {
        const Rgb sdr = Sdr({scene, scene, scene}, Mode::ACES);

        EXPECT_GT(sdr.Peak(), 0.0f)
            << "ACES floored scene " << scene << " to black: " << Fmt(sdr)
            << " — the near-black span is not resolving, so shadow detail is discarded";
        EXPECT_LT(sdr.Peak(), previous)
            << "ACES is not strictly decreasing across the near-black ramp at scene " << scene
            << " (" << Fmt(sdr) << ", previous step " << previous << ") — two distinct scene "
            << "values are rendering to the same pixel";
        previous = sdr.Peak();
    }
}

// --- 10. ACES near-black: no channel is deleted ----------------------------
//
// A sky-lit shadow reaches the curve's negative span in order of channel
// magnitude, so red arrives while green and blue are still resolving. Under the
// old constant this pixel returned red EXACTLY zero with both neighbours alive —
// the surface lost a channel and rotated toward cyan instead of darkening. This
// scene value is that case: a measured sky-lit shadow ratio scaled into the span.
TEST_F(TonemapHdrHeadroom, AcesDoesNotDropAChannelWhileOthersResolve)
{
    const Rgb scene{0.00213f, 0.008863f, 0.01053f};
    const Rgb sdr = Sdr(scene, Mode::ACES);

    // Premise: the neighbours must actually be resolving, or "red survived" is
    // vacuous — it would only mean the whole pixel sits above the span.
    ASSERT_GT(sdr.G, 0.0f) << "probe is not in the near-black span: " << Fmt(sdr);
    ASSERT_GT(sdr.B, 0.0f) << "probe is not in the near-black span: " << Fmt(sdr);

    EXPECT_GT(sdr.R, 0.0f)
        << "ACES deleted the red channel while green and blue resolved: " << Fmt(sdr)
        << " — a shadow lit by a blue sky loses red entirely instead of darkening";

    // Deleted-vs-darkened, not merely nonzero: the output ratio should track the
    // input's rather than collapsing. Input R/B is 0.202; a per-channel curve
    // compresses the small channel harder, so the band is wide and only catches
    // near-annihilation.
    EXPECT_GT(sdr.R / sdr.B, 0.05f)
        << "red survived only as a rounding artefact: " << Fmt(sdr) << " (input R/B "
        << (scene.R / scene.B) << ")";
}

// --- 11. ACES saturated colours are not desaturated ------------------------
//
// REGRESSION GUARD aimed at a specific rejected shape. An earlier attempt at
// test 10's defect added a "gamut floor" that pinned every colour's minimum
// channel to 10% of its own luminance. That fires on saturated colours deep
// inside the gamut, where nothing is being deleted: pure saturated green gained
// 45 codes of red and 56 of blue. Any fix for the near-black span must leave
// saturated mid-tones alone, so this pins them.
//
// The tolerance discriminates rather than merely passing: the legitimate
// near-black correction moves these by at most 3.7e-4, while a 10%-of-luminance
// floor moves saturated green's red by 2.6e-2 — 70x the tolerance.
TEST_F(TonemapHdrHeadroom, AcesSaturatedColoursAreNotDesaturated)
{
    constexpr float kTol = 2.0e-3f;
    struct Pin
    {
        const char* Name;
        Rgb Scene;
        Rgb Expected;
    };
    // Measured on the shipped operator; unchanged to within kTol by the
    // near-black correction.
    const Pin kPins[] = {
        {"saturated green", {0.0f, 0.5f, 0.0f}, {0.0f, 0.367173f, 0.0f}},
        {"pure blue", {0.0f, 0.0f, 0.5f}, {0.0f, 0.0f, 0.337519f}},
        {"pure red", {0.5f, 0.0f, 0.0f}, {0.335191f, 0.0f, 0.001055f}},
        {"foliage green", {0.05f, 0.40f, 0.05f}, {0.014155f, 0.291092f, 0.023609f}},
        {"deep saturated blue", {0.02f, 0.05f, 0.6f}, {0.0f, 0.014845f, 0.409069f}},
        {"mid grey", {0.18f, 0.18f, 0.18f}, {0.105851f, 0.105851f, 0.105851f}},
    };

    for (const Pin& pin : kPins)
    {
        const Rgb sdr = Sdr(pin.Scene, Mode::ACES);
        EXPECT_NEAR(sdr.R, pin.Expected.R, kTol) << pin.Name << " red moved: " << Fmt(sdr);
        EXPECT_NEAR(sdr.G, pin.Expected.G, kTol) << pin.Name << " green moved: " << Fmt(sdr);
        EXPECT_NEAR(sdr.B, pin.Expected.B, kTol) << pin.Name << " blue moved: " << Fmt(sdr);
    }

    // The rejected shape's fingerprint, asserted directly: it drove every colour
    // to the SAME min/luminance ratio. A saturated primary must stay saturated.
    const Rgb green = Sdr({0.0f, 0.5f, 0.0f}, Mode::ACES);
    const float greenLuma = 0.2126f * green.R + 0.7152f * green.G + 0.0722f * green.B;
    ASSERT_GT(greenLuma, 0.0f);
    EXPECT_LT(std::min(green.R, std::min(green.G, green.B)) / greenLuma, 0.01f)
        << "saturated green's minimum channel was lifted toward its luminance: " << Fmt(green)
        << " — a saturation ceiling has been reintroduced";
}

} // namespace
