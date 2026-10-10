#include "Rendering/Passes/PixelPerfectUpscalePass.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"

namespace GameEngine {
namespace Rendering {
namespace Passes {

namespace
{
// pixelperfect_upscale.frag's UpscalePC members, written by NAME against the
// reflected block (never hand-mirrored — see TonemapPass for the drift-class
// rationale). vec2 members are written as float[2].
struct UpscaleValues
{
    float sourceSize[2]{0.0f, 0.0f};
    float referenceSize[2]{0.0f, 0.0f};
    float sampleOrigin[2]{0.0f, 0.0f};
    float outputOrigin[2]{0.0f, 0.0f};
    float outputSize[2]{0.0f, 0.0f};
    float zoom = 1.0f;
    int32_t outEncoding = 1;
    float paperWhiteNits = 203.0f;
};

struct UpscaleDeviceResources
{
    SamplerHandle sampler{INVALID_SAMPLER_HANDLE};
};

struct UpscaleShaderBytes
{
    std::vector<uint8_t> vs;
    std::vector<uint8_t> fs;
    // Reflection meta — the single source of truth for the descriptor-set
    // layout and push-constant block.
    ShaderMeta meta;
    bool loaded = false;
    bool warned = false;
};

static std::unordered_map<IDevice*, UpscaleDeviceResources> g_UpscalePerDevice;

UpscaleShaderBytes& GetUpscaleShaderBytes(const IDevice& device)
{
    static UpscaleShaderBytes s;
    if (!s.loaded)
    {
        ShaderPackage pkg{};
        std::string err;
        if (LoadShaderPkg("Shaders/pixelperfect_upscale.shaderpkg", device.PreferredShaderSource(), pkg, &err))
        {
            auto itVs = pkg.stageBytes.find("vs");
            auto itFs = pkg.stageBytes.find("fs");
            if (itVs != pkg.stageBytes.end())
                s.vs = std::move(itVs->second);
            if (itFs != pkg.stageBytes.end())
                s.fs = std::move(itFs->second);
            s.meta = std::move(pkg.meta);
        }
        // Latch only on success — a not-yet-ready loader environment retries
        // instead of permanently disabling the pass; the warning fires once.
        s.loaded = !s.vs.empty() && !s.fs.empty();
        if (!s.loaded && !s.warned)
        {
            s.warned = true;
            Logger::Log::Warning(
                "PixelPerfectUpscale: Shaders/pixelperfect_upscale.shaderpkg unavailable ({}); "
                "retrying",
                err);
        }
    }
    return s;
}

SamplerHandle GetOrCreateUpscaleSampler(IDevice* device)
{
    if (!device)
        return {};

    auto [it, inserted] = g_UpscalePerDevice.try_emplace(device);
    if (inserted)
    {
        // Drop this per-device cache whenever the device's GPU objects die —
        // shutdown OR an in-place rebuild after device loss. The cached sampler
        // belongs to one VkDevice; a rebuild destroys it while leaving this
        // IDevice* key alive.
        device->RegisterPerDeviceCacheCleanup(
            "Passes.PixelPerfectUpscale",
            [](IDevice* d) { CleanupPixelPerfectUpscalePassForDevice(d); });
    }

    auto& res = it->second;
    if (res.sampler.IsValid())
        return res.sampler;

    // Nearest (point) filtering with clamp: crisp blocks, no bleeding at the
    // padded RT border when the sub-pixel window shift reaches its extreme.
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = 0; // nearest
    samplerDesc.magFilter = 0; // nearest
    samplerDesc.mipFilter = 0;
    samplerDesc.addressModeU = 2; // clamp to edge
    samplerDesc.addressModeV = 2;
    samplerDesc.addressModeW = 2;
    samplerDesc.debugName = "PixelPerfectUpscale.NearestClamp";
    res.sampler = device->CreateSampler(samplerDesc);
    return res.sampler;
}

int32_t SelectOutputEncoding(IDevice* device, bool useBackbuffer)
{
    // HDR transfer functions are display-referred — they only belong on the
    // swapchain. An offscreen target gets plain sRGB regardless of the display's
    // active HDR mode, so a readback of it yields a correct SDR image.
    const HdrOutputMode outputMode = useBackbuffer ? device->GetActiveHdrOutputMode() : HdrOutputMode::Off;
    switch (outputMode)
    {
    case HdrOutputMode::HDR10_PQ:
    case HdrOutputMode::HDR10Plus:
        return 2;
    case HdrOutputMode::HLG:
        return 3;
    case HdrOutputMode::ScRGB:
        return 4;
    default:
        // SDR. A UNORM swapchain needs the manual linear->sRGB encode; an sRGB
        // swapchain hardware-encodes on write, so pass linear through (encoding 4
        // with paperWhite 80 -> scale 1.0). Dedicated offscreen targets always
        // need the manual encode.
        return (!useBackbuffer || device->SwapchainNeedsManualSRGBEncode()) ? 1 : 4;
    }
}

} // namespace

void CleanupPixelPerfectUpscalePassForDevice(IDevice* device)
{
    if (!device)
        return;
    auto it = g_UpscalePerDevice.find(device);
    if (it == g_UpscalePerDevice.end())
        return;
    if (it->second.sampler.IsValid())
        device->DestroySampler(it->second.sampler);
    g_UpscalePerDevice.erase(it);
}

RenderGraph::RGPass AddPixelPerfectUpscalePassRG(RenderGraph::RGFrame& frame,
                                         const PixelPerfectUpscaleParamsRG& params,
                                         FinalizeInputSpace inputSpace,
                                         const char* passName)
{
    IDevice* device = frame.Device();
    if (!device || !params.Src.IsValid() || !params.Dst.IsValid() ||
        params.Src.Id == params.Dst.Id)
        return {};
    // Finalize contract (FinalizeContract.h): an already-encoded input is
    // SDR-only. Refusing to declare is this pass's loud channel — a missing
    // upscale is unmissable, a silently wrong re-encode is not.
    const bool dstIsBackbuffer = frame.IsBackbuffer(params.Dst);
    const HdrOutputMode outputMode =
        dstIsBackbuffer ? device->GetActiveHdrOutputMode() : HdrOutputMode::Off;
    if (IsFinalizeSpaceHdrViolation(inputSpace, IsHdrOutputModeActive(outputMode)))
    {
        static bool s_WarnedEncodedHdr = false;
        if (!s_WarnedEncodedHdr)
        {
            s_WarnedEncodedHdr = true;
            Logger::Log::Error(
                "PixelPerfectUpscale: '{}' declared FinalizeInputSpace::EncodedSrgb under an "
                "active HDR output mode — the encoded arm is SDR-only; pass not declared",
                passName ? passName : "PixelPerfectUpscale");
        }
        return {};
    }
    // I11 needs no check here: this pass samples NEAREST (NearestClamp, below),
    // so it replicates texels rather than mixing them and cannot low-pass the
    // dither an encoded source arrives with — which is exactly the exemption the
    // invariant is carved around. The transport that CAN break it is the terminal
    // encode, which samples linear, and it asserts the extent rule on itself.
    const auto& shaders = GetUpscaleShaderBytes(*device);
    if (shaders.vs.empty() || shaders.fs.empty())
        return {};

    // Layout + push-constant block derived from the shaderpkg's reflection
    // meta. Interned per device; the per-frame cost is the variant-cache hit.
    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(shaders.vs);
    gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(shaders.fs);
    gd.Topology = PrimitiveTopology::TriangleList;
    gd.Rasterization.cullMode = CullModeFlagBits::None;
    gd.Rasterization.frontFace = FrontFace::CounterClockwise;
    gd.DepthStencil.depthTestEnable = false;
    gd.DepthStencil.depthWriteEnable = false;
    DynamicStateInfo dyn{};
    dyn.states.push_back(DynamicState::Viewport);
    dyn.states.push_back(DynamicState::Scissor);
    gd.DynamicState = std::move(dyn);
    gd.DebugName = "PixelPerfectUpscale.Pipeline";
    DescriptorSetLayoutDesc set0{};
    {
        std::string metaErr;
        if (!MaterialHelper::ApplyShaderMetaToGraphicsDesc(
                *device, shaders.meta, gd, MaterialBuilder::MergeMode::Auto, {true, 128},
                [&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
                {
                    if (setIndex == 0)
                        set0 = dsl;
                },
                &metaErr))
        {
            static bool s_WarnedMeta = false;
            if (!s_WarnedMeta)
            {
                s_WarnedMeta = true;
                Logger::Log::Warning("PixelPerfectUpscale: shader meta rejected ({})", metaErr);
            }
            return {};
        }
    }
    const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(gd));
    if (!pipelineId.IsValid())
        return {};

    // Push constants computed at DECLARATION (the by-value contract): the
    // destination extent comes from the frame's resource desc, the letterbox
    // math mirrors the old pass's Execute, and the output encoding follows
    // the declared input space — a linear input keeps the passthrough /
    // self-encode duality; an encoded input passes bytes through raw (or
    // hands an _SRGB destination D(c)).
    const RenderGraph::RGResourceDesc dd = frame.Graph().ResourceDesc(params.Dst.Id);
    const uint32_t dstWidth = dd.Width > 0 ? dd.Width : 1u;
    const uint32_t dstHeight = dd.Height > 0 ? dd.Height : 1u;

    const float zoom = static_cast<float>(std::max(1u, params.Zoom));
    const float refW = static_cast<float>(std::max(1u, params.ReferenceWidth));
    const float refH = static_cast<float>(std::max(1u, params.ReferenceHeight));
    const float outW = refW * zoom;
    const float outH = refH * zoom;
    // Center the upscaled rect; floor so the origin lands on a whole pixel.
    const float outOriginX = std::floor((static_cast<float>(dstWidth) - outW) * 0.5f);
    const float outOriginY = std::floor((static_cast<float>(dstHeight) - outH) * 0.5f);

    UpscaleValues pc{};
    pc.sourceSize[0] = static_cast<float>(std::max(1u, params.SourceWidth));
    pc.sourceSize[1] = static_cast<float>(std::max(1u, params.SourceHeight));
    pc.referenceSize[0] = refW;
    pc.referenceSize[1] = refH;
    // Sample-window origin = the one-texel border, moved by the sub-pixel
    // remainder in the direction the camera moved. Texel columns grow with
    // world X, so a camera to the right of its snap reads further right; texel
    // rows grow downward against world Y, so a camera above its snap reads
    // further up.
    pc.sampleOrigin[0] = 1.0f + params.FracX;
    pc.sampleOrigin[1] = 1.0f - params.FracY;
    pc.outputOrigin[0] = outOriginX;
    pc.outputOrigin[1] = outOriginY;
    pc.outputSize[0] = outW;
    pc.outputSize[1] = outH;
    pc.zoom = zoom;
    if (inputSpace == FinalizeInputSpace::EncodedSrgb)
    {
        // Encoded bytes pass through untouched — the point-sampled integer
        // upscale is byte-preserving and the OETF already ran upstream. The
        // only transform is destination-keyed, mirroring encode_srgb's arm 6:
        // an _SRGB destination takes D(c) so the ROP's re-encode round-trips
        // byte-exact; everything else takes the raw bytes (0).
        const TextureFormat dstFormat =
            dstIsBackbuffer
                ? device->GetSwapchainTextureFormat()
                : static_cast<TextureFormat>(frame.Graph().ResourceDesc(params.Dst.Id).Format);
        const bool dstIsSrgbRop =
            dstFormat == TextureFormat::RGBA8_SRGB || dstFormat == TextureFormat::BGRA8_SRGB;
        pc.outEncoding = dstIsSrgbRop ? 6 : 0;
    }
    else
    {
        pc.outEncoding =
            params.PassthroughLinear ? 0 : SelectOutputEncoding(device, dstIsBackbuffer);
    }
    pc.paperWhiteNits = device->GetHdrOutputState().staticMetadata.paperWhiteNits;

    // Values -> push constants by MEMBER NAME against the reflected block,
    // resolved at declaration (the by-value contract).
    NamedPushConstantWriter pcw =
        shaders.meta.PushConstants.empty()
            ? NamedPushConstantWriter(shaders.meta, std::string{})
            : NamedPushConstantWriter(shaders.meta, shaders.meta.PushConstants[0].Name);
    if (pcw.IsValid())
    {
        pcw.AddRaw("sourceSize", pc.sourceSize, sizeof(pc.sourceSize));
        pcw.AddRaw("referenceSize", pc.referenceSize, sizeof(pc.referenceSize));
        pcw.AddRaw("sampleOrigin", pc.sampleOrigin, sizeof(pc.sampleOrigin));
        pcw.AddRaw("outputOrigin", pc.outputOrigin, sizeof(pc.outputOrigin));
        pcw.AddRaw("outputSize", pc.outputSize, sizeof(pc.outputSize));
        pcw.Add("zoom", pc.zoom);
        pcw.Add("outEncoding", pc.outEncoding);
        pcw.Add("paperWhiteNits", pc.paperWhiteNits);
    }

    const RenderGraph::RGTexture src = params.Src;
    return frame.AddPass(
        passName ? passName : "PixelPerfectUpscale", PassPhase::kFinalize,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(src, RenderGraph::RGTextureRead::Sampled);
            RenderGraph::RGAttachmentOps ops{};
            // The fullscreen shader writes every destination pixel (letterbox
            // bars are shader-black), so DontCare is safe — the old arm's
            // Load was only alias hygiene.
            ops.Load = RenderGraph::RGLoadOp::DontCare;
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, params.Dst, ops);
        },
        [pipelineId, src, set0, pcw, meta = &shaders.meta, dstWidth, dstHeight](RenderGraph::RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            auto* dev = ctx.GetDevice();
            if (!cl || !dev)
                return;
            const auto srcTex = ctx.GetTexture(src);
            if (!srcTex.IsValid())
                return;
            const SamplerHandle sampler = GetOrCreateUpscaleSampler(dev);
            if (!sampler.IsValid())
                return;
            const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
            if (!pipeline.IsValid())
                return;

            cl->SetPipeline(pipeline);
            if (pcw.IsValid())
                pcw.Flush(cl);
            // Viewport is the full destination — the shader letterboxes via
            // gl_FragCoord.
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(dstWidth),
                            static_cast<float>(dstHeight));
            cl->SetScissor(0, 0, static_cast<int>(dstWidth), static_cast<int>(dstHeight));

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = set0;
            dsDesc.transient = true;
            dsDesc.debugName = "PixelPerfectUpscale.DS0";
            const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            NamedDescriptorWriter wd(dev, ds, *meta, 0);
            wd.AddCombinedImageSampler("uSourceColor", srcTex, sampler);
            wd.Flush();
            cl->BindDescriptorSet(0, ds, pipeline);
            cl->Draw(3, 1);
        });
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
