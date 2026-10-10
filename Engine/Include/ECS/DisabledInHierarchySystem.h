#pragma once

#include "Components/Hierarchy.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace GameEngine
{
namespace ECS
{

// Derives DisabledInHierarchy — "this entity is off, or an ancestor is" — from
// the authored Disabled tag and the Parent chain. Queries exclude both tags, so
// turning a parent off takes its whole subtree out of every system without any
// of those systems knowing that hierarchies exist.
//
// An entity's own state is immediate: Entity::SetEnabled changes it in one
// archetype move, adding both tags on the way off and removing both on the way
// on unless the parent is still off. This pass owns the derived half, so
// descendants reach their new state on the first update after the change, and
// a world that never runs this system still excludes self-disabled entities —
// just not their children. Binding to a world, it names Parent as the world's
// parent relation (World::SetParentRelation), so switching an entity back on
// while its parent is still off keeps the derived tag instead of dropping it
// until the next pass.
//
// It mutates archetypes, so it runs exclusively within its wave, and it runs at
// all only when a structural change or a Parent edit could have moved the
// answer. With nothing in the world disabled it exits on two archetype-level
// counts.
class DisabledInHierarchySystem : public ISystem
{
  public:
    const char* GetName() const override { return "DisabledInHierarchy"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(World& world, float32 deltaTime) override;

    // Tags added plus tags removed by the last update that ran, and the number
    // of updates that derived at all — the diagnostics the tests assert on.
    std::size_t GetLastPropagatedCount() const { return m_LastPropagatedCount; }
    std::size_t GetDerivationCount() const { return m_DerivationCount; }

  private:
    // One slot per entity index, valid for the run whose stamp it carries.
    // Entity pins the slot to a handle so a recycled index cannot be mistaken
    // for the entity a stale Parent handle names.
    struct EntityState
    {
        uint32 Stamp = 0;
        EntityHandle Entity{};
        EntityHandle Parent{};
        uint32 Flags = 0;
    };

    enum StateFlags : uint32
    {
        kSelfDisabled = 1u << 0,
        kHasTag = 1u << 1,
        kCandidate = 1u << 2,
        kResolved = 1u << 3,
        kInactive = 1u << 4,
        kVisiting = 1u << 5,
    };

    void BindTo(World& world);
    bool HasParentColumnChange(uint64 entryVersion);
    // Fills the per-entity slots from the Parent, Disabled and
    // DisabledInHierarchy columns and returns the entities worth resolving.
    void CollectCandidates(World& world);
    // Walks up the Parent chain, memoizing every entity it passes, so the whole
    // candidate set costs one pass over the edges rather than one per entity.
    bool ResolveInactive(EntityHandle entity);
    EntityState* FindState(EntityHandle entity);
    EntityState& TouchState(EntityHandle entity);

    uint64 m_WorldId = 0;
    bool m_Bound = false;

    std::optional<Query<Read<Components::Parent>>> m_ParentQuery;
    // A Changed<> gate lives on the query object it is set on, so the reparent
    // probe keeps its own instance rather than filtering the collection pass.
    std::optional<Query<Read<Components::Parent>>> m_ParentChangeQuery;
    std::optional<Query<Read<Disabled>>> m_DisabledQuery;
    std::optional<Query<Read<DisabledInHierarchy>>> m_TaggedQuery;

    std::size_t m_StructuralVersion = 0;
    bool m_StructuralVersionValid = false;
    ChangeGate m_ParentGate;
    bool m_ParentGateValid = false;

    // Scratch retained across frames: a steady-state run allocates nothing.
    std::vector<EntityState> m_States;
    std::vector<EntityHandle> m_Candidates;
    std::vector<uint32> m_Chain;
    std::vector<EntityHandle> m_ToTag;
    std::vector<EntityHandle> m_ToUntag;
    uint32 m_RunStamp = 0;
    std::size_t m_LastPropagatedCount = 0;
    std::size_t m_DerivationCount = 0;
};

} // namespace ECS
} // namespace GameEngine
