#pragma once

#include "Ocean/OceanShapeSampleInputs.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Core/Device.h" // PipelineDesc, DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h" // GraphicsPipelineId

#include <array>
#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Ocean
{

/// Floor on the measured reflected-caustics displacement bound (meters), so a
/// glassy readback still leaves the band room for the shader's surface.
inline constexpr float kReflectedCausticsMeasuredBoundFloor = 0.25f;

/// Vertical distance from SeaLevel within which the reflected-caustics pass
/// treats the displaced surface as lying; receivers outside [SeaLevel - bound,
/// SeaLevel + bound + ReflectedCausticsHeight] are never lit. Gerstner mode: the
/// summed wave amplitudes (exact). FFT mode: twice the measured recent maximum
/// (`measuredFFTMaxVertical`, from the height-field readback; the margin covers
/// its 2 m spacing and the long cascades it sees in part), floored at
/// kReflectedCausticsMeasuredBoundFloor and
/// capped at the clamp (MaxVerticalDisplacement); the clamp itself when nothing
/// was measured.
float ReflectedCausticsSurfaceDisplacementBound(const OceanParamsGPU& params,
                                                const float* measuredFFTMaxVertical);

/// The shader traces a receiver back to the surface along the reflected sun ray,
/// whose rise it clamps to at least this slope (it reads the value from the
/// params, uOcean.w), so a lit receiver sits at most (height above the surface) /
/// kReflectedCausticsMinRise metres sideways of the water it reflects.
inline constexpr float kReflectedCausticsMinRise = 0.15f;

// Above-water reflected caustics from the ocean surface. The historical class name
// remains for now because reflected caustics reused the old fullscreen pass.
// Mirrors OceanUnderwater's structure: a render-graph copy of the scene colour,
// then a fullscreen composite reading that copy and writing SceneColor.
// Declines cleanly if SceneColor or the program can't be resolved.
class OceanLightShafts
{
public:
    ~OceanLightShafts();

    // Schedules the reflected-caustics pass for this view at the post phase.
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, const OceanParamsGPU& params,
                        const OceanUnderwaterSettings& settings,
                        const OceanShapeSampleInputs& shape,
                        ::GameEngine::Rendering::TextureHandle caustics,
                        ::GameEngine::Rendering::SamplerHandle causticsSampler,
                        float surfaceDisplacementBound);

    bool IsReady() const { return m_PipelineId.IsValid(); }

private:
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;

    // Composite program (runtime-compiled vs+fs): scene snapshot -> scene + caustics.
    ::GameEngine::Rendering::PipelineDesc m_Pipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};

    // Scene-copy program (stock fullscreen copy.shaderpkg).
    ::GameEngine::Rendering::PipelineDesc m_CopyPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CopyPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CopyLayout{};

    // MSAA-resolve program (sampler2DMS averaging) for the scene copy when
    // SceneColor is multisampled.
    ::GameEngine::Rendering::PipelineDesc m_ResolvePipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_ResolvePipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_ResolveLayout{};

    bool m_PipelineLoadAttempted = false;
    bool m_WarnedLoadFailed = false;

    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    ::GameEngine::Rendering::TextureHandle m_DummyArrayTexture;
    // 1x1 zero 2D texture: fallback for the depth/caustics sampler2D bindings
    // when their source declines (the shader gates use via the params UBO).
    ::GameEngine::Rendering::TextureHandle m_DummyTexture2D;
};

} // namespace GameEngine::Ocean
