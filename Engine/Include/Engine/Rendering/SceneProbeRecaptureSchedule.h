#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

class RenderServices;

/// The epochs a scene reflection probe capture can see move between bakes
/// that its own input digest leaves out: moved or edited renderables, shadow
/// casters, lights, dynamic depth (terrain, ocean, grass, playing characters)
/// and material properties. Shader-time animation, playing video textures and
/// blend-only forward producers such as particles move none of them.
struct SceneProbeWorldEpochs
{
    uint64_t RenderContent = 0;
    uint64_t ShadowCasters = 0;
    uint64_t Lights = 0;
    /// Global, not per world: it advances every frame while any depth-writing
    /// producer or dynamic-depth feature is active anywhere.
    uint64_t DynamicDepth = 0;
    /// Global: Material::GetGlobalContentEpoch, moved by any material property
    /// edit or reload. A playing video texture uploads into the texture it is
    /// bound to and does not move it.
    uint64_t Materials = 0;

    bool operator==(const SceneProbeWorldEpochs&) const = default;

    static SceneProbeWorldEpochs Read(const RenderServices& services, uint64_t worldId);
};

/// When a scene reflection probe recaptures. A capture photographs a scene lit
/// by the previous bake, so every change is followed by convergence bakes.
/// Once probes recapture when their inputs change; realtime probes also
/// recapture while the world's epochs move, at most once per interval, and a
/// world at rest is not recaptured once its convergence bakes are done.
class SceneProbeRecaptureSchedule
{
  public:
    static constexpr uint32_t kConvergenceBakes = 2;

    void Tick(float deltaTimeSeconds) { m_SecondsSinceBake += deltaTimeSeconds; }

    /// `inputDigest` is the probe's own inputs (0 = no content: never due).
    /// `realtimeInterval` < 0 marks a Once probe; 0 recaptures every frame.
    bool Due(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs, float realtimeInterval) const;

    /// Records the bake that consumes this frame's state.
    void ConsumeBake(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs, bool realtime);

  private:
    uint64_t m_LastInputDigest = 0;
    SceneProbeWorldEpochs m_LastEpochs{};
    uint32_t m_ConvergenceBakesRemaining = 0;
    float m_SecondsSinceBake = 0.0f;
};

} // namespace Engine::Renderer
} // namespace GameEngine
