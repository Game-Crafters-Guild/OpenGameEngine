#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;

// Extracts lens flares from ECS each frame: finds LensFlareSource entities,
// resolves their flare definition + atlas texture, projects each source to
// screen space, lays the elements along the optical axis, builds the per-frame
// instance list, and pushes it to the LensFlareRenderFeature. One atlas per
// frame (the first encountered) is supported in v1 — flares sharing it batch
// into a single draw.
class LensFlareExtractionSystem : public ECS::ISystem
{
  public:
    explicit LensFlareExtractionSystem(RenderServices* renderServices);

    const char* GetName() const override { return "LensFlareExtraction"; }
    void Update(ECS::World& world, float32 deltaTime) override;

  private:
    RenderServices* m_RenderServices = nullptr;
    float32 m_Time = 0.0f;
};

} // namespace GameEngine::Engine::Renderer
