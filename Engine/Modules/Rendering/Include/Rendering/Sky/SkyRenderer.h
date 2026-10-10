#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"

#include <cstddef>
#include <filesystem>

namespace GameEngine
{
namespace Rendering
{

struct AtmosphereParametersGPU
{
    float planetRadius;
    float atmosphereRadius;
    float rayleighScaleHeight;
    float mieScaleHeight;
    float betaRayleigh[3];
    float pad0;
    float betaMie[3];
    float mieG;
    float groundAlbedo[3];
    float groundBrightness;
    float groundNightColor[3];
    float groundNightColorStd140Pad;
    float groundHorizonColor[3];
    float groundHorizonColorStd140Pad;
    float groundHorizonNightColor[3];
    // std140 gives every vec3 a full 16-byte slot, so a bare pad must follow each
    // vec3 that is itself followed by another vec3. Omitting this one skewed every
    // field below it by 4 bytes versus the shader's layout.
    float groundHorizonNightColorStd140Pad;
    float nightSkyHorizonColor[3];
    float nightSkyHorizonColorStd140Pad;
    float groundHorizonDayCosWidth;
    float groundHorizonNightCosWidth;
    float belowHorizonBlendSharpness;
    float belowHorizonDarkness;
    float belowHorizonDarkPadding[2];
    // vec3 belowHorizonDarkColor needs a 16-byte boundary; std140 inserts 24 bytes
    // of padding after belowHorizonDarkPadding to reach offset 176.
    float belowHorizonDarkColorStd140PrePad[6];
    float belowHorizonDarkColor[3];
    // Trailing scalar packs into belowHorizonDarkColor's 16-byte slot at offset 188.
    uint32_t belowHorizonMode;
    // PlanetGround aerial-perspective haze strength on its own std140 row at 192.
    float groundHazeStrength;
    float groundHazeStrengthPad[3];
};

static_assert(sizeof(AtmosphereParametersGPU) == 208,
              "AtmosphereParametersGPU must match GLSL AtmosphereUBO std140 layout");
static_assert(offsetof(AtmosphereParametersGPU, groundNightColor) == 64, "AtmosphereUBO groundNightColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonColor) == 80, "AtmosphereUBO groundHorizonColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonNightColor) == 96, "AtmosphereUBO groundHorizonNightColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, nightSkyHorizonColor) == 112, "AtmosphereUBO nightSkyHorizonColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, nightSkyHorizonColorStd140Pad) == 124, "AtmosphereUBO nightSkyHorizonColorStd140Pad offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonDayCosWidth) == 128, "AtmosphereUBO groundHorizonDayCosWidth offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHorizonNightCosWidth) == 132, "AtmosphereUBO groundHorizonNightCosWidth offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonBlendSharpness) == 136, "AtmosphereUBO belowHorizonBlendSharpness offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkness) == 140, "AtmosphereUBO belowHorizonDarkness offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkPadding) == 144, "AtmosphereUBO belowHorizonDarkPadding offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkColorStd140PrePad) == 152,
              "AtmosphereUBO belowHorizonDarkColorStd140PrePad offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonDarkColor) == 176, "AtmosphereUBO belowHorizonDarkColor offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, belowHorizonMode) == 188, "AtmosphereUBO belowHorizonMode offset mismatch");
static_assert(offsetof(AtmosphereParametersGPU, groundHazeStrength) == 192, "AtmosphereUBO groundHazeStrength offset mismatch");

// Camera + celestial + star upload block. Hand-padded so C++ tight-packing
// matches the GLSL SkyUBO std140 layout byte-for-byte (320 B). Shared by the
// sky-view, render and star-billboard passes; the capture pass uses the smaller
// CaptureSkyUbo prefix instead. Keep in lock-step with the SkyUBO block in
// sky_render.frag / sky_stars_billboard.{vert,frag}.
struct SkyUBOGPU
{
    float cameraPosition[3]; float exposureEV;
    float cameraRight[3];    float pad0;
    float cameraUp[3];       float pad1;
    float cameraForward[3];  float skyPan2D;
    float sunDirection[3];   float sunAngularRadius;
    float moonDirection[3];  float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    float sunColor[3];       float sunIntensity;
    float timeOfDayHours;    float skyPad0;
    float viewportAspect;    float tanHalfFovY;
    float viewportResolution[2]; float nightSkyBlend; float skyTimeSeconds;
    float starSizeShape[4];
    float starTwinkleParams[4];
    float starTwinkleAmp[4];
    float starDensityHorizon[4];
    float starSizeRange[4];
    float fallingStarParams[4];
    float fallingStarShapeParams[4];
    // Gradient sky: .rgb = scene-linear premultiplied color, gradientSkyTop.w = mode sentinel
    // (>= 0.5 -> render the gradient instead of the atmosphere). Blended by view-dir.y.
    float gradientSkyTop[4];
    float gradientSkyHorizon[4];
    float gradientSkyBottom[4];
};

static_assert(sizeof(SkyUBOGPU) == 320, "SkyUBOGPU must match GLSL SkyUBO std140 layout");
static_assert(offsetof(SkyUBOGPU, sunColor) == 112, "SkyUBO sunColor offset mismatch");
static_assert(offsetof(SkyUBOGPU, starSizeShape) == 160, "SkyUBO starSizeShape offset mismatch");
static_assert(offsetof(SkyUBOGPU, fallingStarParams) == 240, "SkyUBO fallingStarParams offset mismatch");
static_assert(offsetof(SkyUBOGPU, fallingStarShapeParams) == 256, "SkyUBO fallingStarShapeParams offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyTop) == 272, "SkyUBO gradientSkyTop offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyHorizon) == 288, "SkyUBO gradientSkyHorizon offset mismatch");
static_assert(offsetof(SkyUBOGPU, gradientSkyBottom) == 304, "SkyUBO gradientSkyBottom offset mismatch");

struct StarInstanceGPU
{
    float dirAndBrightness[4];
    float properties[4];
};

// Engine-owned moon art, relative to the install assets root. The Editor's and Player's
// asset staging copy the repo's Assets/Textures tree next to the executable, so this is a
// staged-tree path and never a repo path.
inline constexpr const char* kSkyMoonFullTextureRelativePath = "Textures/Sky/MoonPhases/moon_full.png";

class SkyRenderer
{
  public:
    struct Config
    {
        uint32_t skyViewLutWidth = 256;
        uint32_t skyViewLutHeight = 256;

        uint32_t transmittanceLutWidth = 256;
        uint32_t transmittanceLutHeight = 64;

        // Hillaire multiscatter LUT is a coarse 2D table (sunCosZenith x height);
        // 32x32 is the production size — the data is very low-frequency.
        uint32_t multiscatterLutSize = 32;

        uint32_t maxStars = 8000;

        // Where the host staged the engine assets (PathUtils::GetInstallAssetsRoot()).
        // The renderer does not derive it: install layout is the host's concern, and this
        // module has no dependency that can answer it. Empty leaves engine-owned sky art
        // unresolvable, which the load path reports rather than silently substituting.
        std::filesystem::path installAssetsRoot;
    };

    // Set0 binding indices for each sky pipeline, resolved from the shaderpkg's
    // reflected meta by name at CreatePipelines. Both this class's pipeline
    // layouts and SkyRenderNode's transient descriptor sets are built from these
    // (single source of truth), so a shader layout edit stays in lockstep with
    // the C++ binds. Defaults are the GLSL literals used as a reflection-miss
    // fallback. Only the INDEX comes from reflection; descriptor types stay
    // explicit (the reflected type enum is unreliable for images).
    struct SkyBindingSlots
    {
        struct
        {
            uint32_t TransLUT = 0; // uTransLUT (storage image)
            uint32_t Atmos = 1;    // uAtmos (UBO)
        } Transmittance;
        struct
        {
            uint32_t MultiscatterLUT = 0;  // oMultiscatterLUT (storage image)
            uint32_t TransmittanceLUT = 1; // uTransmittanceLUT (sampler)
            uint32_t Atmos = 2;            // uAtmos (UBO)
        } Multiscatter;
        struct
        {
            uint32_t SkyViewLUT = 0;       // oSkyViewLUT (storage image)
            uint32_t TransmittanceLUT = 1; // uTransmittanceLUT (sampler)
            uint32_t Atmos = 2;            // uAtmos (UBO)
            uint32_t Sky = 3;              // uSky (UBO)
            uint32_t MultiscatterLUT = 4;  // uMultiscatterLUT (sampler)
        } SkyViewLut;
        struct
        {
            uint32_t Sky = 0;        // uSky (UBO)
            uint32_t SkyViewLUT = 1; // uSkyViewLUT (sampler)
            uint32_t Atmos = 2;      // uAtmos (UBO)
            uint32_t TransLUT = 3;   // uTransLUT (sampler)
            uint32_t MoonFull = 4;   // uMoonFullTexture (sampler)
        } SkyRender;
        struct
        {
            uint32_t Stars = 0; // sb (SSBO)
            uint32_t Sky = 1;   // uSky (UBO)
        } StarBillboard;
    };

    SkyRenderer();
    ~SkyRenderer();

    bool Initialize(IDevice* device, const Config& config);

    // Q6 slice 4 (§8-completion): after an in-place device rebuild, the LUT
    // textures, samplers, star SSBO and per-frame UBOs are dead but their handles
    // still read IsValid(). Re-run Initialize (which overwrites every handle with a
    // fresh resource — no DestroyTexture/Buffer on the dead ones, so no double-free)
    // and reset the one-shot LUT-compute flags so the transmittance / sky-view /
    // multiscatter LUTs recompute into the fresh textures. The lazily-loaded moon
    // texture is zeroed so its getter reloads it.
    void ReprovisionAfterDeviceRebuild();

    struct CameraState
    {
        float position[3] = {0, 0, 0};
        // LH +Z. Live sky does not read this (SkyRenderNode::ExtractCameraVectors
        // copies view row 2). The default must still match that convention so a
        // future SetCamera caller does not inherit a right-handed look.
        float forward[3] = {0, 0, 1};
        float up[3] = {0, 1, 0};
        float right[3] = {1, 0, 0};
        float fovYDegrees = 60.0f;
        float aspect = 1.777f;
        uint32_t viewportWidth = 1280;
        uint32_t viewportHeight = 720;
    };

    void SetCamera(const CameraState& camera);

    void Update(const SkySettings& settings, const SkySystemState& state);

    void InvalidateTransmittance();

    static void FillDefaultAtmosphere(AtmosphereParametersGPU& atmo);

    IDevice* GetDevice() const { return m_Device; }
    const Config& GetConfig() const { return m_Config; }
    TextureHandle GetTransmittanceLutTexture() const { return m_TransmittanceLutTex; }
    TextureHandle GetSkyViewLutTexture() const { return m_SkyViewLutTex; }
    TextureHandle GetMultiscatterLutTexture() const { return m_MultiscatterLutTex; }
    TextureHandle EnsureMoonFullTexture();
    SamplerHandle GetLinearSampler() const { return m_LinearSampler; }
    // Sampler for the sky-view LUT reads only: Repeat on u so the full-circle
    // azimuth wraps seamlessly across u=0/1, ClampToEdge on v for the elevation
    // poles. Other LUT reads (transmittance, multiscatter, moon) keep using the
    // fully-clamped m_LinearSampler.
    SamplerHandle GetSkyViewSampler() const { return m_SkyViewSampler; }
    // Lazy-intern the compute / graphics pipeline ids on first call. Subsequent
    // calls return the cached id. Thread-safe: intern is idempotent across
    // callers via the device's pipeline cache.
    ComputePipelineId  EnsureTransmittanceId(IDevice& device);
    ComputePipelineId  EnsureMultiscatterId(IDevice& device);
    ComputePipelineId  EnsureSkyViewLutId(IDevice& device);
    GraphicsPipelineId EnsureSkyRenderId(IDevice& device);
    GraphicsPipelineId EnsureStarBillboardId(IDevice& device);
    BufferHandle GetStarSSBO() const { return m_StarSSBO; }
    uint32_t GetStarInstanceCount(float density) const;
    bool IsTransmittanceComputed() const { return m_TransmittanceComputed; }
    void MarkTransmittanceComputed() { m_TransmittanceComputed = true; }

    // Sky-view LUT dirty tracking. The LUT is a function of SkySettings
    // only (sun direction, atmosphere, etc. — NOT camera). The pipeline
    // node computes a digest of the relevant settings each frame and
    // calls IsSkyViewLutCurrentFor() in its activation predicate; if the
    // digest matches the last-computed value, the dispatch is skipped.
    // MarkSkyViewLutComputed() is called from the execute lambda after
    // a successful dispatch.
    bool IsSkyViewLutCurrentFor(uint64_t inputDigest) const
    {
        return m_SkyViewLutComputedOnce && m_SkyViewLutInputDigest == inputDigest;
    }
    void MarkSkyViewLutComputed(uint64_t inputDigest)
    {
        m_SkyViewLutInputDigest = inputDigest;
        m_SkyViewLutComputedOnce = true;
    }
    // The digest of the sky state the LUT currently CONTAINS (stamped after the
    // compute executes; 0 until first computed). Consumers that capture the LUT
    // (probe bakes) fold this into their own input digests so a capture
    // triggered on the frame the settings change re-triggers once the LUT
    // catches up — instead of silently keeping the previous sky forever.
    uint64_t GetSkyViewLutComputedDigest() const
    {
        return m_SkyViewLutComputedOnce ? m_SkyViewLutInputDigest : 0;
    }

    // Multiscatter LUT dirty tracking. Shares the sky-view input digest (the
    // multiscatter LUT is a function of the same atmosphere + sun-zenith
    // parameterization). Gated independently so the three-pass chain
    // (transmittance -> multiscatter -> sky-view) re-runs together on a change.
    bool IsMultiscatterCurrentFor(uint64_t inputDigest) const
    {
        return m_MultiscatterComputedOnce && m_MultiscatterInputDigest == inputDigest;
    }
    void MarkMultiscatterComputed(uint64_t inputDigest)
    {
        m_MultiscatterInputDigest = inputDigest;
        m_MultiscatterComputedOnce = true;
    }

    const SkySettings& GetSettings() const { return m_Settings; }
    const SkySystemState& GetSystemState() const { return m_State; }
    const CameraState& GetCamera() const { return m_Camera; }

    // Reflected set0 binding indices shared with SkyRenderNode (see SkyBindingSlots).
    const SkyBindingSlots& GetBindingSlots() const { return m_Bindings; }

    uint32_t GetFrameSlot(IDevice* device) const;

    BufferHandle GetSkyViewLutSkyUBO(uint32_t slot) const { return m_SkyViewLutSkyUBO[slot]; }
    BufferHandle GetSkyViewLutAtmoUBO(uint32_t slot) const { return m_SkyViewLutAtmoUBO[slot]; }
    BufferHandle GetMultiscatterAtmoUBO(uint32_t slot) const { return m_MultiscatterAtmoUBO[slot]; }
    BufferHandle GetSkyRenderUBO(uint32_t slot) const { return m_SkyRenderUBO[slot]; }
    BufferHandle GetTransmittanceAtmoUBO(uint32_t slot) const { return m_TransmittanceAtmoUBO[slot]; }
    BufferHandle GetRenderAtmoUBO(uint32_t slot) const { return m_RenderAtmoUBO[slot]; }

  private:
    IDevice* m_Device = nullptr;
    Config m_Config;
    SkyBindingSlots m_Bindings;

    TextureHandle m_TransmittanceLutTex;
    TextureHandle m_SkyViewLutTex;
    TextureHandle m_MultiscatterLutTex;
    TextureHandle m_MoonFullTexture;
    SamplerHandle m_LinearSampler;
    SamplerHandle m_SkyViewSampler;

    // Source PipelineDescs kept as shader/state holders during pipeline
    // construction; descs are translated into interned compute/graphics ids
    // on first Execute and the resulting ids drive lookup thereafter.
    PipelineDesc m_TransmittancePipe;
    PipelineDesc m_MultiscatterPipe;
    PipelineDesc m_SkyViewLutPipe;
    PipelineDesc m_SkyRenderPipe;
    PipelineDesc m_StarBillboardPipe;

    ComputePipelineId  m_TransmittanceId{};
    ComputePipelineId  m_MultiscatterId{};
    ComputePipelineId  m_SkyViewLutId{};
    GraphicsPipelineId m_SkyRenderId{};
    GraphicsPipelineId m_StarBillboardId{};
    BufferHandle m_StarSSBO;
    uint32_t m_StarCount = 0;

    static constexpr uint32_t kMaxFramesInFlight = IDevice::kMaxSupportedFramesInFlight;
    uint32_t m_FramesInFlight = 1;

    BufferHandle m_SkyViewLutSkyUBO[kMaxFramesInFlight] = {};
    BufferHandle m_SkyViewLutAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_MultiscatterAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_SkyRenderUBO[kMaxFramesInFlight] = {};
    BufferHandle m_TransmittanceAtmoUBO[kMaxFramesInFlight] = {};
    BufferHandle m_RenderAtmoUBO[kMaxFramesInFlight] = {};

    bool m_TransmittanceComputed = false;
    uint64_t m_SkyViewLutInputDigest = 0;
    bool m_SkyViewLutComputedOnce = false;
    uint64_t m_MultiscatterInputDigest = 0;
    bool m_MultiscatterComputedOnce = false;

    bool CreatePipelines();
    void GenerateStarCatalog();
    void DestroyResources();

    SkySettings m_Settings;
    SkySystemState m_State;
    CameraState m_Camera;
};

} // namespace Rendering
} // namespace GameEngine
