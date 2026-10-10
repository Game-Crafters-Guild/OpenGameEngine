#pragma once

#include "Engine/Rendering/AmbientFloorData.h" // AmbientFloorData (authored ambient floor, uploaded via EnvData)
#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Core/DeviceFrameCounter.h" // unwraps the device frame index for the EnvData ring
#include "Rendering/Utils/IBLSet.h" // IBLSet + Device.h (TextureHandle, SamplerHandle, BufferHandle)

#include <cstdint>
#include <memory>

namespace GameEngine
{
namespace Rendering
{
class IDevice;
} // namespace Rendering

namespace Engine::Renderer
{

class IEnvironmentSource;

// Owns the GPU resources for image-based lighting: a diffuse irradiance cube, a
// prefiltered specular cube, a shared split-sum BRDF LUT, their samplers, and the
// EnvData UBO. Engine-shared and view-independent — the environment is a function
// of the sun/atmosphere, not the camera — so one set is bound into every view's
// world pass (unlike the per-view ShadowMapRenderFeature).
//
// Initialize seeds the cubes as a 1x1x6 ambient-constant fallback so the IBL
// binding is always valid (the old flat-ambient look, never black) before any
// source bakes real cubes. An IEnvironmentSource (Stage 2: the sky) then fills
// them through the IBL generation node. The BRDF LUT is environment-independent
// and baked once, shared across all sources.
class ImageBasedLightingFeature final : public IRenderFeature
{
  public:
    // Constructor + destructor are out-of-line so the owned unique_ptr<IEnvironmentSource>
    // member only needs the complete type in the .cpp (not at EnsureFeature sites).
    ImageBasedLightingFeature();
    ~ImageBasedLightingFeature() override;

    // Set-0 layout shared by every environment convolve compute pass (sky and
    // scene-probe sources run the same shaders): a sampled cube (b0) and a
    // writable rgba16f array image (b1). One definition, because the WebGPU
    // annotations (view dimension, storage format) live in the layout and a
    // drifted copy fails only at bind-group creation, only on that backend.
    static const Rendering::DescriptorSetLayoutDesc& ConvolveSet0();

    bool Initialize(GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    // Q6 slice 4 (§8-completion): re-seed the ambient-fallback cubes + BRDF LUT +
    // samplers + EnvData UBOs after a device rebuild (so the world-pass IBL binding
    // is valid on the first resumed frame — never black), and re-arm the bake so the
    // real sky/probe cubes re-convolve over the next frames (IBL visibly pops in).
    void OnDeviceRebuilt(GameEngine::Rendering::IDevice* device) override;

    // Forwards to the active environment source: a scene-capturing source
    // submits its capture-view culling here.
    void OnScheduleCulling(const FeatureCullingContext& ctx) override;

    // Resources bound into the world pass by RenderServices::ResolvePassResources
    // (gated on MaterialKeyword::IBL), keyed by the GLSL names ge_irradianceCube /
    // ge_prefilterCube / ge_brdfLUT / "Env".
    GameEngine::Rendering::TextureHandle GetIrradianceCube() const { return m_Set.diffuseEnvTex; }
    GameEngine::Rendering::TextureHandle GetPrefilterCube() const { return m_Set.specularEnvTex; }
    GameEngine::Rendering::TextureHandle GetBrdfLut() const { return m_Set.brdfLutTex; }
    GameEngine::Rendering::SamplerHandle GetCubeSampler() const { return m_CubeSampler; }
    GameEngine::Rendering::SamplerHandle GetLutSampler() const { return m_LutSampler; }

    // Square edge length defaults and limits for the env-capture / prefilter GPU
    // targets. Reflection probes may request another resolution; the feature
    // resizes the shared bake resources before the probe bake imports them.
    static constexpr uint32_t kDefaultEnvCaptureSize = 256;
    static constexpr uint32_t kMinEnvCaptureSize = 32;
    static constexpr uint32_t kMaxEnvCaptureSize = 1024;
    static constexpr uint32_t kIrradianceSize = 64;
    static constexpr uint32_t kBrdfLutSize = 512;

    // Byte size of the EnvData UBO (std140; mirrored by EnvDataGpu in the .cpp and
    // static_asserted against this). The zero-filled pre-init fallback buffer in
    // RenderServices must be exactly this size — a shorter buffer leaves shader
    // reads of the tail fields (the ambient-floor block) out of bounds.
    static constexpr uint32_t kEnvDataBytes = 224;

    // Full mip chain for the env capture cube. The
    // capture pass renders mip 0; a box-downsample compute fills mips 1..N-1 BEFORE
    // the convolve/prefilter read it. Mip-based filtering (textureLod into this
    // chain) pre-blurs the thin high-frequency horizon glow so the GGX prefilter
    // and cosine convolve never firefly/band, even at low sample counts.
    static constexpr uint32_t kMaxEnvCaptureMipCount = 11;

    // Specular prefilter mip chain length at max resolution. Runtime mip count
    // keeps the final prefilter mip around 16x16, matching the original 256 -> 5
    // mip setup while allowing sharper high-resolution probe captures.
    static constexpr uint32_t kMaxPrefilterMipCount = 7;

    // Highest valid specular prefilter mip INDEX (= mipCount - 1). Single source
    // for the shader's roughness->lod mapping; uploaded as EnvData.prefilterMaxMip.
    uint32_t GetEnvCaptureSize() const { return m_EnvCaptureSize; }
    uint32_t GetPrefilterSize() const { return m_PrefilterSize; }
    uint32_t GetPrefilterMaxMip() const { return m_PrefilterMipCount > 0 ? m_PrefilterMipCount - 1 : 0; }
    uint32_t GetPrefilterMipCount() const { return m_PrefilterMipCount; }
    static uint32_t NormalizeCaptureResolution(uint32_t requested);
    bool EnsureCaptureResolution(uint32_t requested);

    void SetIblIntensity(float v) { m_IblIntensity = v; }
    float GetIblIntensity() const { return m_IblIntensity; }

    // Authored additive ambient irradiance floor (AmbientLight component), pushed each frame by
    // AmbientLightSystem and written into the EnvData UBO by UploadEnvData — the additive analogue
    // of the source's multiplicative AmbientGradientTint, and independent of whether a sky source is
    // active. The default (Mode 0) is a zero floor, so a scene with no AmbientLight is byte-identical
    // to a build without the feature.
    void SetAmbientFloor(const AmbientFloorData& floor) { m_AmbientFloor = floor; }
    const AmbientFloorData& GetAmbientFloor() const { return m_AmbientFloor; }

    // Refresh this device frame's EnvData UBO element from the current scalars
    // (iblIntensity, the prefilter mip cap, the ambient floor, the source's tint and
    // local box projection) and return the buffer to bind. Those scalars are dynamic —
    // the SkyEnvironment slider and the AmbientLight component drive them — so the UBO
    // is a kCaptureFrameSlots-deep ring and the GPU never reads the element the CPU is
    // overwriting; the ordering that rests on is stated at the selection site.
    //
    // The element is not nameable from outside: every consumer binds the handle this
    // returns, so a frame's writer and all its readers agree by construction. Idempotent
    // within a device frame — the IBL gen node, each view's world pass, the ocean and the
    // grass contributor all refresh and bind the same element. Invalid before Initialize.
    GameEngine::Rendering::BufferHandle UploadEnvData(GameEngine::Rendering::IDevice* device);

    // The active environment source (Stage 2: SkyEnvironmentSource). Owned here
    // (engine-shared lifetime) so the bake passes' activation predicates, which
    // capture the source and can outlive the node that created it, never dangle.
    // Null keeps the ambient-fallback cubes (no reflections, no regression).
    void SetEnvironmentSource(std::unique_ptr<IEnvironmentSource> src) { m_Source = std::move(src); }
    void ClearEnvironmentSource() { m_Source.reset(); }
    IEnvironmentSource* GetEnvironmentSource() const { return m_Source.get(); }

    GameEngine::Rendering::IBLSet& GetSet() { return m_Set; }

    // Stage 2 bake resources, written by SkyEnvironmentSource via the IBL gen node:
    // the sky capture cube (sampled by the convolves) + the storage views the
    // convolve / per-mip prefilter compute writes into.
    GameEngine::Rendering::TextureHandle GetEnvCaptureTex() const { return m_EnvCaptureTex; }
    GameEngine::Rendering::TextureViewHandle GetEnvCaptureCubeView() const { return m_EnvCaptureCubeView; }
    uint32_t GetEnvCaptureMipCount() const { return m_EnvCaptureMipCount; }
    // Per-mip View2DArray storage view the box-downsample compute writes mip `mip`
    // into (mip >= 1; mip 0 is rendered by the capture pass, not downsampled).
    GameEngine::Rendering::TextureViewHandle GetEnvCaptureStoreView(uint32_t mip) const
    {
        return mip < m_EnvCaptureMipCount ? m_EnvCaptureStoreViews[mip] : GameEngine::Rendering::TextureViewHandle{};
    }
    // Per-mip ViewCube sampled view of exactly mip `mip` — the box-downsample's
    // SOURCE binding. The downsample must not sample through the whole-chain
    // cube view: its descriptor claims ShaderReadOnly for every mip the view can
    // reach, and the destination mip is simultaneously storage-resident in
    // GENERAL (VUID-VkDescriptorImageInfo-imageLayout-00344). A single-mip view
    // keeps the claim exactly on the subresource the render graph declared Read.
    GameEngine::Rendering::TextureViewHandle GetEnvCaptureMipSampleView(uint32_t mip) const
    {
        return mip < m_EnvCaptureMipCount ? m_EnvCaptureMipSampleViews[mip] : GameEngine::Rendering::TextureViewHandle{};
    }
    GameEngine::Rendering::TextureViewHandle GetIrradianceStoreView() const { return m_IrradianceStoreView; }
    GameEngine::Rendering::TextureViewHandle GetPrefilterStoreView(uint32_t mip) const
    {
        return mip < m_PrefilterMipCount ? m_PrefilterStoreViews[mip] : GameEngine::Rendering::TextureViewHandle{};
    }

    static constexpr uint32_t kNumCaptureFaces = 6;
    // EnvData ring depth. Sized to IDevice::kMaxSupportedFramesInFlight, the
    // backend-independent upper bound on frames in flight, so the ring is valid for the
    // deepest-pacing backend. On a backend that paces fewer frames than that (Vulkan: 3)
    // the surplus element is the write-after-fence margin stated in UploadEnvData.
    static constexpr uint32_t kCaptureFrameSlots =
        GameEngine::Rendering::IDevice::kMaxSupportedFramesInFlight;
    // A power-of-two ring divides 2^32 evenly, so the frame counter's own wrap keeps the
    // rotation seamless rather than replaying an element early.
    static_assert((kCaptureFrameSlots & (kCaptureFrameSlots - 1u)) == 0u,
                  "kCaptureFrameSlots must be a power of two for the EnvData ring to "
                  "survive the frame counter's 2^32 wrap");
    // Digest of the source inputs the irradiance/prefilter cubes were last baked
    // from. The IBL gen node skips the rebake while this matches the source's
    // current InputDigest() (static sky in an idle editor bakes once).
    uint64_t GetLastBakedDigest() const { return m_LastBakedDigest; }
    void SetLastBakedDigest(uint64_t d) { m_LastBakedDigest = d; }

    // Frames since the last actual rebake. The IBL gen node ticks this every frame
    // and throttles the 20-pass rebake during an animated day/night cycle so the
    // low-frequency ambient is not recomputed every frame; a static sky's edit is
    // not throttled (this counter is already saturated). Reset to 0 on each bake.
    // kIblRebakeIntervalFrames is the shared cadence floor both environment
    // sources (sky + scene probe) gate their input-driven rebakes on.
    static constexpr uint32_t kIblRebakeIntervalFrames = 8;
    uint32_t GetFramesSinceBake() const { return m_FramesSinceBake; }
    void TickFramesSinceBake() { if (m_FramesSinceBake < 0xFFFFFFFFu) ++m_FramesSinceBake; }
    void ResetFramesSinceBake() { m_FramesSinceBake = 0; }

    // The split-sum BRDF LUT is environment-independent, so it bakes exactly once.
    bool IsBrdfLutBaked() const { return m_BrdfLutBaked; }
    void MarkBrdfLutBaked() { m_BrdfLutBaked = true; }

  private:
    bool CreateCaptureResources(uint32_t resolution);
    void DestroyCaptureResources();

    bool m_Initialized = false;
    GameEngine::Rendering::IDevice* m_Device = nullptr;

    GameEngine::Rendering::IBLSet m_Set;                   // irradiance + prefilter + BRDF LUT handles
    GameEngine::Rendering::SamplerHandle m_CubeSampler{};  // trilinear, clamp (cube)
    GameEngine::Rendering::SamplerHandle m_LutSampler{};   // bilinear, clamp, no mips (BRDF LUT)
    // EnvData UBO ring, one element per device frame (the scalars in it are dynamic).
    GameEngine::Rendering::BufferHandle m_EnvDataBuffer[kCaptureFrameSlots]{};
    // Unwraps the device's frame index so all kCaptureFrameSlots elements rotate. Lives
    // with the ring it indexes; UploadEnvData is its only ticker.
    GameEngine::Rendering::DeviceFrameCounter m_EnvDataFrameCounter;

    // Sky capture cube + the storage views the convolve compute writes into.
    GameEngine::Rendering::TextureHandle m_EnvCaptureTex{};            // RGBA16F cubemap, full mip chain
    GameEngine::Rendering::TextureViewHandle m_EnvCaptureCubeView{};   // ViewCube (all mips), sampled by the convolves
    // Per-mip View2DArray storage views the box-downsample compute writes (mip 1..N-1).
    GameEngine::Rendering::TextureViewHandle m_EnvCaptureStoreViews[kMaxEnvCaptureMipCount]{};
    // Per-mip ViewCube sampled views the box-downsample reads (mip 0..N-2).
    GameEngine::Rendering::TextureViewHandle m_EnvCaptureMipSampleViews[kMaxEnvCaptureMipCount]{};
    GameEngine::Rendering::TextureViewHandle m_IrradianceStoreView{};  // View2DArray, diffuse convolve writes
    GameEngine::Rendering::TextureViewHandle m_PrefilterStoreViews[kMaxPrefilterMipCount]{}; // per-mip View2DArray
    uint32_t m_EnvCaptureSize = kDefaultEnvCaptureSize;
    uint32_t m_EnvCaptureMipCount = 9;
    uint32_t m_PrefilterSize = kDefaultEnvCaptureSize;
    uint32_t m_PrefilterMipCount = 5;

    float m_IblIntensity = 1.0f;
    AmbientFloorData m_AmbientFloor{}; // default = off (zero floor); byte-identical when unset

    uint64_t m_LastBakedDigest = 0; // 0 == never baked (sentinel; sources avoid 0)
    uint32_t m_FramesSinceBake = 0xFFFFFFFFu; // saturated so the first bake is never throttled
    bool m_BrdfLutBaked = false;

    std::unique_ptr<IEnvironmentSource> m_Source;
};

} // namespace Engine::Renderer
} // namespace GameEngine
