// The motion payload's two reconstruction lanes, proved by rendering.
//
// The composed motion variant builds its vector from three things the producer
// puts in the per-view block and one the rasterizer gives it: the viewport rect
// gl_FragCoord is measured against, this frame's temporal jitter as a
// viewport-UV offset, the previous endpoint's unjittered clip position, and the
// fragment's own raster position. The jitter lane has a sign, and a sign
// convention is not provable by reading either side of it — the rect is
// top-left before the backend's negative-height flip, the jitter is added in
// Y-up normalized device coordinates and consumed in a Y-down viewport UV, and
// getting either backwards produces a plausible vector that is wrong by twice
// the jitter.
//
// So this renders. A known point is projected through the production
// ApplyNdcJitter so that its jittered raster position lands exactly on a pixel
// centre; that pixel's payload must then carry the delta between the point's
// UNJITTERED viewport UV and its previous endpoint's — the value the temporal
// resolve and the reflection reprojection both index by. The run is repeated
// with the jitter negated, so a flipped sign fails on one arm or the other
// whatever the magnitude.
//
// What is real here and what is not: the payload arithmetic is the shipped
// include (motion_vector_payload.glsl over screen_position.glsl), the block is
// the shipped declaration (deformation_motion.glsl), the block's contents come
// from the production BuildDeformationMotionParams, and the viewport is set
// through the same CommandList call the producer's pass makes. The two probe
// stages are written here: this suite pins the conversion, not the adapter's
// varying packing, which its own suite pins.

#include <gtest/gtest.h>

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/DeformationMotionParams.h"
#include "Engine/Rendering/ViewTemporalHistory.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderReflection.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef GE_RENDERER_REPO_ROOT
#error "GE_RENDERER_REPO_ROOT must be defined by CMake (source anchoring for shader contracts)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::ApplyNdcJitter;
using GameEngine::Engine::Renderer::BuildDeformationMotionParams;
using GameEngine::Engine::Renderer::DeformationMotionEndpoint;
using GameEngine::Engine::Renderer::DeformationMotionParamsGPU;
using GameEngine::Engine::Renderer::DeformationMotionRaster;
using GameEngine::Engine::Renderer::ViewTemporalSample;

namespace
{
namespace fs = std::filesystem;

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 32;
// The pixel the probe reads back. Off-centre on both axes so a swapped or
// mirrored axis cannot pass by symmetry.
constexpr uint32_t kProbeX = 19;
constexpr uint32_t kProbeY = 7;

// The half-open jitter range temporal anti-aliasing rotates through, in texels.
constexpr float kJitterTexelsX = 0.37f;
constexpr float kJitterTexelsY = -0.21f;

// Mirrors GE_YUpNdcToViewportUV (Shaders/Includes/screen_position.glsl). The
// oracle is this conversion applied to values the test computes itself, never
// to anything the shader produced.
struct Vec2
{
    float X = 0.0f;
    float Y = 0.0f;
};

Vec2 YUpNdcToViewportUV(Vec2 ndc)
{
    return {ndc.X * 0.5f + 0.5f, 1.0f - (ndc.Y * 0.5f + 0.5f)};
}

Vec2 ViewportUVToYUpNdc(Vec2 uv)
{
    return {uv.X * 2.0f - 1.0f, (1.0f - uv.Y) * 2.0f - 1.0f};
}

const char* kProbeVertexSource = R"(#version 450
// The three packed components the motion variant's vertex stage emits, on the
// locations and components the adapter uses.
layout(location = 0, component = 2) out vec2 vMotionPrevClipXY;
layout(location = 2, component = 3) out float vMotionPrevClipZ;
layout(location = 15, component = 3) out float vMotionPrevClipW;

layout(push_constant) uniform ProbePush
{
    vec4 uPrevClip;
} Probe;

void main()
{
    // Full-target triangle: every pixel of the target is covered, so the probe
    // pixel's fragment exists and carries its own raster position.
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.5, 1.0);
    vMotionPrevClipXY = Probe.uPrevClip.xy;
    vMotionPrevClipZ = Probe.uPrevClip.z;
    vMotionPrevClipW = Probe.uPrevClip.w;
}
)";

const char* kProbeFragmentSource = R"(#version 450
layout(location = 0) out vec4 oMotion;

layout(location = 0, component = 2) in vec2 vMotionPrevClipXY;
layout(location = 2, component = 3) in float vMotionPrevClipZ;
layout(location = 15, component = 3) in float vMotionPrevClipW;

#include "deformation_motion.glsl"
#include "motion_vector_payload.glsl"

void main()
{
    GE_ScreenRect viewport;
    viewport.originPx = MotionParams.uMotionViewportRect.xy;
    viewport.sizePx = MotionParams.uMotionViewportRect.zw;
    oMotion = GE_MotionVectorPayload(
        viewport, MotionParams.uMotionJitterUv.xy,
        vec4(vMotionPrevClipXY, vMotionPrevClipZ, vMotionPrevClipW));
}
)";

// Half-float decode for the R16G16B16A16_FLOAT readback.
float DecodeHalf(uint16_t bits)
{
    const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    uint32_t out = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
            out = sign;
        else
        {
            uint32_t e = 127 - 15 + 1;
            uint32_t m = mantissa;
            while ((m & 0x400u) == 0)
            {
                m <<= 1;
                --e;
            }
            m &= 0x3FFu;
            out = sign | (e << 23) | (m << 13);
        }
    }
    else if (exponent == 0x1Fu)
        out = sign | 0x7F800000u | (mantissa << 13);
    else
        out = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float result = 0.0f;
    std::memcpy(&result, &out, sizeof(result));
    return result;
}

struct ProbeResult
{
    bool Ran = false;
    std::string SkipReason;
    float Motion[4]{};
};

// Compile the two probe stages against the shipped include tree.
bool CompileProbeStages(std::vector<uint8_t>& outVertex, std::vector<uint8_t>& outFragment,
                        std::string& outError)
{
    ShaderProgramCompileRequest req{};
    req.debugName = "DeformationMotionPayloadProbe";
    const fs::path shaderRoot =
        fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "Rendering" / "Shaders";
    req.baseDirectory = shaderRoot / "Adapters";
    req.cacheRoot = fs::temp_directory_path() / "ge_motion_payload_probe_cache";
    req.includeDirs = {shaderRoot / "Includes"};

    ShaderStageCompileSpec vs{};
    vs.stage = "vs";
    vs.sourcePath = req.baseDirectory / "deformation_motion_probe.vert";
    vs.inlineSource = kProbeVertexSource;
    ShaderStageCompileSpec fs_{};
    fs_.stage = "fs";
    fs_.sourcePath = req.baseDirectory / "deformation_motion_probe.frag";
    fs_.inlineSource = kProbeFragmentSource;
    req.stages = {vs, fs_};

    ShaderProgramCompileResult result{};
    if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result,
                                                    &outError))
        return false;
    const auto vsIt = result.stageBytes.find("vs");
    const auto fsIt = result.stageBytes.find("fs");
    if (vsIt == result.stageBytes.end() || fsIt == result.stageBytes.end())
    {
        outError = "probe program produced no stage bytes";
        return false;
    }
    outVertex = vsIt->second;
    outFragment = fsIt->second;
    return true;
}

// Render one frame of the probe with `params` and `prevClip`, and read the
// payload back from the probe pixel.
ProbeResult RenderProbe(const DeformationMotionParamsGPU& params, const float prevClip[4])
{
    ProbeResult out{};
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    if (!ShaderCompileService::IsCompilerAvailable())
    {
        out.SkipReason = "no shader compiler in this build";
        return out;
    }

    std::vector<uint8_t> vertexSpv;
    std::vector<uint8_t> fragmentSpv;
    std::string error;
    if (!CompileProbeStages(vertexSpv, fragmentSpv, error))
    {
        out.SkipReason = "probe compile failed: " + error;
        return out;
    }

    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        out.SkipReason = "no graphics device";
        return out;
    }

    ReflectionOptions ro{};
    StageReflectionResult rvs{};
    StageReflectionResult rfs{};
    if (!ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vertexSpv.data()),
                      vertexSpv.size() / 4, ro, rvs, &error) ||
        !ReflectSpirv(ShaderStageKind::Fragment,
                      reinterpret_cast<const uint32_t*>(fragmentSpv.data()),
                      fragmentSpv.size() / 4, ro, rfs, &error))
    {
        out.SkipReason = "probe reflection failed: " + error;
        return out;
    }
    ShaderMeta meta = MergeStages({rvs, rfs});

    TextureDesc dstDesc{};
    dstDesc.width = kWidth;
    dstDesc.height = kHeight;
    dstDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                    static_cast<uint32_t>(TextureUsage::TransferSrc);
    const TextureHandle dst = dev->CreateTexture(dstDesc);
    if (!dst.IsValid())
    {
        out.SkipReason = "render target creation failed";
        return out;
    }

    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vertexSpv;
    pd.pixelShader = fragmentSpv;
    pd.debugName = "DeformationMotionPayloadProbe";
    MaterialBuilder::FormatsHint formats{};
    formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT)};
    std::string buildError;
    if (!MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, formats,
                                                    MaterialBuilder::MergeMode::Auto,
                                                    MaterialBuilder::PushConstantPolicy{},
                                                    &buildError))
    {
        out.SkipReason = "probe pipeline desc failed: " + buildError;
        return out;
    }
    const PipelineHandle pipe = dev->CreatePipeline(pd);
    if (pipe == INVALID_HANDLE)
    {
        out.SkipReason = "probe pipeline creation failed";
        return out;
    }

    // The block the fragment reads, and the never-read history the shipped
    // declaration carries beside it.
    BufferDesc paramsDesc{};
    paramsDesc.size = sizeof(DeformationMotionParamsGPU);
    paramsDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
    paramsDesc.memoryUsage = BufferMemoryUsage::Upload;
    paramsDesc.debugName = "DeformationMotionPayloadProbe.Params";
    const BufferHandle paramsBuffer = dev->CreateBuffer(paramsDesc);
    BufferDesc historyDesc{};
    historyDesc.size = sizeof(uint32_t);
    historyDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    historyDesc.memoryUsage = BufferMemoryUsage::Upload;
    historyDesc.debugName = "DeformationMotionPayloadProbe.History";
    const BufferHandle historyBuffer = dev->CreateBuffer(historyDesc);
    if (!paramsBuffer.IsValid() || !historyBuffer.IsValid())
    {
        out.SkipReason = "probe buffer creation failed";
        return out;
    }
    if (void* mapped = dev->MapBuffer(paramsBuffer))
    {
        std::memcpy(mapped, &params, sizeof(params));
        dev->UnmapBuffer(paramsBuffer);
    }
    if (void* mapped = dev->MapBuffer(historyBuffer))
    {
        const uint32_t continuous = 0x200u;
        std::memcpy(mapped, &continuous, sizeof(continuous));
        dev->UnmapBuffer(historyBuffer);
    }

    if (pd.descriptorSetLayouts.empty())
    {
        out.SkipReason = "probe pipeline reflected no descriptor set";
        return out;
    }
    DescriptorSetDesc dsDesc{};
    dsDesc.layout = pd.descriptorSetLayouts[0];
    dsDesc.transient = true;
    dsDesc.debugName = "DeformationMotionPayloadProbe.Set0";
    const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
    if (!ds.IsValid())
    {
        out.SkipReason = "probe descriptor set creation failed";
        return out;
    }
    dev->UpdateBufferBinding(ds, 45, paramsBuffer, 0, sizeof(DeformationMotionParamsGPU));
    dev->UpdateStorageBufferBinding(ds, 46, historyBuffer, 0, sizeof(uint32_t));

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::Undefined,
                                                      ResourceState::RenderTarget));
    RenderPassDesc rp{};
    rp.colorTargets[0] = dst;
    rp.colorTargetCount = 1;
    rp.clearColor[0] = true;
    rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(0, ds, pipe);
    // The same call the producer's pass makes: a top-left rect, flipped to the
    // Y-up viewport by the backend.
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
    cl->SetScissor(0, 0, kWidth, kHeight);
    cl->SetConstants(0, 0, sizeof(float) * 4, prevClip);
    cl->Draw(3, 1);
    cl->EndRenderPass();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::RenderTarget,
                                                      ResourceState::CopySource));
    const uint32_t readbackBytes = kWidth * kHeight * 8u;
    const BufferHandle readback = dev->CreateReadbackBuffer(readbackBytes);
    cl->CopyTextureToBuffer(dst, readback, kWidth, kHeight);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    if (const auto* bytes = static_cast<const uint8_t*>(dev->MapBuffer(readback)))
    {
        const size_t texel = (static_cast<size_t>(kProbeY) * kWidth + kProbeX) * 8u;
        for (int c = 0; c < 4; ++c)
        {
            uint16_t half = 0;
            std::memcpy(&half, bytes + texel + static_cast<size_t>(c) * 2u, sizeof(half));
            out.Motion[c] = DecodeHalf(half);
        }
        dev->UnmapBuffer(readback);
        out.Ran = true;
    }
    else
        out.SkipReason = "readback mapping failed";

    dev->DestroyBuffer(readback);
    dev->DestroyBuffer(historyBuffer);
    dev->DestroyBuffer(paramsBuffer);
    dev->DestroyTexture(dst);
    return out;
}

// One arm of the convention: a point whose JITTERED projection lands on the
// probe pixel's centre, and a previous endpoint for it.
struct ConventionArm
{
    DeformationMotionParamsGPU Params{};
    float PrevClip[4]{};
    Vec2 ExpectedDelta{};
};

ConventionArm BuildArm(float jitterTexelsX, float jitterTexelsY)
{
    ConventionArm arm{};

    // The frozen NDC offsets the view would carry for this jitter, through the
    // production conversion (ViewRegistry::ResolveJitteredCameraData).
    const float ndcJitterX = 2.0f * jitterTexelsX / static_cast<float>(kWidth);
    const float ndcJitterY = 2.0f * jitterTexelsY / static_cast<float>(kHeight);

    // The probe pixel's centre IS the jittered raster position of the point
    // under test, by construction.
    const Vec2 jitteredUv{(static_cast<float>(kProbeX) + 0.5f) / static_cast<float>(kWidth),
                          (static_cast<float>(kProbeY) + 0.5f) / static_cast<float>(kHeight)};
    const Vec2 jitteredNdc = ViewportUVToYUpNdc(jitteredUv);
    // ApplyNdcJitter ADDS the offset in Y-up normalized device coordinates, so
    // the unjittered projection of the same point is the offset taken back off.
    const Vec2 currentNdc{jitteredNdc.X - ndcJitterX, jitteredNdc.Y - ndcJitterY};
    const Vec2 currentUv = YUpNdcToViewportUV(currentNdc);

    // A previous endpoint elsewhere in the viewport: the vertex moved.
    const Vec2 previousUv{0.28f, 0.66f};
    const Vec2 previousNdc = ViewportUVToYUpNdc(previousUv);
    // Carried through a non-unit w so the payload's perspective divide runs.
    const float previousW = 2.5f;
    arm.PrevClip[0] = previousNdc.X * previousW;
    arm.PrevClip[1] = previousNdc.Y * previousW;
    arm.PrevClip[2] = 0.25f * previousW;
    arm.PrevClip[3] = previousW;

    arm.ExpectedDelta = {currentUv.X - previousUv.X, currentUv.Y - previousUv.Y};

    // The block, through the production builder: the same call the producer's
    // pass makes, with the same argument shapes.
    ViewTemporalSample previous{};
    previous.DeformationOrigin = 0.0;
    DeformationMotionEndpoint endpoint{};
    endpoint.Previous = &previous;
    endpoint.PreviousValid = true;
    endpoint.CurrentOrigin = 0.0;
    DeformationMotionRaster raster{};
    raster.Viewport = Mathematics::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                        static_cast<float>(kHeight)};
    raster.NdcJitterX = ndcJitterX;
    raster.NdcJitterY = ndcJitterY;
    BuildDeformationMotionParams(endpoint, raster, arm.Params);
    return arm;
}

// Half-float storage of a delta of this magnitude resolves to about 5e-4; the
// tolerance is that, not a fitted number. A flipped jitter sign moves the
// result by 2 * jitter / extent, which is 1.2e-2 on x and 1.3e-2 on y here —
// more than twenty times the tolerance, which is what makes the arm
// falsifiable rather than merely green.
constexpr float kHalfFloatTolerance = 1.0e-3f;

} // namespace

TEST(DeformationMotionPayload, JitterIsTakenBackOffInBothSigns)
{
    for (const int sign : {1, -1})
    {
        const ConventionArm arm =
            BuildArm(static_cast<float>(sign) * kJitterTexelsX,
                     static_cast<float>(sign) * kJitterTexelsY);
        const ProbeResult result = RenderProbe(arm.Params, arm.PrevClip);
        if (!result.Ran)
            GTEST_SKIP() << result.SkipReason;

        SCOPED_TRACE(sign > 0 ? "positive jitter" : "negated jitter");
        EXPECT_NEAR(result.Motion[0], arm.ExpectedDelta.X, kHalfFloatTolerance);
        EXPECT_NEAR(result.Motion[1], arm.ExpectedDelta.Y, kHalfFloatTolerance);
        // The third channel is the previous endpoint's normalized depth and the
        // fourth is validity: a written texel says an exact vector exists here.
        EXPECT_NEAR(result.Motion[2], 0.25f, kHalfFloatTolerance);
        EXPECT_NEAR(result.Motion[3], 1.0f, kHalfFloatTolerance);
    }
}

TEST(DeformationMotionPayload, APreviousEndpointBehindTheEyeExportsTheSentinel)
{
    ConventionArm arm = BuildArm(kJitterTexelsX, kJitterTexelsY);
    // A vertex behind the eye of its own frame has no usable previous surface.
    // This is the encoding the producer uses for a discontinuous instance too,
    // so the row gates both.
    arm.PrevClip[3] = 0.0f;
    const ProbeResult result = RenderProbe(arm.Params, arm.PrevClip);
    if (!result.Ran)
        GTEST_SKIP() << result.SkipReason;

    EXPECT_NEAR(result.Motion[0], 100.0f, 0.5f);
    EXPECT_NEAR(result.Motion[1], 100.0f, 0.5f);
    EXPECT_NEAR(result.Motion[3], 0.0f, kHalfFloatTolerance);
}
