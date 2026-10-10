#include "Terrain/TerrainModifierInspectors.h"

#include "InspectorRegistry.h"

#include "Editor/Entities/EditorComponentTraits.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Logger/Logger.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainRoutePolyline.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/TerrainVolumeNotices.h"
#include "SceneView/TerrainBrushTool.h"
#include "Terrain/TerrainLayers.h"
#include "Terrain/TerrainRoleMaterials.h"
#include "Terrain/TerrainRuleRows.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/TextField.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/PickerQueryFilter.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdio>
#include <string>

#include <algorithm>
#include <any>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using namespace Components;
using namespace InspectorDrag;

// What a regional grass multiplier is a percentage OF: the grass settings on the terrain the
// region sits on. Zero means there is no single answer - with no enabled terrain carrying grass,
// or with several that disagree, the absolute value a multiplier produces is ambiguous, so the
// resolver publishes nothing and the inspector shows the percentage alone.
struct TerrainGrassBase
{
    float32 BladeHeight = 0.0f;
    float32 BladesPerSquareMeter = 0.0f;
};

// Pure read, like AnyActiveSphericalTerrain: no component column is write-stamped, so building
// the inspector cannot trip the modifier bake's change gate.
TerrainGrassBase ResolveTerrainGrassBase(const ECS::World& world)
{
    TerrainGrassBase base;
    uint32 count = 0;
    const_cast<ECS::World&>(world)
        .Query<ECS::Read<Components::Terrain>, ECS::Read<Components::TerrainGrass>>()
        .Each([&](const Components::Terrain&, const Components::TerrainGrass& grass)
        {
            ++count;
            base.BladeHeight = grass.TextureGrass ? grass.TextureSize : grass.BladeHeight;
            base.BladesPerSquareMeter =
                grass.TextureGrass ? grass.TextureCardsPerSquareMeter : grass.BladesPerSquareMeter;
        });
    if (count != 1)
        return {}; // Ambiguous: publish nothing rather than whichever terrain the query saw last.
    return base;
}

// A multiplier row's label, carrying the absolute value that multiplier lands on when the base is
// known ("Height (0.36 m)"). The absolute rides the label rather than the field's unit because a
// unit long enough to hold it does not fit beside the number, and it is bracketed rather than
// dash-separated because a spaced hyphen beside a number reads as a minus sign - the same stack
// holds a Height Offset effect whose value really is signed. Recomputed on every change.
std::string GrassMultiplierLabel(const char* name, float32 multiplier, float32 base, const char* unit)
{
    if (base <= 0.0f)
        return name;
    char text[64];
    std::snprintf(text, sizeof(text), "%s (%.2f %s)", name,
                  static_cast<double>(multiplier * base), unit);
    return text;
}

// The Height row's tooltip, built once from kTerrainGrassHeightReadableFloor so the number an
// author reads is the number the header documents. The threshold is a readability crossing and
// not a limit, and the sentence says so: a region keeps changing below it, into ground rather
// than into shorter grass.
const char* GrassHeightTooltip()
{
    static const std::string text =
        "Blade height inside this region, as a percentage of the terrain's own grass height. "
        "100% leaves the terrain's grass alone. Regions blend toward their target in priority "
        "order across the volume's falloff, so a later 100% region restores the terrain's grass "
        "over an earlier reduction; 0% removes it. Below about "
        + std::to_string(static_cast<int32>(kTerrainGrassHeightReadableFloor * 100.0f + 0.5f))
        + "% a region reads as the ground colour under it rather than as short grass - the "
          "control keeps working all the way down, it just stops reading as grass. To make a "
          "patch look sparser rather than shorter, use Density.";
    return text.c_str();
}

// True when the scene has any enabled spherical (planet) terrain. Pure read — it never
// write-stamps a component column, so drawing this notice can't trip the modifier bake's change
// gate (the #530 zero-rebake-on-inspector-draw contract).
bool AnyActiveSphericalTerrain(ECS::World* world)
{
    if (!world)
        return false;
    bool sphere = false;
    world->Query<ECS::Read<Components::Terrain>>()
        .Each([&](const Components::Terrain& terrain) {
            if (terrain.Domain == TerrainDomain::Spherical)
                sphere = true;
        });
    return sphere;
}

// The terrain whose channel names label a paint effect's picker. A paint effect belongs to one
// scene's terrain, so the first enabled one is the answer; the labels degrade to the semantic
// channel names when there is none, which is also what an unmigrated terrain shows.
const Components::Terrain* FindTerrainForChannelLabels(ECS::World* world)
{
    if (!world)
        return nullptr;
    const Components::Terrain* found = nullptr;
    world->Query<ECS::Read<Components::Terrain>>()
        .Each([&](const Components::Terrain& terrain) {
            if (!found)
                found = &terrain;
        });
    return found;
}

// The channel picker's labels with no terrain to read a binding from.
std::vector<Dropdown::Option> DefaultChannelOptions()
{
    std::vector<Dropdown::Option> options;
    options.reserve(TerrainLayers::kCount);
    for (uint32_t i = 0; i < TerrainLayers::kCount; ++i)
        options.push_back({std::to_string(i), TerrainLayers::kNames[i]});
    return options;
}

// The honest notice for an effect the sphere bake does not apply yet — the height-offset and
// paint-layer effects below. Shown only when a planet is present, so a planar-only scene stays
// uncluttered. Keep the supported list in lockstep with IsSphereSupportedModifier (Noise, Flatten,
// Stamp, Sculpt zones). Answers her #12 confusion: a greyed gizmo on a planet now explains itself
// instead of silently doing nothing.
void AddSphereUnsupportedNotice(const InspectorContext& ctx)
{
    if (!AnyActiveSphericalTerrain(ctx.World))
        return;
    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "Not baked on spherical (planet) terrains yet - affects planar / tiled terrains only. "
        "Planets bake: Noise, Flatten, Stamp and Sculpt zones."));
}

// Blend mode dropdown shared by the flatten / height-offset / noise / stamp effects —
// four call sites below.
//
// Each option's VALUE is the enum value as a string, and 3 is absent because it is the
// retired Smooth tombstone. The option index is therefore NOT the enum value: the selection
// is resolved by matching the value string, and a value with no option (a retired mode
// reaching the component through a reflected write) falls back to Add, which is what the
// bake does with every unhandled value.
// `includeAverage` belongs to the four HEIGHT effects, which are the ones that
// carry a pool group and reach a pool accumulator at the bake. It stays off for
// the sculpt zone, whose blend switch has a fall-through arm — offering Average
// there would be a mode its bake silently bakes as Add.
void AddBlendModeDropdown(UIElement* parent, TerrainModifierBlend currentBlend,
                          std::function<void(TerrainModifierBlend)> onChanged,
                          bool includeAverage = false)
{
    static const std::vector<Dropdown::Option> kBlendOptions = {
        {"0", "Set"},
        {"1", "Add"},
        {"2", "Subtract"},
        {"4", "Min (cut)"},
        {"5", "Max (union)"},
        {"6", "Smooth Min"},
        {"7", "Smooth Max"},
    };
    static const std::vector<Dropdown::Option> kBlendOptionsWithAverage = {
        {"0", "Set"},
        {"1", "Add"},
        {"2", "Subtract"},
        {"4", "Min (cut)"},
        {"5", "Max (union)"},
        {"6", "Smooth Min"},
        {"7", "Smooth Max"},
        {"8", "Average (pool)"},
    };
    const std::vector<Dropdown::Option>& options =
        includeAverage ? kBlendOptionsWithAverage : kBlendOptions;

    const auto optionIndex = [&options](TerrainModifierBlend blend) {
        const std::string want = std::to_string(static_cast<int>(blend));
        for (std::size_t i = 0; i < options.size(); ++i)
            if (options[i].value == want)
                return static_cast<int>(i);
        return -1;
    };
    int selectedIdx = optionIndex(currentBlend);
    if (selectedIdx < 0)
        selectedIdx = optionIndex(TerrainModifierBlend::Add);

    auto* dd = InspectorUI::AddDropdownRow(parent, "Blend", options, selectedIdx,
        includeAverage
            ? "How the modifier combines with existing terrain. Min cuts (never raises), Max "
              "unions (never lowers); the Smooth pair rounds the seam over Blend Smoothing. "
              "Average POOLS instead: every effect sharing a pool contributes its value and "
              "its weight, and the pool applies the weighted average once, so two regions "
              "covering the same ground land between their answers instead of the later one "
              "overwriting the earlier. A pooled effect applies at the pool's slot, not its "
              "own. Pools are per effect type — a Noise pool and a Flatten pool of the same "
              "name are different pools."
            : "How the modifier combines with existing terrain. "
              "Min cuts (never raises), Max unions (never lowers); "
              "the Smooth pair rounds the seam over Blend Smoothing.");
    dd->SetOnValueChanged([onChanged = std::move(onChanged)](const std::string& value) {
        onChanged(static_cast<TerrainModifierBlend>(std::stoi(value)));
    });
}

// The blend radius row, shown only for the two modes that read it — an always-visible field
// that does nothing for five of the seven modes is the confusion this avoids.
template<typename TComp>
void AddBlendSmoothingRow(UIElement* parent, const TComp* comp, ECS::World* w,
                          ECS::EntityHandle e, Editor::EditorChangeNotifications* n)
{
    if (!BlendUsesSmoothing(comp->Blend))
        return;
    AddComponentFloatRowWithDrag<TComp>(
        parent, "Blend Smoothing", comp->BlendSmoothing, w, e, n,
        [](TComp& u, float v) { u.BlendSmoothing = std::max(0.0f, v); },
        "Blend radius in metres of height: how wide the Smooth Min / Smooth Max seam rounds");
}

// Effects only apply where a volume defines a region, and the volume's own
// section is where the user changes that region — say so instead of rendering an
// effect that quietly does nothing.
// The pool group name: a free-text row rather than a picker, because a pool is
// created by naming it and there is no registry of names to pick from.
TextField* AddPoolGroupRow(UIElement* parent, const std::string& initial)
{
    UIElement* row = InspectorUI::AddRow(parent);
    // The tooltip quotes the bound, so the assert keeps the two together: the
    // capacity is the buffer, the authored name is one shorter (terminator).
    static_assert(kTerrainPoolGroupCapacity == 32,
                  "The Pool Group tooltip below quotes 31 characters. Update it with the capacity.");
    InspectorUI::AddLabel(row, "Pool Group",
        "LEAVE BLANK unless you need this region kept apart. Blank means it averages with every "
        "other blank one where they overlap, which is what a junction is. Naming a pool moves "
        "this region into a system of its own: it averages with regions of the same name and "
        "OVERWRITES the rest instead of averaging with them. Up to 31 characters — a longer name "
        "is refused rather than shortened, because two names that agree that far would merge "
        "into one pool.");
    UIElement* field = InspectorUI::AddFieldContainer(row);
    auto text = std::make_unique<TextField>();
    text->SetValue(initial);
    text->AddClass("inspector-text-field");
    TextField* raw = text.get();
    field->AddChild(std::move(text));
    return raw;
}

// The claim row, shown on EVERY blend mode. Ownership is not a pooling concept:
// the bake reads this flag at the slot the effect applies in whatever operator it
// composes with, so hiding it off Average would hide a knob that works.
//
// One template for all four height effects, so the row cannot drift apart
// between them.
template <typename TEffect>
void AddClaimsRow(const InspectorContext& ctx, const TEffect* comp, const char* claimsTooltip)
{
    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

    AddToggleRow(ctx.Parent, "Stops at Claimed Ground", comp->RespectClaims,
        [w, e, n](bool v) {
            auto* c = w->GetComponent<TEffect>(e);
            if (!c) return;
            TEffect u = *c;
            u.RespectClaims = v;
            Editor::CommitComponentUpdate(w, e, n, u);
        },
        claimsTooltip);
}

// The pool group row, shown only when the blend is Average — pooling is the whole
// meaning of Average, and the bake reaches the group only through the pooled
// branch, so on any other blend it is a knob that does nothing and is absent
// instead. Same rule as the blend-smoothing row.
//
// One template for all four height effects: the row must not drift apart, and
// the over-length refusal below is the same answer the scene loader gives at the
// same bound.
template <typename TEffect>
void AddPoolingRows(const InspectorContext& ctx, const TEffect* comp)
{
    if (comp->Blend != TerrainModifierBlend::Average)
        return;

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

    // ---- Advanced: how this region composes with the others ------------
    InspectorUI::AddTextBlock(ctx.Parent, "Advanced", "inspector-body-subheader");

    TextField* groupField = AddPoolGroupRow(ctx.Parent, std::string(EffectPoolName(*comp)));
    groupField->SetOnCommit([groupField, w, e, n]()
    {
        auto* c = w->GetComponent<TEffect>(e);
        if (!c) return;
        const std::string value = groupField->GetValue();

        // Refused, not truncated — the same answer at the same bound the scene
        // loader gives (ApplyEffectPoolGroup in TerrainSceneSchemas.cpp rejects a
        // name of sizeof(PoolGroup) or longer). Truncating would silently MERGE
        // two pools whose first kTerrainPoolGroupCapacity - 1 characters agree,
        // and merged pools average each other's values. The field goes back to
        // the name that is actually in effect, so what is shown is what is baking.
        if (value.size() >= sizeof(c->PoolGroup))
        {
            groupField->SetValue(std::string(EffectPoolName(*c)));
            Logger::Log::Warning(
                "Pool group name is {} characters; the limit is {}. The edit was refused "
                "rather than truncated, because a truncated name would merge this region "
                "into another pool. Shorten the name and commit again.",
                value.size(), sizeof(c->PoolGroup) - 1);
            return;
        }

        TEffect u = *c;
        std::memset(u.PoolGroup, 0, sizeof(u.PoolGroup));
        std::memcpy(u.PoolGroup, value.data(), value.size());
        Editor::CommitComponentUpdate(w, e, n, u);
    });
}

// Every pooled effect loses the GPU height bake: the kernel evaluates one
// modifier record per texel with no scratch to accumulate into, so the packer
// refuses the whole batch and the terrain's height bake drops to the CPU.
// Not a new cost for the flatten — the packer refused pooled flattens before
// the generalization too. Said at the row, because it is a real cost the
// author is choosing.
void AddPooledCpuBakeNotice(const InspectorContext& ctx, TerrainModifierBlend blend)
{
    if (blend != TerrainModifierBlend::Average)
        return;
    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "Average pools this effect, which the GPU height bake cannot express - the whole "
        "terrain's height bake falls back to the CPU while it is on."));
}

// The layer-0 trap, said at the row that arms it. The bake warns about the
// actual scene once it can see the claimant; this is the standing rule, which an
// author needs BEFORE they go and find out the hard way.
//
// Not a per-scene diagnosis — the inspector sees one effect, not the modifier
// stack — so it states the requirement rather than accusing a particular volume.
void AddClaimedGroundNeedsPaintNotice(const InspectorContext& ctx, bool respectClaims)
{
    if (!respectClaims)
        return;
    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "The claiming volume must PAINT the material this ground should keep. The splatmap starts "
        "empty every bake and an empty texel resolves to the first material, so a claim with no "
        "paint under it leaves a patch of the wrong material instead of preserving anything."));
}

// Same shape as the pooled height notice: a real cost the author is choosing,
// said at the row that chooses it.
//
// Surface rules only. A paint effect already forces the CPU splat whatever its
// flags say — the kernel has no mask sampler or zone payload to reproduce a
// paint stroke with — so arming claims on one costs nothing new, and saying it
// would there would be a false warning.
void AddClaimRespectingRulesCpuBakeNotice(const InspectorContext& ctx, bool respectClaims)
{
    if (!respectClaims)
        return;
    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "Stopping at claimed ground is a CPU-side accumulate across the modifier stack, which the "
        "GPU splat bake cannot express - the whole terrain's splat bake falls back to the CPU "
        "while it is on."));
}

void AddOrphanEffectNotice(const InspectorContext& ctx)
{
    if (ctx.World->GetComponent<TerrainModifierVolume>(ctx.Entity))
        return;
    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "No Terrain Modifier Volume on this entity, so this effect is inactive. "
        "Add one to give it a region."));
}

// Effects stack in the order their sections appear under the volume; dragging a
// section header rewrites StackOrder. The number is shown, not edited.
template <typename TEffect>
void AddEffectHeader(const InspectorContext& ctx, const TEffect* comp)
{
    AddOrphanEffectNotice(ctx);
    InspectorUI::AddLine(ctx.Parent,
                         "Stack position " + std::to_string(comp->StackOrder)
                             + " - drag the section header to reorder");
}

// ---- Add Effect ------------------------------------------------------------
//
// Terrain effects are absent from the general Add Component picker and arrive
// only here, the same way post-process effects arrive only through the Volume
// inspector's Add Post FX button: an effect without a volume has no region and
// does nothing, so the affordance lives where the region does.

// The BUTTON's width, and the anchor maths that centres the popup under it.
constexpr float kTerrainEffectPickerWidthPx = 360.0f;

// The POPUP's width, which is a different number and was previously left at
// SearchDialog's 360px default by omission. Every effect description is 74-86
// characters and was being scissored mid-word: the dialog clips with a hard
// rect because `text-overflow` is dropped by the CSS parser, so there is no
// ellipsis to soften a too-narrow panel - only room, or a cut.
constexpr float kTerrainEffectPickerPopupWidthPx = 560.0f;

struct AddTerrainEffectChoice
{
    ECS::ComponentTypeId TypeId = 0;
};

void NotifyComponentChanged(Editor::EditorChangeNotifications* notifications,
                            ECS::World* world,
                            ECS::EntityHandle entity,
                            ECS::ComponentTypeId typeId)
{
    if (!notifications)
        return;
    notifications->NotifyComponentChanged(
        {world, entity, typeId, Editor::EditorChangeNotifications::ChangeKind::Commit});
}

// Adds one terrain effect through the volume inspector's picker. The new effect
// goes on TOP of the stack (StackOrder = max + 1), which is where a user who
// just added it expects it to apply.
class AddTerrainEffectCommand final : public Editor::IEditorCommand
{
  public:
    AddTerrainEffectCommand(std::string name,
                            ECS::World* world,
                            Editor::EditorChangeNotifications* notifications,
                            ECS::EntityHandle entity,
                            ECS::ComponentTypeId effectTypeId)
        : m_Name(std::move(name)),
          m_World(world),
          m_Notifications(notifications),
          m_Entity(entity),
          m_EffectTypeId(effectTypeId)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Redo() override
    {
        m_AddedEffect = false;
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        const int32 stackOrder = TerrainECS::NextEffectStackOrder(*m_World, m_Entity);
        if (!TerrainECS::AddTerrainEffectDefault(*m_World, m_Entity, m_EffectTypeId, stackOrder))
            return;

        m_AddedEffect = true;
        NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_EffectTypeId);
    }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (m_AddedEffect && m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_EffectTypeId))
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_EffectTypeId);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_EffectTypeId = 0;
    bool m_AddedEffect = false;
};

// The canonical effects this volume does not already carry — one component of a
// type per entity, so a present effect is not offerable. The entity's effect set
// is snapshotted when the picker opens.
class TerrainEffectSearchProvider final : public ISearchProvider
{
  public:
    TerrainEffectSearchProvider(ECS::World* world, ECS::EntityHandle entity)
    {
        for (const TerrainECS::TerrainEffectTypeInfo& info : TerrainECS::TerrainEffectTypes())
        {
            if (world && world->HasComponent(entity, info.TypeId))
                continue;
            m_Entries.push_back({std::string(info.Title), std::string(info.Description), info.TypeId});
        }
    }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        const PickerQueryFilter filter = PickerQueryFilter::Parse(query);
        std::vector<SearchResultItem> results;
        for (std::size_t i = 0; i < m_Entries.size(); ++i)
        {
            const auto& entry = m_Entries[i];
            if (!filter.Matches(entry.Label, entry.Description))
                continue;

            SearchResultItem item;
            item.Id = static_cast<SearchItemId>(i + 1);
            item.Label = entry.Label;
            item.Detail = entry.Description;
            item.UserData = AddTerrainEffectChoice{entry.TypeId};
            results.push_back(std::move(item));
        }
        sink(std::move(results), true);
    }

    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search effects..."; }

  private:
    struct EffectEntry
    {
        std::string Label;
        std::string Description;
        ECS::ComponentTypeId TypeId = 0;
    };

    std::vector<EffectEntry> m_Entries;
};

void AddTerrainEffectButton(const InspectorContext& ctx)
{
    if (ctx.Entities.size() > 1)
        return;

    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const std::function<ECS::World*()> getWorld = ctx.GetWorld;
    const std::function<void()> requestInspectorRefresh = ctx.RequestInspectorRefresh;

    auto button = std::make_unique<Button>();
    button->SetText("Add Effect");
    button->SetTooltip("Add a terrain effect to this volume.");
    button->AddClass("inspector-add-component-button");
    button->Overrides()
        .Set(Style::Width, StyleLength::Px(kTerrainEffectPickerWidthPx))
        .Set(Style::AlignSelf, AlignItems::Center)
        .Set(Style::MarginTop, StyleLength::Px(6.0f))
        .Set(Style::Order, 1);

    auto pickerOpen = std::make_shared<bool>(false);
    button->RegisterEventHandler(kEventButtonClick, [world, entity, notifications, undo, getWorld,
         requestInspectorRefresh, pickerOpen](UIEvent& e)
        {
            UIElement& source = *e.CurrentTarget;
            ECS::World* currentWorld = getWorld ? getWorld() : world;
            if (*pickerOpen || !currentWorld || !entity.IsValid() ||
                !currentWorld->IsValid(entity))
                return;

            UIManager* manager = source.GetOwnerManager();
            UIElement* root = manager ? manager->GetRootElement() : nullptr;
            if (!root)
                return;

            *pickerOpen = true;
            auto provider = std::make_shared<TerrainEffectSearchProvider>(currentWorld, entity);
            auto dialog = std::make_unique<SearchDialog>();
            dialog->SetProvider(provider.get());
            dialog->SetFixedHeight(true);
            dialog->SetPanelWidth(kTerrainEffectPickerPopupWidthPx);

            SearchDialog* dialogPtr = dialog.get();
            dialog->SetOnResult(
                [world, entity, notifications, undo, getWorld,
                 requestInspectorRefresh, pickerOpen, provider, dialogPtr, root](
                    const SearchResultItem& item)
                {
                    (void)provider; // Keeps the dialog's raw provider pointer valid.
                    const auto* choice = std::any_cast<AddTerrainEffectChoice>(&item.UserData);
                    if (!choice)
                        return;

                    const ECS::ComponentTypeId effectTypeId = choice->TypeId;
                    const std::string caption = "Add " + item.Label;
                    root->PostSafeAction(
                        [world, entity, notifications, undo, getWorld,
                         requestInspectorRefresh, pickerOpen, dialogPtr, root,
                         effectTypeId, caption]()
                        {
                            *pickerOpen = false;
                            root->RemoveChild(dialogPtr);
                            ECS::World* currentWorld = getWorld ? getWorld() : world;
                            if (!currentWorld || !entity.IsValid() ||
                                !currentWorld->IsValid(entity))
                                return;

                            if (undo)
                            {
                                undo->Execute(std::make_unique<AddTerrainEffectCommand>(
                                    caption, currentWorld, notifications, entity, effectTypeId));
                            }
                            else
                            {
                                AddTerrainEffectCommand command(
                                    caption, currentWorld, notifications, entity, effectTypeId);
                                command.Do();
                            }

                            if (requestInspectorRefresh)
                                requestInspectorRefresh();
                        });
                });
            dialog->SetOnCancel(
                [pickerOpen, provider, dialogPtr, root]()
                {
                    (void)provider; // Keeps the dialog's raw provider pointer valid.
                    *pickerOpen = false;
                    root->RemoveChild(dialogPtr);
                });

            const float buttonCenterX = source.GetLayoutX() + source.GetLayoutWidth() * 0.5f;
            const float anchorX = buttonCenterX - kTerrainEffectPickerPopupWidthPx * 0.5f;
            const float anchorY = source.GetLayoutY() + source.GetLayoutHeight();
            root->AddChild(std::move(dialog));
            dialogPtr->SetAnchorPosition(
                anchorX, anchorY, SearchDialogHorizontalAnchor::LeadingLeft,
                source.GetLayoutHeight());
            dialogPtr->Show();
        });

    ctx.Parent->AddChild(std::move(button));
}

// A volume whose gather finds no effects bakes nothing. Say so where the fix is,
// rather than leaving the user with a gizmo that changes no terrain.
void AddEmptyVolumeNotice(const InspectorContext& ctx)
{
    for (const TerrainECS::TerrainEffectTypeInfo& info : TerrainECS::TerrainEffectTypes())
        if (ctx.World->HasComponent(ctx.Entity, info.TypeId))
            return;

    ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "No effects on this volume yet, so it does nothing. Add Effect below."));
}

} // anonymous namespace

void RegisterTerrainModifierInspectors()
{
    // The Surface Rules effect's two editor icons, registered rather than added to the inspector's
    // and the hierarchy's built-in type chains. Those chains are the central hubs this registry
    // exists to keep closed, and the effect's editor surface is owned here.
    //
    // The hierarchy class is what makes the row read as the rules rather than as the volume: the
    // entity carries TerrainModifierVolume too, and the traits lookup runs ahead of the volume
    // branches, so the rules win the row without either chain naming this type.
    {
        Editor::EditorComponentTraits traits;
        traits.InspectorIconClass = "inspector-section-icon-terrain-surface-rules";
        traits.HierarchyRowClass = "hierarchy-entity-terrain-surface-rules";
        Editor::EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<TerrainSurfaceRulesEffect>(), std::move(traits));
    }

    // ---- Modifier Volume: the region every effect on this entity shares ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainModifierVolume>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainModifierVolume>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEmptyVolumeNotice(ctx);

        const bool globalShape = comp->Shape == TerrainVolumeShape::Global;
        Editor::AddGlobalVolumeScopeNotice(ctx.Parent, *ctx.World, ctx.Entity,
                                           AnyActiveSphericalTerrain(ctx.World));
        Editor::AddStampInGlobalVolumeNotice(ctx.Parent, *ctx.World, ctx.Entity);

        const bool splineShape = comp->Shape == TerrainVolumeShape::SplinePath
                              || comp->Shape == TerrainVolumeShape::SplineArea;
        if (splineShape)
        {
            InspectorUI::AddLine(ctx.Parent, "Region comes from the Spline on this entity");
            if (!ctx.World->GetComponent<SplineComponent>(ctx.Entity))
            {
                ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
                    "No Spline component on this entity, so this volume has no region and is "
                    "ignored. Add a Spline, or pick a Circle / Rectangle shape."));
            }
            if (AnyActiveSphericalTerrain(ctx.World))
            {
                ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
                    "Spline shapes are not baked on spherical (planet) terrains yet - this volume "
                    "affects planar / tiled terrains only."));
            }
        }

        {
            static const std::vector<Dropdown::Option> kShapeOptions = {
                {"0", "Rectangle"},
                {"1", "Circle"},
                {"2", "Spline Path"},
                {"3", "Spline Area"},
                {"4", "Global"},
            };
            auto* dd = InspectorUI::AddDropdownRow(ctx.Parent, "Shape", kShapeOptions,
                                      static_cast<int>(comp->Shape),
                                      "Region of effect. Spline shapes take their geometry from "
                                      "the Spline on this entity: Path is the swept band, Area "
                                      "also fills what a closed spline encloses. Global covers "
                                      "every terrain at full strength, with no edge.");
            dd->SetOnValueChanged([w, e, n](const std::string& value) {
                auto* c = w->GetComponent<TerrainModifierVolume>(e);
                if (!c) return;
                TerrainModifierVolume u = *c;
                int shapeAsInt = 0;
                if (std::from_chars(value.c_str(), value.c_str() + value.length(), shapeAsInt).ec == std::errc())
                    u.Shape = static_cast<TerrainVolumeShape>(shapeAsInt);
                Editor::CommitComponentUpdate(w, e, n, u);
            });
        }

        if (comp->Shape == TerrainVolumeShape::Circle)
        {
            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Radius", comp->Radius, w, e, n,
                [](TerrainModifierVolume& u, float v) { u.Radius = std::max(0.1f, v); },
                "Circle radius in world units");
        }
        else if (comp->Shape == TerrainVolumeShape::Rectangle)
        {
            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Rect Half X", comp->RectHalfX, w, e, n,
                [](TerrainModifierVolume& u, float v) { u.RectHalfX = std::max(0.1f, v); });
            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Rect Half Z", comp->RectHalfZ, w, e, n,
                [](TerrainModifierVolume& u, float v) { u.RectHalfZ = std::max(0.1f, v); });
        }
        else if (comp->Shape == TerrainVolumeShape::SplinePath)
        {
            // Only SplinePath: the route feeds a pooled flatten's per-station
            // heights, and SplineArea builds no route, so the dial would be
            // inert there. Clamped to the gather's own floor.
            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Station Spacing", comp->StationSpacing, w, e, n,
                [](TerrainModifierVolume& u, float v) {
                    u.StationSpacing = std::max(TerrainECS::kMinRouteStationSpacing, v);
                },
                "Metres between resampled stations along the route. A pooled flatten reads "
                "its per-station route heights at this granularity; the volume's footprint "
                "follows the analytic curve regardless. An upper bound: the route divides "
                "into equal intervals no longer than this.",
                {}, TerrainECS::kMinRouteStationSpacing);
        }

        // Both falloff rows are the two halves of one ramp across the shape EDGE,
        // and a global volume has no edge. Hiding them is only honest while
        // NOTHING else reads them for a global: the bake's weight ramp
        // short-circuits, and the one other reader — the Stamp mask's projection
        // extent — cannot occur, because a stamp on a global volume is refused
        // outright (notice above). A knob that does nothing is worse than an
        // absent one; a hidden knob that still does something is worse again.
        if (!globalShape)
        {
            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Falloff", comp->Falloff, w, e, n,
                [](TerrainModifierVolume& u, float v) { u.Falloff = std::max(0.0f, v); },
                "Outer half of the edge ramp: distance OUTSIDE the edge over which the "
                "weight rises from zero (0 = hard edge)");

            AddComponentFloatRowWithDrag<TerrainModifierVolume>(
                ctx.Parent, "Falloff Inward", comp->FalloffInward, w, e, n,
                [](TerrainModifierVolume& u, float v) { u.FalloffInward = std::max(0.0f, v); },
                "Inner half of the SAME ramp: distance INSIDE the edge over which the weight "
                "reaches full strength (0 = full strength right at the edge). Set both to widen "
                "a plateau's shoulder into one continuous slope.");
        }

        AddComponentFloatRowWithDrag<TerrainModifierVolume>(
            ctx.Parent, "Weight", comp->Weight, w, e, n,
            [](TerrainModifierVolume& u, float v) { u.Weight = std::clamp(v, 0.0f, 1.0f); },
            "Master strength for every effect in this volume (0-1)");

        AddComponentFloatRowWithDrag<TerrainModifierVolume>(
            ctx.Parent, "Priority", comp->Priority, w, e, n,
            [](TerrainModifierVolume& u, float v) { u.Priority = v; },
            "Order against OTHER volumes (higher = applied later). Effects INSIDE this "
            "volume are ordered by their section order instead.");

        AddTerrainEffectButton(ctx);
    });
    {
        Editor::EditorComponentTraits traits;
        traits.EnableToggleTooltip = "Switch this component on or off. Off suspends every effect in this volume.";
        traits.HostsInspectorSection = TerrainECS::IsTerrainEffectComponent;
        traits.HostedEnableToggleTooltip = "Switch this effect on or off. Off skips it and keeps its settings.";
        traits.MissingHostBadge = "No Volume";
        traits.MissingHostWarning =
            "This effect has no Terrain Modifier Volume on its entity, so it is inactive. "
            "Add a Terrain Modifier Volume (the effect nests under it, and the volume defines "
            "the region it applies to), or remove the effect.";
        Editor::EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<TerrainModifierVolume>(), std::move(traits));
    }

    // ---- Flatten effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainFlattenEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainFlattenEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);
        AddPooledCpuBakeNotice(ctx, comp->Blend);

        AddToggleRow(ctx.Parent, "Relative To Volume", comp->UseVolumeHeight,
            [w, e, n](bool v) {
                auto* c = w->GetComponent<TerrainFlattenEffect>(e);
                if (!c) return;
                TerrainFlattenEffect u = *c;
                u.UseVolumeHeight = v;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            "Measure Target Height from the volume's own height - the entity Y for circle and "
            "rectangle shapes, the spline Y at each point for spline shapes (so a sloped road "
            "flattens to its own profile). Off = an absolute world Y.");

        AddComponentFloatRowWithDrag<TerrainFlattenEffect>(
            ctx.Parent, comp->UseVolumeHeight ? "Height Offset" : "Target Height",
            comp->TargetHeight, w, e, n,
            [](TerrainFlattenEffect& u, float v) { u.TargetHeight = v; },
            comp->UseVolumeHeight ? "Offset from the volume height"
                                  : "World-space Y height to flatten to "
                                    "(planar terrains; spherical reads this as a radius)");

        AddBlendModeDropdown(ctx.Parent, comp->Blend,
            [w, e, n](TerrainModifierBlend blend) {
                auto* c = w->GetComponent<TerrainFlattenEffect>(e);
                if (!c) return;
                TerrainFlattenEffect u = *c;
                u.Blend = blend;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            true);
        AddBlendSmoothingRow(ctx.Parent, comp, w, e, n);

        AddClaimsRow(ctx, comp,
            "Stop at ground another volume has claimed, instead of grading through it. Turn this "
            "OFF for a region that owns its own ground, or its own claim holds it back. Answered "
            "PER EFFECT, in every blend mode: a run that owns its ground grades it while the path "
            "beside it stops at the claim.");
        AddPoolingRows(ctx, comp);
    });

    // ---- Ground Claim effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainGroundClaimEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainGroundClaimEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);
        AddSphereUnsupportedNotice(ctx);

        // Bounded to [0, 1]: ownership saturates, and there is nothing above full
        // ownership for the drag to reach.
        AddComponentFloatRowWithDrag<TerrainGroundClaimEffect>(
            ctx.Parent, "Strength", comp->Strength, w, e, n,
            [](TerrainGroundClaimEffect& u, float v) { u.Strength = v; },
            "How strongly this region owns its ground, from 0 (not at all) to 1 (entirely), "
            "scaled by the volume's own falloff. Any height effect set to stop at claimed ground "
            "is held back by it, whatever that effect blends with, as long as this claim applies "
            "FIRST. This effect writes no height of its own.",
            {}, 0.0f, 1.0f);
    });

    // ---- Regional grass targets ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainGrassEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainGrassEffect>(ctx.Entity);
        if (!comp) return;
        AddEffectHeader(ctx, comp);
        AddSphereUnsupportedNotice(ctx);

        // What the two multipliers scale: the grass settings on the terrain this region sits on.
        // Resolved once per inspector build (never per frame) so the field can show the absolute
        // value an author is actually choosing. With no terrain, or more than one, the absolute is
        // ambiguous and only the percentage is shown.
        const TerrainGrassBase base = ResolveTerrainGrassBase(*ctx.World);

        AddComponentMultiplierPercentRow<TerrainGrassEffect>(
            ctx.Parent, comp->HeightScale, ctx.World, ctx.Entity, ctx.ChangeNotifications,
            [](TerrainGrassEffect& u, float v) { u.HeightScale = ClampTerrainGrassEffectScale(v); },
            [base](float v) { return GrassMultiplierLabel("Height", v, base.BladeHeight, "m"); },
            GrassHeightTooltip());

        AddComponentMultiplierPercentRow<TerrainGrassEffect>(
            ctx.Parent, comp->DensityScale, ctx.World, ctx.Entity, ctx.ChangeNotifications,
            [](TerrainGrassEffect& u, float v) { u.DensityScale = ClampTerrainGrassEffectScale(v); },
            // U+00B2 as explicit UTF-8 bytes: the sources carry no BOM and the build sets no
            // /utf-8, so a literal superscript would depend on the compiler's source codepage.
            [base](float v) { return GrassMultiplierLabel("Density", v, base.BladesPerSquareMeter,
                                                          "/m\xC2\xB2"); },
            "How much of the terrain's own grass density this region keeps. 100% leaves it alone, "
            "0% clears the region. Blades keep their positions and randomness, so lowering this "
            "thins the field rather than reshuffling it, and it can never place more grass than "
            "the terrain already pays for.");
    });

    // ---- Height Offset effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainHeightOffsetEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainHeightOffsetEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);
        AddSphereUnsupportedNotice(ctx); // no sphere twin for a constant offset yet

        AddComponentFloatRowWithDrag<TerrainHeightOffsetEffect>(
            ctx.Parent, "Offset", comp->Offset, w, e, n,
            [](TerrainHeightOffsetEffect& u, float v) { u.Offset = v; },
            "Height change in world units (negative sinks, e.g. a river bed)");

        AddBlendModeDropdown(ctx.Parent, comp->Blend,
            [w, e, n](TerrainModifierBlend blend) {
                auto* c = w->GetComponent<TerrainHeightOffsetEffect>(e);
                if (!c) return;
                TerrainHeightOffsetEffect u = *c;
                u.Blend = blend;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            true);
        AddBlendSmoothingRow(ctx.Parent, comp, w, e, n);
        AddPooledCpuBakeNotice(ctx, comp->Blend);

        AddClaimsRow(ctx, comp,
            "Stop at ground another volume has claimed, instead of raising or lowering through "
            "it. Turn this OFF for a region that owns its own ground, or its own claim holds it "
            "back. Answered PER EFFECT, in every blend mode: a region that owns its ground shapes "
            "it while the one beside it stops at the claim.");
        AddPoolingRows(ctx, comp);
    });

    // ---- Noise effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainNoiseEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainNoiseEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);

        AddBlendModeDropdown(ctx.Parent, comp->Blend,
            [w, e, n](TerrainModifierBlend blend) {
                auto* c = w->GetComponent<TerrainNoiseEffect>(e);
                if (!c) return;
                TerrainNoiseEffect u = *c;
                u.Blend = blend;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            true);
        AddBlendSmoothingRow(ctx.Parent, comp, w, e, n);
        AddPooledCpuBakeNotice(ctx, comp->Blend);

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Frequency", comp->Frequency, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.Frequency = std::max(0.01f, v); },
            "Noise frequency (higher = more detail)");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Amplitude", comp->Amplitude, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.Amplitude = v; },
            "Height displacement in world units");

        AddComponentIntRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Octaves", static_cast<int>(comp->Octaves), w, e, n,
            [](TerrainNoiseEffect& u, int v) { u.Octaves = static_cast<uint32>(std::clamp(v, 1, 8)); },
            "fBM octave count (more = more fractal detail)");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Lacunarity", comp->Lacunarity, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.Lacunarity = std::max(0.1f, v); },
            "Frequency multiplier per octave (typically 2.0)");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Persistence", comp->Persistence, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.Persistence = std::clamp(v, 0.0f, 1.0f); },
            "Amplitude multiplier per octave (typically 0.5)");

        // Erosion block. Strength 0 leaves the noise untouched, so the rest of
        // these rows only matter once it is raised off zero.
        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Erosion", comp->ErosionStrength, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionStrength = std::max(0.0f, v); },
            "Gully carving strength (0 = off)");

        AddComponentIntRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Erosion Octaves", static_cast<int>(comp->ErosionOctaves), w, e, n,
            [](TerrainNoiseEffect& u, int v) { u.ErosionOctaves = static_cast<uint32>(std::clamp(v, 0, 8)); },
            "Gully octave count (more = finer branching)");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Erosion Frequency", comp->ErosionFrequency, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionFrequency = std::max(0.01f, v); },
            "Gully frequency as a multiple of the noise frequency");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Erosion Detail", comp->ErosionDetail, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionDetail = std::clamp(v, 0.01f, 4.0f); },
            "How tightly fine gullies keep to steep ground (lower = tighter)");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Gully Weight", comp->ErosionGullyWeight, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionGullyWeight = std::clamp(v, 0.0f, 2.0f); },
            "How strongly gullies branch off each other");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Edge Rounding", comp->ErosionEdgeRounding, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionEdgeRounding = std::clamp(v, 0.0f, 1.0f); },
            "0 = crisp branching creases, 1 = rounded");

        AddComponentFloatRowWithDrag<TerrainNoiseEffect>(
            ctx.Parent, "Erosion Fade", comp->ErosionFade, w, e, n,
            [](TerrainNoiseEffect& u, float v) { u.ErosionFade = std::clamp(v, 0.0f, 1.0f); },
            "Fade gullies out at peaks and valleys");

        AddClaimsRow(ctx, comp,
            "Stop at ground another volume has claimed, instead of roughening through it. Turn "
            "this OFF for a region that owns its own ground, or its own claim holds it back. "
            "Answered PER EFFECT, in every blend mode: a region that owns its ground shapes it "
            "while the one beside it stops at the claim.");
        AddPoolingRows(ctx, comp);
    });

    // ---- Stamp effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainStampEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainStampEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);
        // Also stated on the volume's own section — but at a real inspector
        // width that is several hundred pixels above this one and often off
        // screen, and every row below would otherwise read as live.
        Editor::AddStampInGlobalVolumeNotice(ctx.Parent, *ctx.World, ctx.Entity);

        AddBlendModeDropdown(ctx.Parent, comp->Blend,
            [w, e, n](TerrainModifierBlend blend) {
                auto* c = w->GetComponent<TerrainStampEffect>(e);
                if (!c) return;
                TerrainStampEffect u = *c;
                u.Blend = blend;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            true);
        AddBlendSmoothingRow(ctx.Parent, comp, w, e, n);
        AddPooledCpuBakeNotice(ctx, comp->Blend);

        AddComponentFloatRowWithDrag<TerrainStampEffect>(
            ctx.Parent, "Height Scale", comp->HeightScale, w, e, n,
            [](TerrainStampEffect& u, float v) { u.HeightScale = v; },
            "Scale applied to stamp texture height values");

        AddComponentFloatRowWithDrag<TerrainStampEffect>(
            ctx.Parent, "Rotation", comp->Rotation, w, e, n,
            [](TerrainStampEffect& u, float v) { u.Rotation = v; },
            "Rotation in degrees around Y axis");

        InspectorUI::AddAssetFieldRow(ctx.Parent, "Stamp Mask",
            comp->StampAssetGuid.ToGuid(), {AssetType::Texture},
            &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
            [w, e, n](const GUID& guid) {
                auto* c = w->GetComponent<TerrainStampEffect>(e);
                if (!c) return;
                TerrainStampEffect u = *c;
                if (guid.IsNull())
                    u.StampAssetGuid.Clear();
                else
                    u.StampAssetGuid.Set(guid);
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            ctx.Thumbnails,
            "Grayscale height mask (R channel) projected over the volume. Empty = flat raise/lower.");

        AddClaimsRow(ctx, comp,
            "Stop at ground another volume has claimed, instead of stamping through it. Turn this "
            "OFF for a region that owns its own ground, or its own claim holds it back. Answered "
            "PER EFFECT, in every blend mode: a region that owns its ground shapes it while the "
            "one beside it stops at the claim.");
        AddPoolingRows(ctx, comp);
    });

    // ---- Paint Layer effect ----
    InspectorRegistry::Get().RegisterComponentInspector<TerrainPaintLayerEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainPaintLayerEffect>(ctx.Entity);
        if (!comp) return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        AddEffectHeader(ctx, comp);
        AddSphereUnsupportedNotice(ctx); // splat modifiers need a per-face sphere splat layer

        {
            // Built per draw, not once per process: the labels are the materials the terrain
            // binds to each channel, so a rebind or a rename has to reach this list. The
            // process-lifetime static this replaces could only ever show "Layer 0 (Grass)".
            const Components::Terrain* terrain = FindTerrainForChannelLabels(w);
            TerrainMaterialLibraryAsset* library =
                terrain ? Editor::TerrainRoleMaterials::ResolveLibrary(*terrain) : nullptr;
            const std::vector<Dropdown::Option> layerOptions =
                terrain ? Editor::TerrainRoleMaterials::BuildRoleOptions(*terrain, library)
                        : DefaultChannelOptions();

            auto* dd = InspectorUI::AddDropdownRow(ctx.Parent, "Channel", layerOptions,
                                      std::clamp(static_cast<int>(comp->LayerIndex), 0,
                                                 static_cast<int>(TerrainLayers::kCount) - 1),
                                      "Which material channel this effect paints, named by the "
                                      "material the terrain binds to it.");
            dd->SetOnValueChanged([w, e, n](const std::string& value) {
                auto* c = w->GetComponent<TerrainPaintLayerEffect>(e);
                if (!c) return;
                TerrainPaintLayerEffect u = *c;
                // Clamp on write, like the grass layer picker: the value is a splat channel the
                // bake indexes, so an out-of-range one paints the wrong layer rather than the
                // one the dropdown is showing.
                u.LayerIndex = static_cast<uint32>(
                    std::clamp(std::stoi(value), 0, static_cast<int>(TerrainLayers::kCount) - 1));
                Editor::CommitComponentUpdate(w, e, n, u);
                // The brush follows the channel picked here, so a stroke started right after
                // paints the channel the user just chose rather than the built-in default.
                Editor::SceneTools::TerrainBrushTool::SetPaintLayer(u.LayerIndex);
            });
        }

        AddComponentFloatRowWithDrag<TerrainPaintLayerEffect>(
            ctx.Parent, "Strength", comp->Strength, w, e, n,
            [](TerrainPaintLayerEffect& u, float v) { u.Strength = std::clamp(v, 0.0f, 1.0f); },
            "Paint intensity (0-1)");

        AddToggleRow(ctx.Parent, "Replace", comp->Replace,
            [w, e, n](bool v) {
                auto* c = w->GetComponent<TerrainPaintLayerEffect>(e);
                if (!c) return;
                TerrainPaintLayerEffect u = *c;
                u.Replace = v;
                Editor::CommitComponentUpdate(w, e, n, u);
            },
            "When enabled, replaces existing weights entirely. When disabled, adds and renormalizes.");

        AddClaimsRow(ctx, comp,
            "Leave ground another volume has claimed with the material that volume painted, "
            "instead of painting over it. The claim must apply FIRST. The claimant has to paint "
            "the material itself - the splatmap starts empty every bake and an empty texel comes "
            "out as the first material, so a claim with no paint under it leaves a patch of the "
            "wrong material rather than preserving anything.");
        AddClaimedGroundNeedsPaintNotice(ctx, comp->RespectClaims);
    });

    // ---- Surface Rules effect ----
    // The body is TerrainRuleRows.cpp's; this registration resolves what the rows
    // cannot see from a rule alone — which terrain names the channels and how
    // tall it is, the metres domain a height band spans.
    InspectorRegistry::Get().RegisterComponentInspector<TerrainSurfaceRulesEffect>(
        [](const InspectorContext& ctx)
    {
        auto* comp = ctx.World->GetComponent<TerrainSurfaceRulesEffect>(ctx.Entity);
        if (!comp) return;

        AddEffectHeader(ctx, comp);
        AddSphereUnsupportedNotice(ctx); // splat effect: needs a per-face sphere splat layer

        const Components::Terrain* terrain = FindTerrainForChannelLabels(ctx.World);
        TerrainMaterialLibraryAsset* library =
            terrain ? Editor::TerrainRoleMaterials::ResolveLibrary(*terrain) : nullptr;

        // Block-scoped rows come before the per-rule rows: rendered below them,
        // "Stops at Claimed Ground" reads as a field of the last rule.
        AddClaimsRow(ctx, comp,
            "Leave ground another volume has claimed with the material that volume painted, "
            "instead of ruling over it. Masks the whole block - a rule block is one effect at one "
            "slot - and the claim must apply FIRST. The claimant has to paint the material "
            "itself: the splatmap starts empty every bake and an empty texel comes out as the "
            "first material, so a claim with no paint under it leaves a patch of the wrong "
            "material rather than preserving anything.");
        AddClaimedGroundNeedsPaintNotice(ctx, comp->RespectClaims);
        AddClaimRespectingRulesCpuBakeNotice(ctx, comp->RespectClaims);

        AddTerrainSurfaceRuleRows(ctx, *comp, terrain, library);
    });

}

} // namespace GameEngine
