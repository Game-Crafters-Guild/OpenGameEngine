#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "CBTTerrain/SphereSculptLayer.h"
#include "TerrainECS/TerrainService.h"

#include <utility>
#include <vector>

namespace GameEngine::Editor
{

// One undo entry for a whole planet (sphere) sculpt brush stroke — the sphere sibling of
// TerrainZoneStrokeCommand. The payload is the touched sculpt pages' dab-layer pre/post images,
// captured lazily by SphereSculptLayer's stroke capture (only pages the stroke actually wrote,
// never the virtual face area), so a small stroke on a big planet stays memory-bounded.
//
// Restore goes through TerrainService::RestoreSphereSculptPages, which re-enters the exact
// edit-update pipeline an interactive dab drives: sculpt version advance, the per-face physics +
// render dirty unions, and the edit-driven re-tessellation / forced VertexEval those feed — an
// undo IS an edit. The sphere brush edits the service-owned sculpt layer (no zone entity), so
// unlike the planar stroke there is no component/transform revert and no structure notification.
class SphereSculptStrokeCommand final : public IEditorCommand
{
public:
    SphereSculptStrokeCommand(TerrainECS::TerrainService* service,
                              std::vector<CBTTerrain::SphereSculptPageState> before,
                              std::vector<CBTTerrain::SphereSculptPageState> after)
        : m_Service(service), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    const char* GetName() const override { return "Planet Sculpt Stroke"; }
    const char* GetTypeName() const override { return "SphereSculptStrokeCommand"; }

    void Do() override { Redo(); }
    void Redo() override
    {
        if (m_Service)
            m_Service->RestoreSphereSculptPages(m_After);
    }
    void Undo() override
    {
        if (m_Service)
            m_Service->RestoreSphereSculptPages(m_Before);
    }

private:
    TerrainECS::TerrainService* m_Service = nullptr; // not owned
    std::vector<CBTTerrain::SphereSculptPageState> m_Before;
    std::vector<CBTTerrain::SphereSculptPageState> m_After;
};

} // namespace GameEngine::Editor
