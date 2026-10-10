#include "ECSModules/Rendering/Systems/RenderGraphBuildSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"

#include <cstring>
#include <string>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

using namespace GameEngine::Rendering;

void RenderGraphBuildSystem::Update(ECS::World& /*world*/, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    if (!rs->GetDevice())
        return;

    // Allow examples to fully own the graph (skip system-authored passes/present)
    if (const char* e = std::getenv("GE_EXAMPLE_OWNS_GRAPH"))
    {
        if (e[0] == '1')
        {
            rs->BeginWorldDrawFrame();
            return;
        }
    }

    // Build per-view batch keys from this frame's submissions. The bucketer
    // scheduler and the world / depth-pass execute lambdas both consume this
    // set to issue one indirect draw per (Material, mesh) batch.
    rs->BuildWorldBatchKeys();
    rs->Materials().FinalizeFrameBuffers();

    const auto& views = rs->Views().GetViews();
    if (const char* v = std::getenv("GE_RENDER_DIAG_WORLD_PASSES"); v && !(v[0] == '0' && v[1] == '\0'))
    {
        uint32_t totalWorldBatches = 0;
        for (const auto& view : views)
        {
            const auto keysForView = rs->GetEntityBatchKeys(view.id);
            totalWorldBatches += static_cast<uint32_t>(keysForView.size());
        }
        // Delta-only: only log when total batches or view count changes.
        static uint32_t s_PrevBatches = 0xFFFFFFFFu, s_PrevViews = 0xFFFFFFFFu;
        const uint32_t vCount = static_cast<uint32_t>(views.size());
        if (s_PrevBatches != totalWorldBatches || s_PrevViews != vCount)
        {
            s_PrevBatches = totalWorldBatches;
            s_PrevViews = vCount;
            Logger::Log::Info("RenderGraphBuild: worldBatches={}, views={} (changed)",
                              totalWorldBatches, vCount);
        }
    }

    // The frame's render graph is declared by the host that owns the
    // per-window RGFrame driver (Editor / Player) via
    // RenderServices::BuildFrameGraph(RGFrame&). This system only runs the
    // per-frame housekeeping above; m_SkipFrameGraphBuild is the host's
    // ownership switch (kept on GetSkipFrameGraphBuild) and no longer gates
    // any graph build here.
}

} // namespace Engine::Renderer
} // namespace GameEngine
