#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;

// Per-frame ECS housekeeping ahead of render-graph declaration: material
// upload, world batch-key build, and frame-buffer finalize. The frame's
// graph itself is declared by the host that owns the per-window RGFrame
// driver (Editor / Player) via RenderServices::BuildFrameGraph(RGFrame&).
//
// SkipFrameGraphBuild is the ownership switch the host sets so exactly one
// recorder declares per window; it is kept because both hosts drive their
// own RGFrame and only need this system's housekeeping.
class RenderGraphBuildSystem : public ECS::ISystem
{
  public:
    explicit RenderGraphBuildSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "RenderGraphBuildSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    void SetSkipFrameGraphBuild(bool skip) { m_SkipFrameGraphBuild = skip; }
    bool GetSkipFrameGraphBuild() const { return m_SkipFrameGraphBuild; }

  private:
    RenderServices* m_RenderServices = nullptr; // non-owning
    bool m_SkipFrameGraphBuild = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
