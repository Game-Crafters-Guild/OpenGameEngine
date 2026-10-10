#include "Inspectors/SplineFenceInspector.h"

#include "InspectorRegistry.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineFenceOverrideFields.h"
#include "Placement/FenceSpanOverrides.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Components/Hierarchy.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Core/Engine.h"
#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::kSplineFencePoolCapacity;
using Components::SplineFence;
using PoolArray = Components::ModelRef[kSplineFencePoolCapacity];

// Write one slot, then compact the pool so active entries stay a dense prefix.
// Compaction is what keeps the selection contract (leading non-empty slots) and
// the row layout in agreement after a clear — and it heals any hole a
// hand-edited scene carried the moment the pool is touched here.
void SetPoolSlotCompacted(PoolArray& pool, uint32 slot, const GUID& guid)
{
    if (slot >= kSplineFencePoolCapacity)
        return;
    pool[slot] = Components::ModelRef(guid);
    Components::ModelRef compacted[kSplineFencePoolCapacity];
    uint32 n = 0;
    for (const Components::ModelRef& m : pool)
    {
        if (!m.IsNull())
            compacted[n++] = m;
    }
    for (uint32 i = 0; i < kSplineFencePoolCapacity; ++i)
        pool[i] = i < n ? compacted[i] : Components::ModelRef{};
}

// Pool row counts change with their content, so a commit re-renders this
// component's section. The controller needs no signal — it observes the recipe
// bytes each frame.
void NotifyInspectorRebuild(ECS::World* w, ECS::EntityHandle e,
                            Editor::EditorChangeNotifications* n)
{
    if (!n)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = w;
    rebuild.entity = e;
    rebuild.componentType = ECS::GetComponentTypeId<SplineFence>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    n->NotifyComponentChanged(rebuild);
}

void BuildPoolRows(const InspectorContext& ctx, const std::string& poolLabel,
                   PoolArray SplineFence::* poolMember, const char* tooltip)
{
    auto* comp = ctx.World->GetComponent<SplineFence>(ctx.Entity);
    if (!comp)
        return;
    const PoolArray& pool = comp->*poolMember;
    const uint32 active = Components::ActivePoolCount(pool);
    const bool hole = Components::PoolHasHole(pool);

    // With a hole, show every slot so the ignored tail entries are visible and
    // fixable; otherwise the dense prefix plus one empty add row.
    const uint32 rows = hole ? kSplineFencePoolCapacity
                             : std::min(active + 1u, kSplineFencePoolCapacity);

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const std::string undoName = "Change " + poolLabel;

    for (uint32 slot = 0; slot < rows; ++slot)
    {
        InspectorUI::AddAssetFieldRow(
            ctx.Parent, poolLabel + " " + std::to_string(slot + 1), pool[slot].ToGuid(),
            {AssetType::Model}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
            [w, e, n, undo, undoName, poolMember, slot](const GUID& picked)
            {
                InspectorDrag::CommitComponentWithUndo<SplineFence>(
                    w, e, n, undo, undoName,
                    [poolMember, slot, picked](SplineFence& u)
                    { SetPoolSlotCompacted(u.*poolMember, slot, picked); });
                NotifyInspectorRebuild(w, e, n);
            },
            ctx.Thumbnails, tooltip);
    }

    if (active == kSplineFencePoolCapacity)
    {
        InspectorUI::AddInfoCard(ctx.Parent, poolLabel + " is full (" +
                                                 std::to_string(kSplineFencePoolCapacity) + "/" +
                                                 std::to_string(kSplineFencePoolCapacity) +
                                                 ") — capacity is fixed");
    }
    if (hole)
    {
        InspectorUI::AddInfoCard(ctx.Parent,
                                 poolLabel + " has an empty slot before a mesh — entries after "
                                             "the first empty slot are ignored; re-assign any "
                                             "slot to compact");
    }
}

// The spans a fence's own pieces stand in, as (point, ordinal): an override
// naming a span no piece stands in places nothing, and its row says so. Read
// once per inspector build from the pieces the fence placed.
std::vector<std::pair<uint32, uint32>> StandingSpans(ECS::World& world, ECS::EntityHandle fence)
{
    std::vector<std::pair<uint32, uint32>> spans;
    world.Query<ECS::Read<Components::SplineFenceSpanPiece>, ECS::Read<Components::Parent>>().Each(
        [&spans, fence](const Components::SplineFenceSpanPiece& piece, const Components::Parent& parent)
        {
            if (parent.parent == fence)
                spans.emplace_back(piece.Run, piece.OrdinalInRun);
        });
    return spans;
}

// One row group per filled slot of the override table: what the span draws,
// which span it is and which pool piece it takes, and what the layout cannot
// do with it. Setting the kind to Remove empties the slot. Overrides are made
// by picking a span in the scene view (Inspectors/SplineFenceSpanPieceInspector.cpp);
// these rows are where the author sees them all and corrects one after the
// spline is edited.
void BuildOverrideRows(const InspectorContext& ctx)
{
    const auto* comp = ctx.World->GetComponent<SplineFence>(ctx.Entity);
    if (!comp)
        return;
    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    InspectorUI::AddLine(ctx.Parent, "Span Overrides");
    if (Editor::CountSpanOverrides(*comp) == 0u)
    {
        InspectorUI::AddLine(ctx.Parent,
                             "None. Select a span in the scene view to make it a gate or keep an "
                             "explicit piece there.");
        return;
    }

    const std::vector<std::pair<uint32, uint32>> standing = StandingSpans(*w, e);
    uint32 shown = 0;
    for (uint32 slot = 0; slot < Components::kSplineFenceMaxSpanOverrides; ++slot)
    {
        const Components::SplineSpanOverride& entry = comp->Overrides[slot];
        if (entry.Kind == Components::SplineSpanOverrideKind::None)
            continue;
        ++shown;
        auto* kind = InspectorUI::AddDropdownRow(
            ctx.Parent, "Override " + std::to_string(shown), Editor::SpanOverrideKindOptions("Remove"),
            Editor::SpanOverrideKindIndex(entry.Kind),
            "Gate draws the span from the Gate pool and clears the crest pieces over it; Explicit "
            "Piece keeps a chosen Span pool piece there; Remove gives the span back to the fence's "
            "own pick");
        kind->SetOnValueChanged(
            [w, e, n, undo, slot](const std::string& value)
            {
                const auto chosen = Editor::SpanOverrideKindOfOption(value);
                if (!chosen)
                    return;
                InspectorDrag::CommitComponentWithUndo<SplineFence>(
                    w, e, n, undo, "Change Span Override",
                    [slot, chosen](SplineFence& u)
                    {
                        if (*chosen == Components::SplineSpanOverrideKind::None)
                            u.Overrides[slot] = Components::SplineSpanOverride{};
                        else
                            u.Overrides[slot].Kind = *chosen;
                    });
                NotifyInspectorRebuild(w, e, n);
            });
        InspectorDrag::AddComponentIntRowWithDrag<SplineFence>(
            ctx.Parent, "Point", static_cast<int>(entry.PointIndex), w, e, n, undo,
            "Change Span Override Point",
            [slot](SplineFence& u, int v) { u.Overrides[slot].PointIndex = static_cast<uint32>(std::max(v, 0)); },
            0, "The authored point that opens the span's run, counted from 0");
        InspectorDrag::AddComponentIntRowWithDrag<SplineFence>(
            ctx.Parent, "Span", static_cast<int>(entry.SpanOrdinal), w, e, n, undo,
            "Change Span Override Span",
            [slot](SplineFence& u, int v)
            {
                u.Overrides[slot].SpanOrdinal =
                    static_cast<uint16>(std::clamp(v, 0, static_cast<int>(UINT16_MAX)));
            },
            0, "Which span of that run, counted from 0 at the point");
        const bool gate = entry.Kind == Components::SplineSpanOverrideKind::Gate;
        Editor::AddPoolPieceRow(
            ctx.Parent, gate ? "Gate Piece" : "Span Piece", gate ? comp->GatePool : comp->SpanPool,
            entry.PoolSlot, gate ? "Gate" : "Span", "Which pool piece the span takes",
            [w, e, n, undo, slot](uint8 picked)
            {
                InspectorDrag::CommitComponentWithUndo<SplineFence>(
                    w, e, n, undo, "Change Span Override Piece",
                    [slot, picked](SplineFence& u) { u.Overrides[slot].PoolSlot = picked; });
            });
        const bool stands = std::find(standing.begin(), standing.end(),
                                      std::pair<uint32, uint32>{entry.PointIndex, entry.SpanOrdinal}) !=
                            standing.end();
        for (const std::string& note : Editor::SpanOverrideNotes(*comp, entry, stands))
            InspectorUI::AddInfoCard(ctx.Parent, note);
    }
    if (shown == Components::kSplineFenceMaxSpanOverrides)
    {
        InspectorUI::AddInfoCard(ctx.Parent,
                                 "Span overrides are full (" +
                                     std::to_string(Components::kSplineFenceMaxSpanOverrides) +
                                     "/" +
                                     std::to_string(Components::kSplineFenceMaxSpanOverrides) +
                                     ") — capacity is fixed; remove one to add another");
    }
}

template <typename TEnum>
void AddEnumDropdownRow(const InspectorContext& ctx, const std::string& label,
                        const std::vector<Dropdown::Option>& options, TEnum current,
                        const std::string& undoName, TEnum SplineFence::* field,
                        const char* tooltip)
{
    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    auto* dd = InspectorUI::AddDropdownRow(ctx.Parent, label, options,
                                             static_cast<int>(current), tooltip);
    dd->SetOnValueChanged(
        [w, e, n, undo, undoName, field](const std::string& value)
        {
            int v = 0;
            try { v = std::stoi(value); }
            catch (...) { return; }
            InspectorDrag::CommitComponentWithUndo<SplineFence>(
                w, e, n, undo, undoName,
                [field, v](SplineFence& u) { u.*field = static_cast<TEnum>(v); });
        });
}

} // namespace

void RegisterSplineFenceInspector()
{
    using namespace Components;
    using namespace InspectorDrag;

    InspectorRegistry::Get().RegisterComponentInspector<SplineFence>(
        [](const InspectorContext& ctx)
        {
            auto* comp = ctx.World ? ctx.World->GetComponent<SplineFence>(ctx.Entity) : nullptr;
            if (!comp)
                return;

            ECS::World* w = ctx.World;
            ECS::EntityHandle e = ctx.Entity;
            Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
            Editor::UndoRedoService* undo = ctx.Undo;

            BuildPoolRows(ctx, "Post", &SplineFence::PostPool,
                          "Meshes planted at stations. May be left EMPTY: most fence kits build "
                          "the post into each panel's ends, and the spans then articulate at the "
                          "stations by themselves.");
            BuildPoolRows(ctx, "Span", &SplineFence::SpanPool,
                          "Meshes stretched between consecutive stations. Members need not share "
                          "a length — the run's fill computes how many instances cover it.");
            BuildPoolRows(ctx, "Gate", &SplineFence::GatePool,
                          "Pieces that stand in a span made a gate, at their own length. Make a "
                          "span a gate by selecting it in the scene view. Optional; an empty pool "
                          "leaves the opening empty.");
            BuildPoolRows(ctx, "Crest", &SplineFence::CrestPool,
                          "Whole pieces laid along the top of the spans at a fixed pitch — "
                          "battlements, coping, lamps. Stretched only with the wall under them, "
                          "when both share one length; towers and gates remove the pieces under "
                          "them.");
            BuildPoolRows(ctx, "Cap", &SplineFence::CapPool,
                          "End pieces that close the gap where a whole crest piece does not fit — "
                          "at a post, a tower or a gate. The longest that fits is used; shorter "
                          "than the pitch. Empty leaves the gap, which the post covers. Used only "
                          "where the row does not stand one piece on each wall: a crest piece of "
                          "another length than the wall, or a Crest Pitch that leaves air.");

            AddComponentFloatRowWithDrag<SplineFence>(
                ctx.Parent, "Crest Pitch", comp->CrestPitch, w, e, n, undo, "Change Crest Pitch",
                [](SplineFence& u, float v) { u.CrestPitch = std::max(v, 0.0f); }, 0.0f,
                "Distance between crest pieces. 0 uses the longest crest piece's own length; a "
                "smaller value is raised to it",
                {}, 0.0f);

            InspectorUI::AddAssetFieldRow(
                ctx.Parent, "Material Override", comp->OverrideMaterial.ToGuid(),
                {AssetType::Material}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
                [w, e, n, undo](const GUID& picked)
                {
                    InspectorDrag::CommitComponentWithUndo<SplineFence>(
                        w, e, n, undo, "Change Material Override",
                        [picked](SplineFence& u) { u.OverrideMaterial.Set(picked); });
                },
                ctx.Thumbnails,
                "Material applied to every piece the fence places: posts, spans, crest pieces and "
                "caps. Empty keeps each mesh's own embedded model material, which on many kits is "
                "an untextured placeholder.");

            AddComponentFloatRowWithDrag<SplineFence>(
                ctx.Parent, "Post Pitch", comp->PostPitch, w, e, n, undo, "Change Post Pitch",
                [](SplineFence& u, float v) { u.PostPitch = std::max(v, 0.05f); }, 2.4f,
                "Target metres between stations along the draped centerline; each run bends it "
                "slightly so its stations land on both authored points",
                {}, 0.05f);

            {
                static const std::vector<Dropdown::Option> kConformOptions = {
                    {"0", "None"},
                    {"1", "Height"},
                    {"2", "Height And Slope"},
                };
                AddEnumDropdownRow<SplinePlacementConform>(
                    ctx, "Conform", kConformOptions, comp->ConformMode, "Change Conform Mode",
                    &SplineFence::ConformMode,
                    "How stations meet the surface under the spline; spans derive their frame "
                    "from the stations and cast no rays");
            }
            {
                static const std::vector<Dropdown::Option> kConformTargetOptions = {
                    {"0", "Scene"},
                    {"1", "Terrain Only"},
                };
                AddEnumDropdownRow<SplineConformTarget>(
                    ctx, "Conform Target", kConformTargetOptions, comp->ConformTarget,
                    "Change Conform Target", &SplineFence::ConformTarget,
                    "What the conform ray may land on. Scene takes the nearest surface of any "
                    "kind, so a run passing under a tree or an awning stands on top of it — and "
                    "because a run's length is measured over the draped line, that also changes "
                    "how many spans the run builds; TerrainOnly makes scenery transparent. Under "
                    "TerrainOnly the drawn drape line can still ride scene props while the built "
                    "pieces stand on the terrain under them — the line does not read this "
                    "setting yet");
            }
            {
                static const std::vector<Dropdown::Option> kGradeOptions = {
                    {"0", "Racked"},
                    {"1", "Stepped"},
                    {"2", "Sheared"},
                };
                AddEnumDropdownRow<SplineSpanGrade>(
                    ctx, "Span Grade", kGradeOptions, comp->SpanGrade, "Change Span Grade",
                    &SplineFence::SpanGrade,
                    "Racked rotates the whole piece onto the grade, which leans its posts with it "
                    "(rails, hedges); Stepped holds it level at the lower station, so the height "
                    "jumps at stations and the uphill end buries (brick walls, castle walls); "
                    "Sheared slants only the run and leaves the piece's up axis with its stations, "
                    "so both ends meet their own station — the joinery a real fence uses on a "
                    "slope, and the one to reach for when a stepped run reads as a staircase. Its "
                    "posts therefore stand however the stations stand: plumb at Slope Blend 0, "
                    "leaning with them above it");
            }
            {
                static const std::vector<Dropdown::Option> kPlantOptions = {
                    {"0", "Bounds Min"},
                    {"1", "Pivot Plane"},
                };
                AddEnumDropdownRow<SplinePlantMode>(
                    ctx, "Plant Mode", kPlantOptions, comp->PlantMode, "Change Plant Mode",
                    &SplineFence::PlantMode,
                    "Pivot Plane lands the mesh's local y=0 on the ground and lets a planting "
                    "skirt bury — right for kits authored that way. Bounds Min lifts until the "
                    "lowest vertex touches, which floats a skirted mesh");
            }

            AddComponentFloatRowWithDrag<SplineFence>(
                ctx.Parent, "Slope Blend", comp->SlopeBlend, w, e, n, undo, "Change Slope Blend",
                [](SplineFence& u, float v) { u.SlopeBlend = std::clamp(v, 0.0f, 1.0f); }, 0.0f,
                "0 = plumb posts, which is what a real fence does; 1 = posts lean into the slope",
                {}, 0.0f, 1.0f);

            AddComponentFloatRowWithDrag<SplineFence>(
                ctx.Parent, "Span Max Stretch", comp->SpanMaxStretch, w, e, n, undo,
                "Change Span Max Stretch",
                [](SplineFence& u, float v) { u.SpanMaxStretch = std::max(v, 1.0f); }, 1.25f,
                "Cap on a span's length scale before the run re-walks with one more or one fewer "
                "station; a run no count satisfies is reported rather than silently distorted",
                {}, 1.0f);

            AddComponentIntRowWithDrag<SplineFence>(
                ctx.Parent, "Seed", static_cast<int>(comp->Seed), w, e, n, undo, "Change Seed",
                [](SplineFence& u, int v) { u.Seed = static_cast<uint32>(std::max(v, 0)); }, 0,
                "Salts the per-station, per-span and per-crest-piece pool picks; same seed "
                "reproduces the same picks on every rebuild. Caps are chosen by length, not by "
                "the seed");

            BuildOverrideRows(ctx);
        });
}

} // namespace GameEngine
