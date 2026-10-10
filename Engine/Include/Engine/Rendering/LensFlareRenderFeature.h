#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Assets/LensFlareTypes.h" // LensFlare::FlareGlobals
#include "Mathematics/Curve.h"     // Math::CurveKey
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{
// std430 mirror of FlareInstance in lens_flare.vert (4 vec4 = 64 bytes). The
// two basis vectors describe a pixel-correct, camera-facing quad directly in
// NDC, avoiding distortion when a non-square flare element is rotated.
struct alignas(16) FlareInstanceGPU
{
    float PosBasisX[4]{0, 0, 0, 0};  // xy center NDC, zw local-X half-vector
    float BasisYProbe[4]{0, 0, 0, 0}; // xy local-Y half-vector, zw source UV
    float ColorDepth[4]{1, 1, 1, -1}; // rgb premultiplied fades, w source depth
    float UVRect[4]{0, 0, 1, 1};      // xy uv min (top-left), zw uv size
};
static_assert(sizeof(FlareInstanceGPU) == 64, "FlareInstanceGPU must be 64 bytes (std430)");

// One atlas-resolved element of a flare. View-independent: the extraction system
// resolves the atlas sprite UV + per-element params once per frame; the render
// node projects/sizes/rotates it per view.
struct ResolvedFlareElement
{
    float UVRect[4]{0, 0, 1, 1}; // atlas uv min + size
    float Tint[4]{1, 1, 1, 1};
    float Brightness = 1.0f;
    float Scale = 1.0f;
    float SizeX = 1.0f;
    float SizeY = 1.0f;
    float Position = 0.0f;
    float OffsetX = 0.0f;
    float OffsetY = 0.0f;
    float AnamorphicX = 0.0f;
    float AnamorphicY = 0.0f;
    float Angle = 0.0f;
    float RotationSpeed = 0.0f;
    // Per-element dynamic-boost coefficients; LensFlare::kInheritGlobalBoost =
    // use the flare's global (negative overrides are valid authored values).
    float EdgeBrightnessBoost = LensFlare::kInheritGlobalBoost;
    float CenterBrightnessBoost = LensFlare::kInheritGlobalBoost;
    float EdgeScaleBoost = LensFlare::kInheritGlobalBoost;
    float CenterScaleBoost = LensFlare::kInheritGlobalBoost;
    bool UseStarRotation = false;
    bool RotateToFlare = false;
};

// A flare anchored in the world, atlas + globals resolved. View-independent; the
// node projects it with each view's camera (so multi-view is correct).
struct ResolvedFlare
{
    ::GameEngine::Rendering::TextureHandle Atlas{};
    bool AtlasHasAuthoredAlpha = false;
    float WorldPos[3]{0, 0, 0};
    float Forward[3]{0, 0, 1};   // source entity's world forward (angle limit/fade)
    float TransformScale = 1.0f; // source entity's world scale (X axis)
    bool SunMode = false;        // project Forward as an infinite directional source
    float Intensity = 1.0f;
    float Scale = 1.0f;
    float Tint[4]{1, 1, 1, 1};       // per-source tint multiplier
    float MaxDistanceOverride = 0.0f; // > 0 replaces the definition's max distance
    bool Occlude = true;
    LensFlare::FlareGlobals Globals{};
    // Authored curves converted to evaluable keys (empty = consumer default).
    std::vector<::GameEngine::Math::CurveKey> AngleCurve;
    std::vector<::GameEngine::Math::CurveKey> DynamicEdgeCurve;
    std::vector<ResolvedFlareElement> Elements;
};

// Owns the GPU resources for screen-space lens flares: the additive instanced
// quad pipeline, the atlas sampler, and per-view rings of instance storage
// buffers. The extraction system pushes view-independent resolved flares via
// SetFlares; LensFlareRenderNode projects + draws them per view.
class LensFlareRenderFeature : public IRenderFeature
{
  public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    // Q6 slice 4 (§8-completion): the per-view instance rings are persistently
    // MAPPED; an in-place device rebuild frees them and their mapped pointers dangle,
    // so UploadInstances' next UpdateBuffer would be a use-after-free WRITE. Drop the
    // rings so they re-create + re-map, and recreate the eager atlas sampler
    // (Initialize early-returns on m_Initialized and won't).
    void OnDeviceRebuilt(::GameEngine::Rendering::IDevice* device) override;

    // View-independent per-frame data, set by the extraction system.
    void SetFlares(std::vector<ResolvedFlare> flares);
    void SetTime(float time) { m_Time = time; }
    void ClearFrameData();

    bool HasFlares() const { return !m_Flares.empty(); }
    const std::vector<ResolvedFlare>& GetFlares() const { return m_Flares; }
    float GetTime() const { return m_Time; }

    // Upload one view's instances into that view's ring buffer for `frameIndex`.
    // Returns the buffer (instance count via outCount). Invalid on failure.
    ::GameEngine::Rendering::BufferHandle UploadInstances(
        ::GameEngine::Rendering::IDevice& device, uint32_t viewId, uint32_t frameIndex,
        const std::vector<FlareInstanceGPU>& instances, uint32_t& outCount);

    ::GameEngine::Rendering::GraphicsPipelineId GetPipelineId() const { return m_PipelineId; }
    const ::GameEngine::Rendering::DescriptorSetLayoutDesc& GetLayout() const { return m_Layout; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    // Set0 binding indices resolved from lens_flare's reflected meta by name at
    // Initialize (fallback to the GLSL literals on a reflection miss). The render
    // node reads these instead of hard-coded 0/1/2 so a shader layout edit stays
    // in lockstep with the C++ binds.
    uint32_t GetAtlasBinding() const { return m_AtlasBinding; }       // uAtlas
    uint32_t GetInstanceBinding() const { return m_InstanceBinding; } // Instances (SSBO)
    uint32_t GetDepthBinding() const { return m_DepthBinding; }       // uSceneDepth

  private:
    bool m_Initialized = false;

    ::GameEngine::Rendering::PipelineDesc m_Pipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler{};
    ::GameEngine::Rendering::IDevice* m_Device = nullptr;

    // Reflected set0 binding indices (default to the GLSL literals as fallback).
    uint32_t m_AtlasBinding = 0u;
    uint32_t m_InstanceBinding = 1u;
    uint32_t m_DepthBinding = 2u;

    // Sized from the device's frames-in-flight so a slot is never reused while the
    // GPU may still be reading it.
    uint32_t m_FramesInFlight = 1;
    struct ViewRing
    {
        std::vector<::GameEngine::Rendering::BufferHandle> Buffers;
        std::vector<uint32_t> Capacity;
    };
    std::unordered_map<uint32_t, ViewRing> m_ViewRings;

    std::vector<ResolvedFlare> m_Flares;
    float m_Time = 0.0f;
};

} // namespace GameEngine::Engine::Renderer
