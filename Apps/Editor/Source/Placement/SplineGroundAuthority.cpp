#include "Placement/SplineGroundAuthority.h"

#include "Placement/SplineExtrudeSanitize.h"

#include "UndoRedo/SplineGroundCleanupCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Logger/Logger.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineGroundAuthority;

// What one authority resolves to, as components. Written out as data rather than
// as four branches at each write site, so "a road claims and does not defer"
// exists once and the tests can read the same table the controller writes from.
struct GroundPlan
{
    bool GradesGround = false;
    bool Claim = false;
    const char* Group = "";     // empty = the shared pool
    bool RespectClaims = true;
    float32 Priority = kGradedRunPriority;
};

GroundPlan PlanFor(SplineGroundAuthority authority)
{
    switch (authority)
    {
    case SplineGroundAuthority::Grades:
        return {true, false, "", true, kGradedRunPriority};
    case SplineGroundAuthority::Owns:
        // Its own pool, deferring to nothing, below the runs that stop at it.
        // All three are one decision: an owning run that shared the default pool
        // would average its carriageway with every path that crosses it, and one
        // that deferred to claims would defer to its OWN.
        return {true, true, kOwnedRunGroupName, false, kOwnedRunPriority};
    case SplineGroundAuthority::Auto:
    case SplineGroundAuthority::None:
        break;
    }
    return {};
}

void WritePoolGroup(Components::TerrainFlattenEffect& fx, const char* group)
{
    std::memset(fx.PoolGroup, 0, sizeof(fx.PoolGroup));
    const std::size_t length = std::strlen(group);
    std::memcpy(fx.PoolGroup, group, std::min(sizeof(fx.PoolGroup) - 1, length));
}

const char* AuthorityName(SplineGroundAuthority authority)
{
    switch (authority)
    {
    case SplineGroundAuthority::Auto:   return "Auto";
    case SplineGroundAuthority::None:   return "None";
    case SplineGroundAuthority::Grades: return "Grades";
    case SplineGroundAuthority::Owns:   return "Owns";
    }
    return "?";
}

// Writes the provenance ledger back to the entity: present while any piece is
// recipe-created, absent otherwise, so a scene never carries an all-false
// marker block.
void WriteMarker(ECS::World& world, ECS::EntityHandle entity,
                 const Components::SplineGroundProvisioned& owned)
{
    const bool any = owned.Flatten || owned.Claim || owned.Volume;
    const auto* existing = world.GetComponent<Components::SplineGroundProvisioned>(entity);
    if (any)
    {
        if (!existing || !(*existing == owned))
            world.AddComponentImmediate(entity, owned);
    }
    else if (existing)
    {
        world.RemoveComponentImmediate<Components::SplineGroundProvisioned>(entity);
    }
}

} // namespace

void SplineGroundAuthorityController::Update(ECS::World& world, UndoRedoService* undo)
{
    ++m_SweepGeneration;

    // Collected first, applied after: adding or removing a component moves the
    // entity between archetypes, which is not safe during the query that found
    // it.
    struct Pending
    {
        ECS::EntityHandle Entity;
        SplineGroundAuthority Authority;
        Components::SplineExtrude Recipe;
    };
    std::vector<Pending> pending;

    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::SplineExtrude>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent&,
                  const Components::SplineExtrude& recipe) {
            // A run saved with the retired Rectangle profile reads as a path
            // here; grading the ground under what was drawn as a wall is never
            // what its author meant, so it is left alone.
            if (CarriesRetiredWallProfile(world, e))
                return;
            const SplineGroundAuthority resolved = Components::ResolveGroundAuthority(recipe);
            const auto [record, firstSight] = m_Provisioned.try_emplace(e);
            record->second.SeenGeneration = m_SweepGeneration;

            // A scene that just loaded already HAS the effects and this controller
            // has no record of them, so the first sweep ADOPTS rather than
            // rewrites: it records what the recipe resolves to and touches no
            // column at all. Rewriting instead would stamp every run's effect
            // column on load and hand the terrain modifier system a full re-bake
            // of every road in the scene, on every scene open.
            //
            // Adoption records the AUTHORITY only — never provenance. Which pieces
            // the recipe created is the entity's own SplineGroundProvisioned
            // marker, written when a piece is created and serialized with the
            // scene, so it survives the reload this branch exists for.
            if (firstSight && world.GetComponent<Components::TerrainFlattenEffect>(e) != nullptr)
            {
                record->second.Authority = resolved;
                return;
            }
            if (record->second.Authority == resolved)
                return; // Steady state: no write, so no re-bake.

            pending.push_back({e, resolved, recipe});
        });

    for (const Pending& p : pending)
    {
        const GroundPlan plan = PlanFor(p.Authority);

        // A Bevel on the Channel path is a FOOTPATH as far as the recipe can
        // tell, and so is a stream authored at a fixed width. The second one gets
        // graded to its own surface without a word, which is the failure an
        // author cannot see: the water still renders and the bed beneath it is
        // simply gone. Reported where the decision is actually made rather than
        // guessed at in ResolveGroundAuthority. Fires when a run is PROVISIONED,
        // so it lands as the author creates the run and never on scene load,
        // which adopts.
        if (p.Authority == SplineGroundAuthority::Grades
            && p.Recipe.Profile == Components::SplineExtrudeProfile::Bevel
            && p.Recipe.WidthMode == Components::SplineExtrudeWidthMode::Channel
            && p.Recipe.GroundAuthority == SplineGroundAuthority::Auto
            && m_FixedWidthBevelWarned.insert(p.Entity).second)
        {
            Logger::Log::Info(
                "SplineGroundAuthority: entity {} is a fixed-width Bevel run, so Auto grades the "
                "terrain to its own heights, like a path. If this run is WATER rather than a path, "
                "set its Ground Authority to None -- a fixed-width stream left on Auto has its bed "
                "graded away to the water surface.",
                p.Entity.id);
        }

        // The provenance ledger: which of the pieces on this entity the recipe
        // created. It — never this controller's session record — is what
        // separates a flatten the recipe put there from one the author added,
        // and it survives the scene reload the session record does not.
        const auto* markerNow =
            world.GetComponent<Components::SplineGroundProvisioned>(p.Entity);
        Components::SplineGroundProvisioned owned =
            markerNow ? *markerNow : Components::SplineGroundProvisioned{};

        if (!plan.GradesGround)
        {
            // The recipe says it does not touch the ground, so the pieces IT
            // CREATED come off — through the undo journal, and reported. A piece
            // the author placed by hand is not the recipe's to remove, whatever
            // the authority does; the same goes for effects from a scene saved
            // before provenance was recorded, where erring toward an orphaned
            // effect (visible, deletable) beats deleting authored work.
            std::optional<Components::TerrainFlattenEffect> removedFlatten;
            std::optional<Components::TerrainGroundClaimEffect> removedClaim;
            std::optional<Components::TerrainModifierVolume> removedVolume;
            if (owned.Flatten)
                if (const auto* c = world.GetComponent<Components::TerrainFlattenEffect>(p.Entity))
                    removedFlatten = *c;
            if (owned.Claim)
                if (const auto* c =
                        world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity))
                    removedClaim = *c;
            if (owned.Volume)
                if (const auto* c = world.GetComponent<Components::TerrainModifierVolume>(p.Entity))
                    removedVolume = *c;

            if (removedFlatten || removedClaim || removedVolume)
            {
                std::string removed;
                const auto append = [&removed](const char* piece) {
                    if (!removed.empty())
                        removed += ", ";
                    removed += piece;
                };
                if (removedFlatten) append("flatten");
                if (removedClaim)   append("ground claim");
                if (removedVolume)  append("volume");

                auto cmd = std::make_unique<SplineGroundCleanupCommand>(
                    &world, p.Entity, removedFlatten, removedClaim, removedVolume, owned,
                    std::nullopt);
                if (undo)
                    undo->Execute(std::move(cmd));
                else
                    cmd->Do();

                Logger::Log::Info(
                    "SplineGroundAuthority: entity {} no longer grades (authority {}); removed "
                    "the recipe-created {}. Undo restores them; hand-authored components were "
                    "not touched.",
                    p.Entity.id, AuthorityName(p.Authority), removed);
            }
            else if (markerNow)
            {
                // The ledger names pieces that are no longer on the entity (hand-
                // deleted while the authority still graded): only bookkeeping is
                // left to drop.
                world.RemoveComponentImmediate<Components::SplineGroundProvisioned>(p.Entity);
            }

            const bool handPiecesRemain =
                (!owned.Flatten
                 && world.GetComponent<Components::TerrainFlattenEffect>(p.Entity) != nullptr)
                || (!owned.Claim
                    && world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity)
                           != nullptr)
                || (!owned.Volume
                    && world.GetComponent<Components::TerrainModifierVolume>(p.Entity) != nullptr);
            if (handPiecesRemain && m_HandAuthoredConflictWarned.insert(p.Entity).second)
                Logger::Log::Info(
                    "SplineGroundAuthority: entity {}'s authority is {} but it still carries "
                    "terrain effects the recipe did not create (hand-authored, or from a scene "
                    "saved before provenance was recorded). They keep grading; remove them by "
                    "hand if unwanted.",
                    p.Entity.id, AuthorityName(p.Authority));

            m_Provisioned[p.Entity].Authority = p.Authority;
            continue;
        }

        // The volume: created with the run's fade and priority, then left alone.
        // A run's swept half-width comes from the spline's width channel, so the
        // volume carries no width of its own to fall out of step with the mesh.
        bool handVolumeConflict = false;
        if (!world.GetComponent<Components::TerrainModifierVolume>(p.Entity))
        {
            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::SplinePath;
            volume.Falloff = kProvisionedRunFade;
            volume.Priority = plan.Priority;
            world.AddComponentImmediate<Components::TerrainModifierVolume>(p.Entity, volume);
            owned.Volume = true;
        }
        else if (owned.Volume)
        {
            // The authority moved, and the priority is what makes a claim reach
            // the pools that must stop at it — so on the recipe's own volume this
            // one field IS the authority and is rewritten with it. The fade is
            // the author's and is not.
            if (auto* volume =
                    world.GetComponentForWrite<Components::TerrainModifierVolume>(p.Entity))
                volume->Priority = plan.Priority;
        }
        else
        {
            // A hand-authored volume keeps its priority: the recipe does not
            // rewrite what it did not create, even the field it would normally
            // own. Reported below, because claim ordering rides on priority.
            handVolumeConflict = true;
        }

        // Blend travels with the group and the claim flag rather than being left
        // to the author — on the recipe's OWN flatten: Average is what makes the
        // flatten POOL at all, so it is the structural half of the authority.
        // A hand-authored flatten is the author's whole component, blend
        // included; the recipe never rewrites it, and says so instead.
        //
        // The height fields are the author's on either kind, so they are never
        // rewritten and at creation come from the component's own defaults:
        // UseVolumeHeight grades to the spline's own Y at each station, and
        // TargetHeight adds no offset to it. The station spacing that samples
        // those heights is the VOLUME's and defaults with it.
        bool handFlattenConflict = false;
        if (world.GetComponent<Components::TerrainFlattenEffect>(p.Entity) != nullptr)
        {
            if (owned.Flatten)
            {
                if (auto* flatten =
                        world.GetComponentForWrite<Components::TerrainFlattenEffect>(p.Entity))
                {
                    flatten->Blend = Components::TerrainModifierBlend::Average;
                    WritePoolGroup(*flatten, plan.Group);
                    flatten->RespectClaims = plan.RespectClaims;
                }
            }
            else
            {
                handFlattenConflict = true;
            }
        }
        else
        {
            Components::TerrainFlattenEffect fx{};
            fx.Blend = Components::TerrainModifierBlend::Average;
            WritePoolGroup(fx, plan.Group);
            fx.RespectClaims = plan.RespectClaims;
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(p.Entity, fx);
            owned.Flatten = true;
        }

        if (plan.Claim
            && world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity) == nullptr)
        {
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(
                p.Entity, Components::TerrainGroundClaimEffect{});
            owned.Claim = true;
        }

        // Creations recorded before the claim removal below, so the command's
        // before/after marker states are both honest about what is on the entity.
        WriteMarker(world, p.Entity, owned);

        bool handClaimConflict = false;
        if (!plan.Claim)
        {
            // Fetched AFTER the writes above: every AddComponentImmediate is an
            // archetype move, so a pointer taken before one is not a pointer now.
            const auto* claim = world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity);
            if (claim != nullptr && owned.Claim)
            {
                Components::SplineGroundProvisioned after = owned;
                after.Claim = false;
                auto cmd = std::make_unique<SplineGroundCleanupCommand>(
                    &world, p.Entity, std::nullopt, *claim, std::nullopt, owned,
                    (after.Flatten || after.Volume)
                        ? std::optional<Components::SplineGroundProvisioned>(after)
                        : std::nullopt);
                if (undo)
                    undo->Execute(std::move(cmd));
                else
                    cmd->Do();
                owned = after;
                Logger::Log::Info(
                    "SplineGroundAuthority: entity {} moved to authority {}, which keeps the "
                    "grade but does not own the ground; removed the recipe-created ground "
                    "claim. Undo restores it.",
                    p.Entity.id, AuthorityName(p.Authority));
            }
            else if (claim != nullptr)
            {
                handClaimConflict = true;
            }
        }

        if ((handFlattenConflict || handVolumeConflict || handClaimConflict)
            && m_HandAuthoredConflictWarned.insert(p.Entity).second)
        {
            std::string pieces;
            const auto append = [&pieces](const char* piece) {
                if (!pieces.empty())
                    pieces += "; ";
                pieces += piece;
            };
            if (handFlattenConflict)
                append("the flatten keeps its authored blend and pool group (a pooling run "
                       "wants Blend=Average)");
            if (handVolumeConflict)
                append("the volume keeps its authored priority (claim ordering rides on it)");
            if (handClaimConflict)
                append("the ground claim stays");
            Logger::Log::Info(
                "SplineGroundAuthority: entity {}'s authority is {} but some of its terrain "
                "components are hand-authored, and the recipe does not rewrite what it did not "
                "create: {}. Tune the fields yourself, or remove the components to let the "
                "recipe provision its own.",
                p.Entity.id, AuthorityName(p.Authority), pieces);
        }

        m_Provisioned[p.Entity].Authority = p.Authority;
    }

    // Entities that went away between sweeps. Their components went with them, so
    // there is nothing to undo — only the record to drop, or the map grows for
    // the lifetime of the editor session.
    for (auto it = m_Provisioned.begin(); it != m_Provisioned.end();)
        it = (it->second.SeenGeneration == m_SweepGeneration) ? std::next(it)
                                                              : m_Provisioned.erase(it);
}

void SplineGroundAuthorityController::Shutdown()
{
    m_Provisioned.clear();
    m_FixedWidthBevelWarned.clear();
    m_HandAuthoredConflictWarned.clear();
}

} // namespace GameEngine::Editor
