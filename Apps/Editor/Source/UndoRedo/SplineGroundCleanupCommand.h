#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <optional>

namespace GameEngine::Editor
{

// Removes the recipe-created ground components under a generated run when its
// resolved authority stops covering them, and restores them — with their
// provenance marker — on undo. Only pieces the provisioning itself created ride
// in here (SplineGroundProvisioned is the ledger); a hand-authored component is
// never captured, so undo/redo can never touch one.
class SplineGroundCleanupCommand final : public IEditorCommand
{
public:
    SplineGroundCleanupCommand(ECS::World* world, ECS::EntityHandle entity,
                               std::optional<Components::TerrainFlattenEffect> flatten,
                               std::optional<Components::TerrainGroundClaimEffect> claim,
                               std::optional<Components::TerrainModifierVolume> volume,
                               Components::SplineGroundProvisioned markerBefore,
                               std::optional<Components::SplineGroundProvisioned> markerAfter)
        : m_World(world)
        , m_Entity(entity)
        , m_Flatten(flatten)
        , m_Claim(claim)
        , m_Volume(volume)
        , m_MarkerBefore(markerBefore)
        , m_MarkerAfter(markerAfter)
    {
    }

    const char* GetName() const override { return "Remove Recipe Ground Effects"; }
    const char* GetTypeName() const override { return "SplineGroundCleanupCommand"; }

    void Do() override
    {
        if (!m_World || !m_World->IsValid(m_Entity))
            return;
        if (m_Flatten && m_World->GetComponent<Components::TerrainFlattenEffect>(m_Entity))
            m_World->RemoveComponentImmediate<Components::TerrainFlattenEffect>(m_Entity);
        if (m_Claim && m_World->GetComponent<Components::TerrainGroundClaimEffect>(m_Entity))
            m_World->RemoveComponentImmediate<Components::TerrainGroundClaimEffect>(m_Entity);
        if (m_Volume && m_World->GetComponent<Components::TerrainModifierVolume>(m_Entity))
            m_World->RemoveComponentImmediate<Components::TerrainModifierVolume>(m_Entity);

        if (m_MarkerAfter)
            m_World->AddComponentImmediate(m_Entity, *m_MarkerAfter);
        else if (m_World->GetComponent<Components::SplineGroundProvisioned>(m_Entity))
            m_World->RemoveComponentImmediate<Components::SplineGroundProvisioned>(m_Entity);
    }

    void Undo() override
    {
        if (!m_World || !m_World->IsValid(m_Entity))
            return;
        if (m_Flatten)
            m_World->AddComponentImmediate(m_Entity, *m_Flatten);
        if (m_Claim)
            m_World->AddComponentImmediate(m_Entity, *m_Claim);
        if (m_Volume)
            m_World->AddComponentImmediate(m_Entity, *m_Volume);
        m_World->AddComponentImmediate(m_Entity, m_MarkerBefore);
    }

private:
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity;
    std::optional<Components::TerrainFlattenEffect> m_Flatten;
    std::optional<Components::TerrainGroundClaimEffect> m_Claim;
    std::optional<Components::TerrainModifierVolume> m_Volume;
    Components::SplineGroundProvisioned m_MarkerBefore;
    std::optional<Components::SplineGroundProvisioned> m_MarkerAfter;
};

} // namespace GameEngine::Editor
