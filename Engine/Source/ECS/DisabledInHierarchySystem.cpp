#include "ECS/DisabledInHierarchySystem.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine
{
namespace ECS
{

using GameEngine::Components::Parent;

void DisabledInHierarchySystem::BindTo(World& world)
{
    m_WorldId = world.GetWorldId();
    m_Bound = true;
    m_StructuralVersionValid = false;
    m_ParentGateValid = false;
    m_States.clear();

    // Every pass has to see the rows the enable model hides — they are exactly
    // what it is here to classify.
    m_ParentQuery.emplace(&world);
    m_ParentQuery->IncludeDisabled();
    m_ParentChangeQuery.emplace(&world);
    m_ParentChangeQuery->IncludeDisabled();
    m_DisabledQuery.emplace(&world);
    m_DisabledQuery->IncludeDisabled();
    m_TaggedQuery.emplace(&world);
    m_TaggedQuery->IncludeDisabled();

    AutoComponentRegistrar<Parent>::EnsureRegistered();
    world.SetParentRelation(GetComponentTypeId<Parent>(), static_cast<uint32>(offsetof(Parent, parent)));
}

bool DisabledInHierarchySystem::HasParentColumnChange(uint64 entryVersion)
{
    bool changed = false;
    if (ChangeFilter::Enabled())
    {
        if (!m_ParentGateValid)
            m_ParentGate.LastRunVersion = 0; // see everything once
        m_ParentChangeQuery->Changed<Parent>(m_ParentGate);
        m_ParentChangeQuery->BatchEach([&](const Parent*, std::size_t) { changed = true; });
    }
    else
    {
        // Diagnostic filter-off mode has no per-column stamps to read, so the
        // pass runs whenever anything is disabled at all.
        changed = true;
    }

    m_ParentGate.LastRunVersion = entryVersion;
    m_ParentGateValid = true;
    return changed;
}

DisabledInHierarchySystem::EntityState* DisabledInHierarchySystem::FindState(EntityHandle entity)
{
    if (!entity.IsValid() || entity.index >= m_States.size())
        return nullptr;
    EntityState& state = m_States[entity.index];
    if (state.Stamp != m_RunStamp || state.Entity != entity)
        return nullptr;
    return &state;
}

DisabledInHierarchySystem::EntityState& DisabledInHierarchySystem::TouchState(EntityHandle entity)
{
    EntityState& state = m_States[entity.index];
    if (state.Stamp != m_RunStamp || state.Entity != entity)
    {
        state.Stamp = m_RunStamp;
        state.Entity = entity;
        state.Parent = EntityHandle{};
        state.Flags = 0;
    }
    return state;
}

void DisabledInHierarchySystem::CollectCandidates(World& world)
{
    m_Candidates.clear();
    m_States.resize(world.GetEntityIndexBound());

    const auto addCandidate = [this](EntityState& state, EntityHandle entity)
    {
        if ((state.Flags & kCandidate) == 0)
        {
            state.Flags |= kCandidate;
            m_Candidates.push_back(entity);
        }
    };

    m_ParentQuery->Each(
        [&](EntityHandle entity, const Parent& parent)
        {
            EntityState& state = TouchState(entity);
            state.Parent = parent.parent;
            addCandidate(state, entity);
        });

    m_DisabledQuery->Each(
        [&](EntityHandle entity, const Disabled&)
        {
            EntityState& state = TouchState(entity);
            state.Flags |= kSelfDisabled;
            addCandidate(state, entity);
        });

    m_TaggedQuery->Each(
        [&](EntityHandle entity, const DisabledInHierarchy&)
        {
            EntityState& state = TouchState(entity);
            state.Flags |= kHasTag;
            addCandidate(state, entity);
        });
}

bool DisabledInHierarchySystem::ResolveInactive(EntityHandle entity)
{
    m_Chain.clear();
    bool inactive = false;

    for (EntityHandle current = entity;;)
    {
        EntityState* state = FindState(current);
        if (!state)
            break; // no slot: a root this run never saw, or a stale parent handle
        if ((state->Flags & kResolved) != 0)
        {
            inactive = (state->Flags & kInactive) != 0;
            break;
        }
        if ((state->Flags & kVisiting) != 0)
            break; // a Parent cycle: the entity that closed it counts as a root
        state->Flags |= kVisiting;
        m_Chain.push_back(current.index);
        if ((state->Flags & kSelfDisabled) != 0)
        {
            inactive = true;
            break;
        }
        if (!state->Parent.IsValid())
            break;
        current = state->Parent;
    }

    // Nothing on the chain was self-disabled except possibly its last entry, so
    // the whole chain shares the answer that entry produced.
    for (uint32 index : m_Chain)
    {
        EntityState& state = m_States[index];
        state.Flags &= ~kVisiting;
        state.Flags |= kResolved;
        if (inactive)
            state.Flags |= kInactive;
        else
            state.Flags &= ~kInactive;
    }
    return inactive;
}

void DisabledInHierarchySystem::Update(World& world, float32 /*deltaTime*/)
{
    if (!m_Bound || m_WorldId != world.GetWorldId())
        BindTo(world);

    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const bool structuralChange =
        !m_StructuralVersionValid || m_StructuralVersion != structuralVersion;

    // A reparent is a data write to an existing Parent column, so it never
    // bumps the structural version; the change filter is what catches it.
    const bool parentChange = !structuralChange && HasParentColumnChange(entryVersion);
    if (structuralChange)
    {
        m_ParentGate.LastRunVersion = entryVersion;
        m_ParentGateValid = true;
    }
    if (!structuralChange && !parentChange)
        return;

    m_LastPropagatedCount = 0;
    m_StructuralVersion = structuralVersion;
    m_StructuralVersionValid = true;

    // Nothing disabled anywhere: no tag can be owed and none can be stale.
    // Both counts are archetype-level sums over a cached list.
    if (m_DisabledQuery->Count() == 0 && m_TaggedQuery->Count() == 0)
        return;

    if (++m_RunStamp == 0)
    {
        for (EntityState& state : m_States)
            state.Stamp = 0;
        m_RunStamp = 1;
    }

    ++m_DerivationCount;
    CollectCandidates(world);

    m_ToTag.clear();
    m_ToUntag.clear();
    for (EntityHandle entity : m_Candidates)
    {
        const bool inactive = ResolveInactive(entity);
        EntityState* state = FindState(entity);
        const bool hasTag = state != nullptr && (state->Flags & kHasTag) != 0;
        if (inactive && !hasTag)
            m_ToTag.push_back(entity);
        else if (!inactive && hasTag)
            m_ToUntag.push_back(entity);
    }

    // Mutating archetypes invalidates the iteration above, so it happens only
    // once every pass has read what it needs.
    for (EntityHandle entity : m_ToTag)
        world.AddComponentImmediate<DisabledInHierarchy>(entity, DisabledInHierarchy{});
    for (EntityHandle entity : m_ToUntag)
        world.RemoveComponentImmediate<DisabledInHierarchy>(entity);

    m_LastPropagatedCount = m_ToTag.size() + m_ToUntag.size();

    // The pass just made structural changes of its own; adopt the version they
    // produced so the next update is not woken by this one's work. Those moves
    // also stamped every column of the chunks they touched, Parent included, so
    // the Parent gate is resampled here too. An end-of-run sample is sound only
    // because this pass runs exclusively: nothing else writes Parent while it
    // runs, so no writer's stamp can fall between the sample and the return.
    m_StructuralVersion = world.GetStructuralChangeVersion();
    m_ParentGate.LastRunVersion = world.GetGlobalSystemVersion();
}

} // namespace ECS
} // namespace GameEngine
