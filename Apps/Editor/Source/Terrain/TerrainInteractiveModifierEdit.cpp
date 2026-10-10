#include "Terrain/TerrainInteractiveModifierEdit.h"

#include "TerrainECS/TerrainService.h"

namespace GameEngine::Editor
{
namespace
{

// Main-thread only, like the service flag it drives: Inspector callbacks run on
// the UI thread and the modifier system reads the flag in the same frame's
// system pass.
int g_ArmCount = 0;

void SetInspectorDragSource(bool active)
{
    // Null in headless runs and in tests that never build a terrain service. The
    // count is still kept, because the count is what the arms' own contract is
    // written against — a leak has to be observable whether or not a service
    // exists to receive the signal.
    if (auto* service = TerrainECS::TerrainService::TryGet())
    {
        service->SetInteractiveModifierEdit(
            TerrainECS::TerrainService::InteractiveEditSource::InspectorDrag, active);
    }
}

} // namespace

void TerrainInteractiveModifierEditArm::Arm()
{
    if (m_Armed)
        return;
    m_Armed = true;
    if (++g_ArmCount == 1)
        SetInspectorDragSource(true);
}

void TerrainInteractiveModifierEditArm::Release()
{
    if (!m_Armed)
        return;
    m_Armed = false;
    if (--g_ArmCount == 0)
        SetInspectorDragSource(false);
}

TerrainInteractiveModifierEditArm::~TerrainInteractiveModifierEditArm()
{
    Release();
}

int TerrainInteractiveModifierEditArmCount()
{
    return g_ArmCount;
}

} // namespace GameEngine::Editor
