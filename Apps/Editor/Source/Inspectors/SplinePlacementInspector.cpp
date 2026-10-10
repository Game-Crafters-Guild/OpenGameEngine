#include "Inspectors/SplinePlacementInspector.h"

#include "InspectorRegistry.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::kSplinePoolCapacity;
using Components::SplinePlacement;
using PoolArray = Components::ModelRef[kSplinePoolCapacity];

// Write one slot, then compact the pool so active entries stay a dense prefix.
// Compaction is what keeps the selection contract (leading non-empty slots)
// and the row layout in agreement after a clear — and it heals any hole a
// hand-edited scene carried the moment the pool is touched here.
void SetPoolSlotCompacted(PoolArray& pool, uint32 slot, const GUID& guid)
{
    if (slot >= kSplinePoolCapacity)
        return;
    pool[slot] = Components::ModelRef(guid);
    Components::ModelRef compacted[kSplinePoolCapacity];
    uint32 n = 0;
    for (const Components::ModelRef& m : pool)
    {
        if (!m.IsNull())
            compacted[n++] = m;
    }
    for (uint32 i = 0; i < kSplinePoolCapacity; ++i)
        pool[i] = i < n ? compacted[i] : Components::ModelRef{};
}

// The inspector rebuild event: pool row counts change with their content, so a
// commit re-renders this component's section (the controller needs no signal —
// it observes the recipe bytes each frame).
void NotifyInspectorRebuild(ECS::World* w, ECS::EntityHandle e,
                            Editor::EditorChangeNotifications* n)
{
    if (!n)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = w;
    rebuild.entity = e;
    rebuild.componentType = ECS::GetComponentTypeId<SplinePlacement>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    n->NotifyComponentChanged(rebuild);
}

// Apply one mutation to the primary (with undo) and mirror it to every other
// selected entity carrying the component — the multi-selection contract the
// generic fallback provides, kept for this dedicated inspector.
template <typename ApplyFn>
void CommitToSelection(ECS::World* w, ECS::EntityHandle e,
                       const std::vector<ECS::EntityHandle>& extras,
                       Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo,
                       const std::string& name, ApplyFn&& apply)
{
    InspectorDrag::CommitComponentWithUndo<SplinePlacement>(w, e, n, undo, name, apply);
    for (ECS::EntityHandle ex : extras)
    {
        auto* c = w->GetComponent<SplinePlacement>(ex);
        if (!c)
            continue;
        SplinePlacement u = *c;
        apply(u);
        Editor::CommitComponentUpdate(w, ex, n, u);
    }
}

// Rows of Model asset fields for one pool: active entries, one add row while
// capacity remains, and validation lines — a full pool and a hole are reported
// by name, never silently dropped or reshuffled.
void BuildPoolRows(const InspectorContext& ctx, const std::string& poolLabel,
                   PoolArray SplinePlacement::* poolMember, const char* tooltip,
                   const std::vector<ECS::EntityHandle>& extras)
{
    auto* comp = ctx.World->GetComponent<SplinePlacement>(ctx.Entity);
    if (!comp)
        return;
    const PoolArray& pool = comp->*poolMember;
    const uint32 active = Components::ActivePoolCount(pool);
    const bool hole = Components::PoolHasHole(pool);

    // With a hole, show every slot so the ignored tail entries are visible and
    // fixable; otherwise the dense prefix plus one empty add row.
    const uint32 rows = hole ? kSplinePoolCapacity
                             : std::min(active + 1u, kSplinePoolCapacity);

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
            [w, e, extras, n, undo, undoName, poolMember, slot](const GUID& picked)
            {
                CommitToSelection(w, e, extras, n, undo, undoName,
                                  [poolMember, slot, picked](SplinePlacement& u)
                                  { SetPoolSlotCompacted(u.*poolMember, slot, picked); });
                NotifyInspectorRebuild(w, e, n);
            },
            ctx.Thumbnails, tooltip);
    }

    if (active == kSplinePoolCapacity)
    {
        InspectorUI::AddInfoCard(ctx.Parent,
                                 poolLabel + " is full (" + std::to_string(kSplinePoolCapacity) +
                                     "/" + std::to_string(kSplinePoolCapacity) +
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

// Enum dropdown driven by the scanner-generated table bound to the reflected
// field, so a new enumerator can never desync this inspector from the
// component (mirrors the generic EmitEnumDropdownRow: option payload is the
// table index, the committed value is the entry's Value).
//
// A "// @ge-tooltip" marker on the field wins over the literal passed here —
// the same precedence DefaultComponentInspector::FieldTooltip applies, and the
// field is the one place a tooltip can be written once and reach every
// inspector that renders it. Pass nullptr for a field that carries a marker.
template <typename TEnum>
void AddReflectedEnumRow(const InspectorContext& ctx, const std::string& label,
                         std::string_view fieldName, TEnum current, const std::string& undoName,
                         TEnum SplinePlacement::* field, const char* tooltip,
                         const std::vector<ECS::EntityHandle>& extras)
{
    std::span<const ECS::EnumNameValue> names{};
    std::string_view markerTooltip;
    for (const ECS::FieldInfo& f :
         ECS::ComponentFieldRegistry::Get(ECS::GetComponentTypeId<SplinePlacement>()))
    {
        if (f.Name == fieldName)
        {
            names = f.EnumNames;
            markerTooltip = f.Tooltip;
            break;
        }
    }
    if (names.empty())
    {
        // Impossible while the scanner binds the table; loud beats a silent
        // fallback that would duplicate the enumerator list by hand.
        InspectorUI::AddLine(ctx.Parent, label + ": enum table missing from reflection");
        return;
    }

    std::vector<Dropdown::Option> options;
    options.reserve(names.size());
    int selectedIndex = 0;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        options.push_back({std::to_string(i), std::string(names[i].Name)});
        if (names[i].Value == static_cast<std::int64_t>(current))
            selectedIndex = static_cast<int>(i);
    }

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    const std::string rowTooltip(!markerTooltip.empty()  ? markerTooltip
                                 : tooltip               ? std::string_view(tooltip)
                                                         : std::string_view{});
    auto* dd = InspectorUI::AddDropdownRow(ctx.Parent, label, options, selectedIndex,
                                             rowTooltip.empty() ? nullptr : rowTooltip.c_str());
    dd->SetOnValueChanged(
        [w, e, extras, n, undo, undoName, field, names](const std::string& value)
        {
            int idx = -1;
            try { idx = std::stoi(value); }
            catch (...) { return; }
            if (idx < 0 || static_cast<std::size_t>(idx) >= names.size())
                return;
            const TEnum v = static_cast<TEnum>(names[static_cast<std::size_t>(idx)].Value);
            CommitToSelection(w, e, extras, n, undo, undoName,
                              [field, v](SplinePlacement& u) { u.*field = v; });
        });
}

} // namespace

void RegisterSplinePlacementInspector()
{
    using namespace Components;
    using namespace InspectorDrag;

    InspectorRegistry::Get().RegisterComponentInspector<SplinePlacement>(
        [](const InspectorContext& ctx)
        {
            auto* comp = ctx.World ? ctx.World->GetComponent<SplinePlacement>(ctx.Entity) : nullptr;
            if (!comp)
                return;

            ECS::World* w = ctx.World;
            ECS::EntityHandle e = ctx.Entity;
            Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
            Editor::UndoRedoService* undo = ctx.Undo;

            // Multi-selection: mirror edits to the other selected entities that
            // also carry the component (membership mirrors the generic fallback).
            std::vector<ECS::EntityHandle> extras;
            for (ECS::EntityHandle ex : GetAdditionalEntities(ctx))
            {
                if (w->GetComponent<SplinePlacement>(ex))
                    extras.push_back(ex);
            }

            BuildPoolRows(ctx, "Straight", &SplinePlacement::StraightPool,
                          "Tile meshes placed along the spline; each station picks one "
                          "deterministically from Seed. Pool members must share footprint "
                          "width and pivot convention.",
                          extras);
            BuildPoolRows(ctx, "Curve", &SplinePlacement::CurvePool,
                          "Corner pieces for joints past the seam-shear cap (consumed by the "
                          "corner slice). Optional; empty means every joint shears.",
                          extras);
            BuildPoolRows(ctx, "Scatter", &SplinePlacement::ScatterPool,
                          "Accent meshes scattered across the band (consumed by the scatter "
                          "slice).",
                          extras);

            InspectorUI::AddAssetFieldRow(
                ctx.Parent, "Material Override", comp->OverrideMaterial.ToGuid(),
                {AssetType::Material}, &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
                [w, e, extras, n, undo](const GUID& picked)
                {
                    CommitToSelection(w, e, extras, n, undo, "Change Material Override",
                                      [picked](SplinePlacement& u)
                                      { u.OverrideMaterial.Set(picked); });
                },
                ctx.Thumbnails,
                "Material applied to every placed tile. Empty keeps each mesh's own embedded "
                "model material, which on many kits is an untextured placeholder.");

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Spacing", comp->Spacing, w, e, n, undo, "Change Spacing",
                [](SplinePlacement& u, float v) { u.Spacing = std::max(v, 0.05f); }, 2.0f,
                "Metres between tile centers along the draped centerline", extras, 0.05f);

            AddReflectedEnumRow<SplinePlacementFit>(
                ctx, "Fit", "Fit", comp->Fit, "Change Fit", &SplinePlacement::Fit,
                "FitToLength rounds the tile count to close the spline exactly; "
                "FixedPitch steps exactly Spacing metres and cuts the last partial step",
                extras);
            AddReflectedEnumRow<SplinePlacementConform>(
                ctx, "Conform", "ConformMode", comp->ConformMode, "Change Conform Mode",
                &SplinePlacement::ConformMode, "How tiles meet the surface under the spline",
                extras);
            AddReflectedEnumRow<SplineConformTarget>(
                ctx, "Conform Target", "ConformTarget", comp->ConformTarget,
                "Change Conform Target", &SplinePlacement::ConformTarget,
                "What the conform ray may land on. Scene takes the nearest surface of any kind, "
                "so a tile passing under a cart or a canopy sits on top of it; TerrainOnly makes "
                "scenery transparent so tiles follow the ground beneath it. Under TerrainOnly the "
                "drawn drape line can still ride scene props while the placed tiles stand on the "
                "terrain under them — the line does not read this setting yet",
                extras);
            // Tooltip from the @ge-tooltip marker on the field, not a literal here.
            AddReflectedEnumRow<SplinePlantMode>(
                ctx, "Plant Mode", "PlantMode", comp->PlantMode, "Change Plant Mode",
                &SplinePlacement::PlantMode, nullptr, extras);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Lateral Offset", comp->LateralOffset, w, e, n, undo,
                "Change Lateral Offset",
                [](SplinePlacement& u, float v) { u.LateralOffset = v; }, 0.0f,
                "Metres to the right of travel; negative = left", extras);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Slope Blend", comp->SlopeBlend, w, e, n, undo, "Change Slope Blend",
                [](SplinePlacement& u, float v) { u.SlopeBlend = std::clamp(v, 0.0f, 1.0f); },
                1.0f, "0 = tiles stay world-up, 1 = tiles align fully to the surface normal",
                extras, 0.0f, 1.0f);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Max Tilt", comp->MaxTiltDegrees, w, e, n, undo, "Change Max Tilt",
                [](SplinePlacement& u, float v)
                { u.MaxTiltDegrees = std::clamp(v, 0.0f, 90.0f); },
                Components::kDefaultSplineMaxTiltDegrees,
                "Degrees a tile may lean from upright. Ground steeper than this still sets the "
                "tile's height, but the tile holds this angle instead of standing on edge",
                extras, 0.0f, 90.0f);

            AddComponentIntRowWithDrag<SplinePlacement>(
                ctx.Parent, "Seed", static_cast<int>(comp->Seed), w, e, n, undo, "Change Seed",
                [](SplinePlacement& u, int v) { u.Seed = static_cast<uint32>(std::max(v, 0)); },
                0,
                "Salts the per-station pool pick and every jitter draw below; same seed "
                "reproduces the same layout exactly",
                extras);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Seam Shear Max", comp->SeamShearMaxDegrees, w, e, n, undo,
                "Change Seam Shear Max",
                [](SplinePlacement& u, float v) { u.SeamShearMaxDegrees = std::max(v, 0.0f); },
                8.0f, "Cap in degrees on the per-tile skew that softens rigid seams; 0 disables",
                extras, 0.0f);

            // Per-station variation. Every one of these is deterministic in Seed
            // and defaults to a no-op, so an existing recipe lays out unchanged.
            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Spacing Jitter", comp->SpacingJitterMetres, w, e, n, undo,
                "Change Spacing Jitter",
                [](SplinePlacement& u, float v) { u.SpacingJitterMetres = std::max(v, 0.0f); },
                0.0f,
                "Metres a tile may slide along the path, either way. Breaks up the railway-sleeper "
                "look of an exact pitch. Capped at half the spacing, so tiles never reorder",
                extras, 0.0f);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Yaw Jitter", comp->YawJitterDegrees, w, e, n, undo,
                "Change Yaw Jitter",
                [](SplinePlacement& u, float v)
                { u.YawJitterDegrees = std::clamp(v, 0.0f, 180.0f); },
                0.0f, "Degrees a tile may turn on the spot, either way", extras, 0.0f, 180.0f);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Lateral Jitter", comp->LateralJitterMetres, w, e, n, undo,
                "Change Lateral Jitter",
                [](SplinePlacement& u, float v) { u.LateralJitterMetres = std::max(v, 0.0f); },
                0.0f, "Metres a tile may slide across the path, either way", extras, 0.0f);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "Dropout", comp->DropoutChance, w, e, n, undo, "Change Dropout",
                [](SplinePlacement& u, float v) { u.DropoutChance = std::clamp(v, 0.0f, 1.0f); },
                0.0f,
                "Fraction of stations left empty. 0 places every tile, 1 places none — gaps are "
                "what stop a run reading as a manufactured strip",
                extras, 0.0f, 1.0f);

            AddComponentFloatRowWithDrag<SplinePlacement>(
                ctx.Parent, "End Taper", comp->EndTaperMetres, w, e, n, undo, "Change End Taper",
                [](SplinePlacement& u, float v) { u.EndTaperMetres = std::max(v, 0.0f); }, 0.0f,
                "Metres at each end over which tiles thin out to nothing, so a run peters out "
                "instead of stopping square. 0 disables",
                extras, 0.0f);
        });
}

} // namespace GameEngine
