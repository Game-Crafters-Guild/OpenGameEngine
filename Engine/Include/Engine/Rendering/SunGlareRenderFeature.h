#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// std140/push-constant mirror of GlarePC in sun_glare.vert / sun_glare.frag.
struct SunGlarePushConstants
{
    float SunDirRadius[4]{0.0f, 1.0f, 0.0f, 0.0f};       // xyz toward-sun dir, w angular radius
    float SunColorIrradiance[4]{1.0f, 1.0f, 1.0f, 0.0f}; // rgb sun colour, w irradiance E
    float Probe[4]{0.5f, 0.5f, 0.0f, 0.0f};              // xy sun UV, zw probe radius UV per axis
    float Params[4]{1.0f, 1.0f, 0.0f, 0.0f};             // x sky scale, y sun mu, z altitude,
                                                         // w scene-depth sample count, 0 = the
                                                         // screen probe is off this frame
    float CameraShadow[4]{0.0f, 0.0f, 0.0f, 0.0f};       // xyz camera position, w cascades ready
    // x = terrain-skyline visibility in [0, 1], computed on the CPU from the heightfield so it
    // holds whether or not the sun is on screen. yzw are std140 padding: a push-constant block
    // member cannot be tighter than a vec4.
    float Horizon[4]{1.0f, 0.0f, 0.0f, 0.0f};
};
// The GPU ABI is the member OFFSETS, so those are what is asserted. The struct carries no
// alignment specifier: four-float arrays already land on the 16-byte boundaries a std140 vec4
// needs, and an alignas here only pads the render-pass lambda that captures it by value.
static_assert(sizeof(SunGlarePushConstants) == 96,
              "SunGlarePushConstants must match the shaders' GlarePC block");
static_assert(offsetof(SunGlarePushConstants, SunDirRadius) == 0);
static_assert(offsetof(SunGlarePushConstants, SunColorIrradiance) == 16);
static_assert(offsetof(SunGlarePushConstants, Probe) == 32);
static_assert(offsetof(SunGlarePushConstants, Params) == 48);
static_assert(offsetof(SunGlarePushConstants, CameraShadow) == 64);
static_assert(offsetof(SunGlarePushConstants, Horizon) == 80);
// The packager sums the PER-STAGE ranges against a 128-byte limit. The vertex stage declares
// all 96 and the fragment stage a 32-byte prefix, which is exactly 128 -- the Vulkan-guaranteed
// minimum. Anything added here has to come out of the fragment prefix or out of this block.
static_assert(sizeof(SunGlarePushConstants) + 32u <= 128u,
              "vertex block + fragment prefix must fit the guaranteed push-constant limit");

// Owns the GPU resources for the analytic sun glare: the additive fullscreen pipeline and
// the sampler its two hoisted vertex-stage lookups use. SunGlareRenderNode declares the
// pass and fills the push constants per view.
//
// TWO pipelines, differing only in the vertex stage's depth uniform. The glare's occlusion
// probe texel-fetches the scene depth ATTACHMENT the world pass wrote, and that attachment is
// multisampled whenever MSAA is on, so it needs a sampler2DMS variant. The shaderpkg packager
// has no keyword mechanism, so a variant is a second package — the same shape as
// ddgi_trace_hw/_sw and world_debug/world_depthonly.
class SunGlareRenderFeature : public IRenderFeature
{
  public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void OnDeviceRebuilt(::GameEngine::Rendering::IDevice* device) override;

    // Terrain skyline along the sun's azimuth, per view, as a TANGENT (rise over run) of the
    // highest terrain elevation an eye sees in that direction.
    //
    // Published by the TERRAIN side rather than computed here, and that is a layering fact
    // rather than a preference: TerrainECS depends on the renderer (it owns a render feature),
    // so the renderer cannot include TerrainECS without a cycle. The terrain extraction system
    // already runs every frame with the world and with RenderServices, so it marches each
    // view's camera and leaves the answer here as a plain float.
    //
    // kNoSkyline means "this term has nothing to say" — no terrain in the scene, or no station
    // along the ray that could be sampled — and a reader must treat it as fully visible rather
    // than as flat ground.
    static constexpr float kNoSkyline = -1.0e30f;

    struct ViewSkyline
    {
        uint32_t ViewId = 0u;
        float Tangent = kNoSkyline;
    };

    // Replaces the WHOLE set every frame rather than accumulating into it. View ids churn as
    // previews and thumbnails come and go, and an accumulating table filled up with dead ids
    // and then starved the live scene view of its terrain term -- silently, because a missing
    // entry reads as "no terrain" rather than as an error. One call per frame, so there is no
    // begin/end pair to get wrong and no capacity to overflow.
    void SetSkylines(const ViewSkyline* entries, size_t count);
    float GetSkylineTangent(uint32_t viewId) const;

    // `sampleCount` is the scene depth attachment's own sample count. An MSAA view with no
    // multisampled variant available returns an invalid id rather than binding a
    // multisampled image to a sampler2D, which is undefined.
    ::GameEngine::Rendering::GraphicsPipelineId GetPipelineId(uint32_t sampleCount) const
    {
        return sampleCount > 1u ? m_PipelineIdMS : m_PipelineId;
    }
    // The layout that matches GetPipelineId(sampleCount): the multisampled one declares the
    // depth probe as a multisampled image, which WebGPU bakes into the bind group layout.
    const ::GameEngine::Rendering::DescriptorSetLayoutDesc& GetLayout(uint32_t sampleCount) const
    {
        return sampleCount > 1u ? m_LayoutMS : m_Layout;
    }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    ::GameEngine::Rendering::SamplerHandle GetShadowSampler() const { return m_ShadowSampler; }

    uint32_t GetTransmittanceBinding() const { return m_TransmittanceBinding; }
    uint32_t GetDepthBinding() const { return m_DepthBinding; }
    uint32_t GetViewParamsBinding() const { return m_ViewParamsBinding; }
    uint32_t GetShadowArrayBinding() const { return m_ShadowArrayBinding; }
    uint32_t GetShadowDataBinding() const { return m_ShadowDataBinding; }

  private:
    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;
    // A failed Initialize is final for this device: the node asks every frame.
    bool m_InitializeFailed = false;
    ::GameEngine::Rendering::PipelineDesc m_Pipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineIdMS{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_LayoutMS{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler{};
    ::GameEngine::Rendering::SamplerHandle m_ShadowSampler{};
    uint32_t m_TransmittanceBinding = 0;
    uint32_t m_DepthBinding = 1;
    uint32_t m_ViewParamsBinding = 2;
    uint32_t m_ShadowArrayBinding = 3;
    uint32_t m_ShadowDataBinding = 4;
    // Flat rather than a map: a frame has a handful of views, this is written once and read
    // once per view per frame, and a linear scan beats a hash at that size. Reused across
    // frames so the per-frame refill does not allocate.
    std::vector<ViewSkyline> m_Skyline;
};

} // namespace GameEngine::Engine::Renderer
