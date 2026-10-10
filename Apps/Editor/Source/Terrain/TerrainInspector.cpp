#include "Terrain/TerrainInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Core/Engine.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "SceneView/TerrainBrushTool.h"
#include "Terrain/TerrainBaseSourceSection.h"
#include "Terrain/TerrainGrassVocabulary.h"
#include "Terrain/TerrainLayers.h"
#include "Terrain/TerrainMaterialLibraryMint.h"
#include "Terrain/TerrainMaterialRecord.h" // kMaxTerrainMaterials — the slot ID domain
#include "Terrain/TerrainRoleMaterials.h"
#include "TerrainECS/TerrainGrassAlpha.h"
#include "TerrainGrass/GrassPlacementModel.h"       // the fit the Range row reports
#include "TerrainGrass/TerrainGrassRenderFeature.h" // kMaxInstancesPerView
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "UndoRedo/UndoRedoService.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/TerrainSizingNotice.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "Inspectors/InspectorTextureSlotPreview.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr float kColorSwatchSize = 20.0f;
constexpr uint32_t kColorSwatchBorder = 0xFF555555u;

// TerrainGrass::WindDirection is stored in radians, which is what the shader wants and what nobody
// authors: the panel drags it in degrees.
constexpr float kGrassRadToDeg = 57.2957795f;
constexpr float kGrassDegToRad = 0.0174532925f;

// A planar terrain's side, in metres: at least 1, and at most the extent its sample density can
// tile (TerrainECS::MaxTerrainExtentMetres), past which tiles never become resident.
float ClampTerrainSide(float metres, float samplesPerMeter)
{
    return std::clamp(metres, 1.0f, TerrainECS::MaxTerrainExtentMetres(samplesPerMeter));
}

// A planar terrain's density, in samples per metre: at least 0.01, and at most what its longer
// side can tile (TerrainECS::MaxTerrainSamplesPerMeter), the same limit seen from the other input.
float ClampTerrainDensity(float samplesPerMeter, const Components::Terrain& terrain)
{
    const float densest = TerrainECS::MaxTerrainSamplesPerMeter(terrain.SizeX, terrain.SizeZ);
    return std::clamp(samplesPerMeter, 0.01f, std::max(0.01f, densest));
}

// One of this component's sections. Titles repeat across the two terrain inspectors and across
// nesting levels — "Grass" is both a TerrainGrass section and a material layer — so the key that
// remembers whether a section is open is the panel plus the title, not the title alone.
static UIElement* AddSection(UIElement* parent, std::string_view keyScope, std::string_view title,
                             bool expandedByDefault = true)
{
    Foldout* section = InspectorUI::AddComponentSection(
        parent, std::string(keyScope) + "/" + std::string(title), title, expandedByDefault);
    return section ? section->GetContentContainer() : nullptr;
}

static uint32_t OpaqueArgb(uint32_t argb)
{
    return 0xFF000000u | (argb & 0x00FFFFFFu);
}


template <typename Setter>
static void AddGrassTextureField(UIElement* parent,
                                 const char* label,
                                 const Components::TerrainGrass* grass,
                                 ECS::World* w,
                                 ECS::EntityHandle e,
                                 Editor::EditorChangeNotifications* n,
                                 IThumbnailProvider* thumbnails,
                                 const GUID& guid,
                                 Setter setter,
                                 const char* tooltip,
                                 // Invoked after a change that can alter which rows BELONG in the
                                 // section — binding an albedo or alpha map decides whether the
                                 // alpha controls are live. Empty for slots that gate nothing.
                                 std::function<void()> requestRefresh)
{
    if (!parent || !grass)
        return;

    const auto commit = [w, e, n, setter, requestRefresh](const GUID& picked)
    {
        auto* comp = w ? w->GetComponent<Components::TerrainGrass>(e) : nullptr;
        if (!comp)
            return;
        Components::TerrainGrass updated = *comp;
        setter(updated, picked);
        Editor::CommitComponentUpdate(w, e, n, updated);
        if (requestRefresh)
            requestRefresh();
    };

    InspectorUI::AddAssetFieldRow(parent, label, guid,
        {AssetType::Texture},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        commit,
        thumbnails,
        tooltip);

    InspectorUI::AddTextureSlotPreview(parent, guid, commit);
}

static std::string FormatGrassColor(uint32_t argb)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  static_cast<unsigned>((argb >> 16) & 0xFFu),
                  static_cast<unsigned>((argb >> 8) & 0xFFu),
                  static_cast<unsigned>(argb & 0xFFu));
    return buf;
}

// Colour chip. The cursor is left to the caller: a swatch that opens a picker sets Pointer,
// a read-only one (the layer fallback tint) keeps the default arrow.
static void StyleColorSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kColorSwatchSize))
        .Set(Style::Height, StyleLength::Px(kColorSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kColorSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kColorSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kColorSwatchBorder, kColorSwatchBorder, kColorSwatchBorder, kColorSwatchBorder})
        .Set(Style::BackgroundColor, OpaqueArgb(argb));
}

static uint32_t GetGrassColor(const Components::TerrainGrass& grass, int slot)
{
    switch (slot)
    {
    case 0: return grass.RootColor;
    case 1: return grass.TipColor;
    case 2: return grass.BacklightColor;
    default: return 0xFF000000u;
    }
}

static void SetGrassColor(Components::TerrainGrass& grass, int slot, uint32_t argb)
{
    const uint32_t color = OpaqueArgb(argb);
    switch (slot)
    {
    case 0: grass.RootColor = color; break;
    case 1: grass.TipColor = color; break;
    case 2: grass.BacklightColor = color; break;
    default: break;
    }
}

// One colour chip plus its caption, appended to an existing field container. Factored out because
// the palette no longer lives in one row: the canopy colour is a top-level look dial, the root
// colour belongs beside the toggle that decides whether it is read at all, and the backlight colour
// shares a row with the strength it tints.
static void AddGrassColorSwatch(UIElement* field,
                                int slot,
                                const char* name,
                                ECS::World* w,
                                ECS::EntityHandle e,
                                Editor::EditorChangeNotifications* n,
                                OpenColorPickerWindowFn openPicker)
{
    auto* comp = w ? w->GetComponent<Components::TerrainGrass>(e) : nullptr;
    if (!field || !comp)
        return;

    auto group = std::make_unique<UIElement>();
    group->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(4.0f));
    UIElement* groupRaw = group.get();

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    const uint32_t initialArgb = GetGrassColor(*comp, slot);
    StyleColorSwatch(swatchRaw, initialArgb);
    swatchRaw->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    swatchRaw->SetTooltip(std::string(name) + " " + FormatGrassColor(initialArgb));
    groupRaw->AddChild(std::move(swatch));

    auto label = std::make_unique<Label>();
    label->AddClass("inspector-text");
    label->SetText(name);
    label->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* labelRaw = label.get();
    groupRaw->AddChild(std::move(label));

    auto clickHandler = [w, e, n, openPicker, swatchRaw, slot, label = std::string(name)](UIEvent& ev)
    {
        if (ev.Button != 0)
            return;
        ev.Stop();
        if (!openPicker)
            return;
        auto* current = w->GetComponent<Components::TerrainGrass>(e);
        if (!current)
            return;
        const uint32_t originalArgb = GetGrassColor(*current, slot);

        auto apply = [w, e, n, swatchRaw, slot, label](uint32_t argb, bool commit)
        {
            auto* c = w->GetComponent<Components::TerrainGrass>(e);
            if (!c)
                return;
            Components::TerrainGrass updated = *c;
            SetGrassColor(updated, slot, argb);
            if (commit)
                Editor::CommitComponentUpdate(w, e, n, updated);
            else
                Editor::PreviewComponentUpdate(w, e, n, updated);
            const uint32_t newColor = GetGrassColor(updated, slot);
            StyleColorSwatch(swatchRaw, newColor);
            swatchRaw->SetTooltip(label + " " + FormatGrassColor(newColor));
        };

        ColorPickerCallbacks cbs;
        cbs.onApply = [apply](uint32_t argb, float) { apply(argb, true); };
        cbs.onCancel = [apply, originalArgb]() { apply(originalArgb, true); };
        cbs.onValueChanging = [apply](uint32_t argb, float) { apply(argb, false); };
        openPicker(originalArgb, 1.0f, std::move(cbs));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    labelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    field->AddChild(std::move(group));
}

// A labelled row carrying one colour chip.
static void AddGrassColorRow(UIElement* parent,
                             const char* rowLabel,
                             const char* tooltip,
                             int slot,
                             const char* swatchName,
                             ECS::World* w,
                             ECS::EntityHandle e,
                             Editor::EditorChangeNotifications* n,
                             OpenColorPickerWindowFn openPicker)
{
    using namespace InspectorDrag;
    if (!parent || !w || !w->GetComponent<Components::TerrainGrass>(e))
        return;

    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, rowLabel, tooltip);
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(8.0f));
    AddGrassColorSwatch(field, slot, swatchName, w, e, n, openPicker);
}

// What an unbound layer actually looks like on the surface, said out loud rather than left as an
// empty slot: the fallback is a real shading path (built-in tint + procedural variation), not a
// missing texture. Only shown while the slot is empty — binding a texture neutralises the tint.
static void AddFallbackTintRow(UIElement* parent, uint32_t layer, const std::string& id,
                               bool showInfoCards)
{
    using namespace InspectorDrag;
    if (!parent)
        return;

    UIElement* row = InspectorUI::AddRow(parent);
    row->SetId(id);
    InspectorUI::AddLabel(row, "Unbound",
             "No texture bound: the surface shades this layer with a built-in colour plus "
             "procedural variation.");
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(8.0f));

    auto swatch = std::make_unique<UIElement>();
    StyleColorSwatch(swatch.get(), TerrainLayers::FallbackTintSwatchArgb(layer));
    field->AddChild(std::move(swatch));

    auto label = std::make_unique<Label>();
    label->AddClass("inspector-text");
    label->SetText("Built-in tint (procedural)");
    field->AddChild(std::move(label));

    if (showInfoCards)
        InspectorUI::AddInfoCard(
            parent, "Assign a texture to switch this layer to the textured path.");
}

// Bind a channel role to one of the library's materials, or create a material and bind it in
// one gesture. Both write the component's role slot, so the picker and what the surface shades are
// the same fact; create-new additionally appends to the library document. The two writes are one
// compound undo entry, because one click should cost one undo.
//
// The row itself is built by TerrainRoleMaterials — the panel supplies the commit, which is a
// component write this is the only place for.
static void WireRoleMaterialPicker(Dropdown* picker, uint32_t role, const InspectorContext& ctx)
{
    namespace Roles = Editor::TerrainRoleMaterials;
    using namespace InspectorDrag;
    if (!picker)
        return;

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    auto requestRefresh = ctx.RequestInspectorRefresh;

    picker->SetOnValueChanged(
        [w, e, n, undo, role, requestRefresh](const std::string& value)
        {
            // Re-resolve rather than capturing the asset: the callback outlives this build, and
            // the terrain may have been re-bound or the library evicted in between.
            auto* live = w ? w->GetComponent<Components::Terrain>(e) : nullptr;
            TerrainMaterialLibraryAsset* library =
                live ? Roles::ResolveLibrary(*live) : nullptr;
            if (!library)
                return;

            // A bind option's value is the slot it binds; a create has to allocate one, which a
            // library holding all 256 cannot do.
            const bool create = value == Roles::kCreateMaterialOptionValue;
            const uint32_t requested = create ? library->NextFreeSlotId()
                                              : static_cast<uint32_t>(std::stoi(value));
            if (requested >= Terrain::kMaxTerrainMaterials)
                return;

            if (undo)
                undo->BeginCompound(create ? "Create Terrain Material" : "Bind Terrain Material");

            // Which slot a created material took comes back from the add, which resolves it
            // against the list it is mutating — the path the library panel and the grid's add tile
            // share.
            const uint32_t slot = create ? Roles::AddLibraryMaterial(library, undo, {}) : requested;
            if (slot >= Terrain::kMaxTerrainMaterials)
            {
                if (undo)
                    undo->EndCompound();
                return;
            }

            CommitComponentWithUndo<Components::Terrain>(
                w, e, n, undo, "Bind Terrain Material",
                [role, slot](Components::Terrain& u)
                { u.LayerRoleSlot[role] = static_cast<uint8>(slot); });

            if (undo)
                undo->EndCompound();

            // Creating a material is an act of intent to paint with it, so the channel it lands on
            // becomes the brush's channel — R3's "added to the terrain for QoL" half.
            if (create)
                Editor::SceneTools::TerrainBrushTool::SetPaintLayer(role);

            // Every row's option list is the library, and the Paint Channel picker names channels
            // by the material bound to them, so a create or a re-bind leaves both stale.
            if (requestRefresh)
                requestRefresh();
        });
}

// One layer's authoring card: the texture slot, its tiling, and the hex-tiling opt-in. Every
// commit routes through CommitComponentWithUndo, so an edit is one undo entry AND still fires the
// Commit notification the terrain's per-frame re-provision reads (CBTUpdateSystem republishes the
// layer palette from the component every frame — shading-only, no rebake).
//
// These rows are the pre-library authoring path and are shown only while no library resolves: the
// component's per-layer fields are read by exactly one code path, and a bound library replaces it.
// Showing them under a library would offer controls that change nothing. The card is titled by the
// layer's semantic name, which with no library is all a layer has to be called.
static void AddPreLibraryLayerCard(UIElement* parent, uint32_t layer, const InspectorContext& ctx)
{
    using namespace InspectorDrag;
    auto* terrain = ctx.World ? ctx.World->GetComponent<Components::Terrain>(ctx.Entity) : nullptr;
    if (!parent || !terrain || layer >= TerrainLayers::kCount)
        return;

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    // One heading style across the panel, whatever level a block sits at: a layer card heads the
    // same way Material Layers above it does — and remembers its own state, because binding a
    // texture on this very card requests the rebuild that would otherwise reopen it.
    UIElement* content = AddSection(parent, "MaterialLayer", TerrainLayers::kNames[layer]);
    if (!content)
        return;

    const std::string idPrefix = "terrain-layer-" + std::to_string(layer);

    AssetField* textureField = InspectorUI::AddAssetFieldRow(content, "Texture",
        terrain->LayerAlbedoTexture[layer].ToGuid(),
        {AssetType::Texture},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [w, e, n, undo, layer, requestRefresh = ctx.RequestInspectorRefresh](const GUID& guid)
        {
            CommitComponentWithUndo<Components::Terrain>(
                w, e, n, undo, "Change Terrain Layer Texture",
                [layer, guid](Components::Terrain& u) { TerrainLayers::SetAlbedo(u, layer, guid); });
            // Binding or clearing flips the card between the texture path and the built-in-tint
            // notice, so the rows themselves change — rebuild rather than leave a stale notice.
            if (requestRefresh)
                requestRefresh();
        },
        ctx.Thumbnails,
        "Albedo texture for this layer, projected triplanar. Empty leaves the layer on the "
        "built-in tint.");
    if (textureField)
        textureField->SetId(idPrefix + "-texture");

    InspectorUI::AddTextureSlotPreview(content, terrain->LayerAlbedoTexture[layer].ToGuid(),
        [w, e, n, undo, layer, requestRefresh = ctx.RequestInspectorRefresh](const GUID& dropped)
        {
            CommitComponentWithUndo<Components::Terrain>(
                w, e, n, undo, "Change Terrain Layer Texture",
                [layer, dropped](Components::Terrain& u)
                { TerrainLayers::SetAlbedo(u, layer, dropped); });
            if (requestRefresh)
                requestRefresh();
        });

    if (!TerrainLayers::IsTextured(*terrain, layer))
        AddFallbackTintRow(content, layer, idPrefix + "-fallback", ctx.ShowInfoCards);

    FloatField* tilingField = AddComponentFloatRowWithDrag<Components::Terrain>(
        content, "Tiling", terrain->LayerTiling[layer], w, e, n, undo,
        "Change Terrain Layer Tiling",
        [layer](Components::Terrain& u, float v) { TerrainLayers::SetTiling(u, layer, v); },
        1.0f,
        "Repeat rate for this layer against the terrain's Base Tiling (2 = twice as many "
        "repeats, so half the feature size). 0 = use Base Tiling unchanged.",
        {}, 0.0f, 64.0f);
    if (tilingField)
        tilingField->SetId(idPrefix + "-tiling");

    Toggle* hexToggle = AddToggleRow(content, "Hex Tiling",
        TerrainLayers::IsHexTiling(*terrain, layer),
        [w, e, n, undo, layer](bool v)
        {
            CommitComponentWithUndo<Components::Terrain>(
                w, e, n, undo, "Toggle Terrain Layer Hex Tiling",
                [layer, v](Components::Terrain& u) { TerrainLayers::SetHexTiling(u, layer, v); });
        },
        "Breaks visible repetition by blending three hash-rotated copies of the texture on a "
        "hexagonal lattice (Mikkelsen hex-tiling). Costs about 3x this layer's texture samples.");
    if (hexToggle)
        hexToggle->SetId(idPrefix + "-hex");
}

// The way out for a terrain that has no library: mint one from the per-layer fields it already
// shades with. Not a change of appearance — the minted materials reproduce the records the legacy
// path produces — it is the moment those four materials become nameable, editable and extendable.
// A save mints one anyway; this is for a user who wants it now.
static void AddCreateLibraryRow(UIElement* content, const InspectorContext& ctx)
{
    using namespace InspectorDrag;
    if (!content)
        return;

    if (ctx.ShowInfoCards)
        InspectorUI::AddInfoCard(
            content,
            "This terrain has no material library. Creating one captures the four channels it "
            "already shades with, so it looks the same and becomes editable.");

    UIElement* row = InspectorUI::AddRow(content);
    InspectorUI::AddLabel(row, "");
    UIElement* field = InspectorUI::AddFieldContainer(row);

    auto button = std::make_unique<Button>();
    button->SetId("terrain-create-material-library");
    // Filled, like Add Component — `secondary` would make it a transparent outline, a different
    // kind of button from the panel's other actions. Only its size is scaled down.
    button->AddClass("small");
    button->SetText("Create Material Library");
    button->SetTooltip("Writes a .terrainmatlib into the project's asset root holding this "
                       "terrain's four channels, and binds it.");
    button->RegisterEventHandler(kEventButtonClick, [w = ctx.World, e = ctx.Entity, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](UIEvent&)
        {
            auto& engine = EngineCore::GetInstance();
            if (!w || !engine.IsInitialized())
                return;
            // The project's asset root, not the repo: a library resolved from anywhere else would
            // not survive being packaged with the project.
            Editor::MintTerrainMaterialLibraryFor(*w, e, engine.GetResolvedAssetRoot(),
                                                  "Terrain Materials", engine.GetAssetManager(),
                                                  undo);
            if (requestRefresh)
                requestRefresh();
        });
    field->AddChild(std::move(button));
}

// Which channel the terrain brush paints into, named by the material bound to it rather than by a
// bare index. This is the only writer of the brush's paint channel: without it the brush painted
// channel 2 forever, whatever the picker showed.
static void AddPaintChannelRow(UIElement* content, const InspectorContext& ctx,
                               const Components::Terrain& terrain,
                               const TerrainMaterialLibraryAsset* library)
{
    namespace Roles = Editor::TerrainRoleMaterials;
    using namespace InspectorDrag;
    if (!content)
        return;

    const uint32_t current = Editor::SceneTools::TerrainBrushTool::GetPaintLayer();
    Dropdown* picker = InspectorUI::AddDropdownRow(
        content, "Paint Channel", Roles::BuildRoleOptions(terrain, library),
        static_cast<int>(std::min(current, TerrainLayers::kCount - 1u)),
        "Which channel the terrain brush paints into, and so which material a paint stroke "
        "reveals. Shared by every scene view.");
    if (!picker)
        return;
    picker->SetId("terrain-paint-channel");

    picker->SetOnValueChanged(
        [](const std::string& value)
        {
            Editor::SceneTools::TerrainBrushTool::SetPaintLayer(
                static_cast<uint32_t>(std::stoi(value)));
        });
}

static void AddMaterialLayersSection(const InspectorContext& ctx, const Components::Terrain& terrain)
{
    using namespace InspectorDrag;
    if (!ctx.Parent)
        return;

    UIElement* content = AddSection(ctx.Parent, "Terrain", "Material Layers", /*expandedByDefault=*/false);
    if (!content)
        return;

    FloatField* globalTiling = AddComponentFloatRowWithDrag<Components::Terrain>(
        content, "Base Tiling", terrain.MaterialTiling, ctx.World, ctx.Entity,
        ctx.ChangeNotifications, ctx.Undo, "Change Terrain Base Tiling",
        [](Components::Terrain& u, float v) { u.MaterialTiling = std::max(v, 0.01f); },
        10.0f,
        "World-space size of one texture repeat, in metres — larger = bigger features. Every "
        "material scales from this: a material's own Tiling multiplies its repeat rate, so "
        "Tiling 2 repeats twice as often and halves the feature size.",
        {}, 0.01f, 1000.0f);
    if (globalTiling)
        globalTiling->SetId("terrain-material-tiling");

    AssetField* libraryField = InspectorUI::AddAssetFieldRow(
        content, "Material Library", terrain.MaterialLibraryGuid.ToGuid(),
        {AssetType::TerrainMaterialLibrary},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](const GUID& guid)
        {
            // Set-only: an empty library reference means "not migrated yet", and the
            // next scene save mints one back, so a clear would be an edit that undid
            // itself. The field offers no clear button for the same reason.
            if (guid.IsNull())
                return;
            CommitComponentWithUndo<Components::Terrain>(
                w, e, n, undo, "Change Terrain Material Library",
                [guid](Components::Terrain& u) { u.MaterialLibraryGuid.Set(guid); });
            // Binding swaps the section between the material grid and the pre-library
            // per-layer cards, so the section's rows themselves change.
            if (requestRefresh)
                requestRefresh();
        },
        ctx.Thumbnails,
        "The material list this terrain shades from. Several terrains can share one. A terrain "
        "with none shades from the per-channel textures below until it gets one — which the next "
        "scene save does for it.");
    if (libraryField)
    {
        libraryField->SetId("terrain-material-library");
        libraryField->SetClearable(false);
    }

    TerrainMaterialLibraryAsset* library = Editor::TerrainRoleMaterials::ResolveLibrary(terrain);
    if (terrain.MaterialLibraryGuid.IsNull())
        AddCreateLibraryRow(content, ctx);
    Editor::TerrainRoleMaterials::AddOpenLibraryRow(content, library, ctx.PingAsset);
    AddPaintChannelRow(content, ctx, terrain, library);

    // One statement for the whole section, not one per channel: every channel says the same thing
    // about the same library, and the button above is the action it names.
    if (library && ctx.ShowInfoCards)
        InspectorUI::AddInfoCard(
            content,
            "Tint, textures and per-material tiling belong to the material — Edit Materials opens "
            "the library. Base Tiling above scales every material.");

    // A bound terrain shades from the library's materials, so those are what the section shows;
    // the four channel roles are a binding over them and sit in the block below. An
    // unmigrated one still authors four textures, tilings and hex toggles per channel, which needs
    // the cards.
    if (library)
    {
        Editor::TerrainRoleMaterials::AddMaterialGrid(
            content, library,
            [w = ctx.World, e = ctx.Entity, undo = ctx.Undo,
             requestRefresh = ctx.RequestInspectorRefresh]
            {
                // Re-resolve rather than capturing the asset: the handler outlives this build, and
                // the asset manager's loaded map is a library's only strong owner — opening
                // another project ejects the very asset this grid was built from.
                namespace Roles = Editor::TerrainRoleMaterials;
                auto* live = w ? w->GetComponent<Components::Terrain>(e) : nullptr;
                TerrainMaterialLibraryAsset* resolved = live ? Roles::ResolveLibrary(*live)
                                                             : nullptr;
                if (!resolved)
                    return;
                (void)Roles::AddLibraryMaterial(resolved, undo, requestRefresh);
            },
            ctx.PingAsset);

        const Editor::TerrainRoleMaterials::LayerRoleBlock roleBlock =
            Editor::TerrainRoleMaterials::AddLayerRoleBlock(content, terrain, library);
        for (size_t role = 0; role < roleBlock.Pickers.size(); ++role)
            WireRoleMaterialPicker(roleBlock.Pickers[role], static_cast<uint32_t>(role), ctx);

        if (library->NextFreeSlotId() == TerrainMaterialLibraryAsset::kInvalidSlotId &&
            ctx.ShowInfoCards)
        {
            InspectorUI::AddInfoCard(content,
                                     "All 256 slot IDs are taken, so no new material can be "
                                     "created. Remove a retired material in the library to free "
                                     "one.");
        }
    }
    else
    {
        for (uint32_t layer = 0; layer < TerrainLayers::kCount; ++layer)
            AddPreLibraryLayerCard(content, layer, ctx);
    }
}

} // namespace

void RegisterTerrainInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* terrain = ctx.World->GetComponent<Components::Terrain>(ctx.Entity);
        if (!terrain)
        {
            InspectorUI::AddLine(ctx.Parent, "(Terrain missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        using namespace InspectorDrag;

        // Helper: invalidate cached morph ranges after terrain config changes.
        auto invalidateMorphRanges = [](const Components::Terrain& updated)
        {
            if (auto* svc = TerrainECS::TerrainService::TryGet())
            {
                TerrainECS::TerrainHandle h{updated.TerrainDataHandle, updated.TerrainDataGeneration};
                if (auto* data = svc->GetTerrainData(h))
                    data->MorphRangesComputed = false;
            }
        };

        // ---- Terrain Type ----
        // What kind of terrain this is, and so which of the sections below even apply. It heads the
        // panel because everything under it is conditional on the answer.
        UIElement* terrainType = AddSection(ctx.Parent, "Terrain", "Terrain Type");
        {
            static const std::vector<Dropdown::Option> kDomainOptions = {
                {"0", "Planar"},
                {"1", "Spherical"},
            };
            int selectedIdx = (terrain->Domain == Components::TerrainDomain::Spherical) ? 1 : 0;
            auto* domainDD = InspectorUI::AddDropdownRow(terrainType, "Domain", kDomainOptions, selectedIdx,
                "Planar = heightmap terrain. Spherical = cube-sphere planet (radius + relief below).");
            domainDD->SetOnValueChanged(
                [w, e, n, requestRefresh = ctx.RequestInspectorRefresh, invalidateMorphRanges](const std::string& value) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    const bool nowSpherical = std::stoi(value) == 1;
                    updated.Domain = nowSpherical ? Components::TerrainDomain::Spherical
                                                  : Components::TerrainDomain::Planar;
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    // Invariant: a spherical terrain carries the base-relief component so it
                    // renders (with defaults) and is tunable via the Planet Relief section;
                    // a planar one carries none. Mirrors ProvisionTerrainEntity's gating.
                    if (nowSpherical)
                    {
                        if (!w->GetComponent<Components::TerrainPlanetRelief>(e))
                            w->AddComponentImmediate(e, Components::TerrainPlanetRelief{});
                    }
                    else if (w->GetComponent<Components::TerrainPlanetRelief>(e))
                    {
                        w->RemoveComponentImmediate<Components::TerrainPlanetRelief>(e);
                    }
                    invalidateMorphRanges(updated);
                    if (requestRefresh)
                        requestRefresh();
                });
        }

        // ---- Base ----
        // Where a heightmap plane's heights come from. A planet's come from its Planet fields.
        if (terrain->Domain == Components::TerrainDomain::Planar)
            AddTerrainBaseSourceSection(ctx);

        // ---- Terrain Settings ----
        // How big this terrain is, in the terms its domain measures size by: a heightmap plane's
        // extent and sample rate, or a planet's radius. The two sets are exclusive — a plane's
        // dimensions mean nothing on a sphere, and changing them there was a silent no-op that
        // confused users. The Domain row above requests a rebuild, so this re-evaluates on a switch.
        constexpr float kNoDefault = std::numeric_limits<float>::quiet_NaN();
        UIElement* settings = AddSection(ctx.Parent, "Terrain", "Terrain Settings");
        if (terrain->Domain == Components::TerrainDomain::Planar)
        {
            AddFloatRowWithDrag(settings, "Size X", terrain->SizeX,
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SizeX = ClampTerrainSide(v, updated.SamplesPerMeter);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                },
                [w, e, n, invalidateMorphRanges](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SizeX = ClampTerrainSide(v, updated.SamplesPerMeter);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    invalidateMorphRanges(updated);
                },
                kNoDefault, "World-space width of the heightmap terrain (metres). Planar only.");

            AddFloatRowWithDrag(settings, "Size Z", terrain->SizeZ,
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SizeZ = ClampTerrainSide(v, updated.SamplesPerMeter);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                },
                [w, e, n, invalidateMorphRanges](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SizeZ = ClampTerrainSide(v, updated.SamplesPerMeter);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    invalidateMorphRanges(updated);
                },
                kNoDefault, "World-space depth of the heightmap terrain (metres). Planar only.");

            AddFloatRowWithDrag(settings, "Height Scale", terrain->HeightScale,
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.HeightScale = std::max(0.01f, v);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                },
                [w, e, n, invalidateMorphRanges](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.HeightScale = std::max(0.01f, v);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    invalidateMorphRanges(updated);
                },
                kNoDefault,
                "Vertical scale of the heightmap (metres). Planar only — a planet's relief comes "
                "from the Planet fields.");

            AddFloatRowWithDrag(settings, "Samples Per Meter", terrain->SamplesPerMeter,
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SamplesPerMeter = ClampTerrainDensity(v, updated);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                },
                [w, e, n, invalidateMorphRanges](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SamplesPerMeter = ClampTerrainDensity(v, updated);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    invalidateMorphRanges(updated);
                },
                kNoDefault,
                "Heightmap resolution (samples per metre). Planar only — a planet refines "
                "procedurally from Planet Radius.");

            AddFloatRowWithDrag(settings, "Streaming Radius", terrain->StreamingRadius,
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.StreamingRadius = std::max(0.0f, v);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                },
                [w, e, n](float v) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.StreamingRadius = std::max(0.0f, v);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                },
                kNoDefault, "Tile streaming radius, metres (0 = auto from LOD ranges). Planar only.");

            // What the three rows above actually bought. Rendered here, at the rows that decide
            // it, because a summary elsewhere in the panel is hundreds of pixels from the drag.
            Editor::AddTerrainSizingNotice(settings,
                                           TerrainECS::DeriveTerrainSizingPlan(
                                               terrain->SizeX, terrain->SizeZ,
                                               terrain->SamplesPerMeter));

            // Water the terrain sits under (Terrain::SeaLevel): one toggle, and the height only
            // while it is set, so an unset level never shows the sentinel as a number.
            const bool seaLevelSet = terrain->SeaLevel > Components::kNoTerrainSeaLevel;
            AddToggleRow(settings, "Sea Level", seaLevelSet,
                [w, e, n, requestRefresh = ctx.RequestInspectorRefresh](bool on) {
                    auto* comp = w->GetComponent<Components::Terrain>(e);
                    if (!comp) return;
                    Components::Terrain updated = *comp;
                    updated.SeaLevel = on ? 0.0f : Components::kNoTerrainSeaLevel;
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    if (requestRefresh)
                        requestRefresh();
                },
                "Height of a calm water surface the terrain sits under, such as a water mesh. The "
                "renderer keeps the seabed hidden below it coarse instead of spending its triangle "
                "budget there. An Ocean entity supplies its own sea level and overrides this.");
            if (seaLevelSet)
            {
                AddFloatRowWithDrag(settings, "Sea Level Height", terrain->SeaLevel,
                    [w, e, n](float v) {
                        auto* comp = w->GetComponent<Components::Terrain>(e);
                        if (!comp) return;
                        Components::Terrain updated = *comp;
                        updated.SeaLevel = v;
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    },
                    [w, e, n](float v) {
                        auto* comp = w->GetComponent<Components::Terrain>(e);
                        if (!comp) return;
                        Components::Terrain updated = *comp;
                        updated.SeaLevel = v;
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    },
                    kNoDefault,
                    "World Y of the water surface, metres. Seabed more than a couple of metres "
                    "below it is treated as hidden; the shoreline keeps full detail.");
            }
        }
        else
        {
            AddComponentFloatRowWithDrag<Components::Terrain>(
                settings, "Planet Radius", terrain->PlanetRadius, w, e, n,
                [](Components::Terrain& u, float v) { u.PlanetRadius = std::max(1.0f, v); },
                "Sphere radius in metres, centered at the entity origin. MaxDepth auto-scales with "
                "it. A planet has no Size or Samples Per Meter — it refines procedurally.",
                {}, 1.0f, 100000.0f);
        }

        // ---- Material Layers ----
        // What the surface shades with: the bound library's materials, and under them the
        // channel roles they bind to. Which channel shows where is decided by the splat --
        // written by the terrain's authored surface rule rows and paint effects -- while the
        // materials decide what each one looks like.
        AddMaterialLayersSection(ctx, *terrain);

        // ---- Advanced ----
        // Tuning and diagnostics, shut by default: the dials below are read far less often than
        // they are scrolled past.
        UIElement* advanced = AddSection(ctx.Parent, "Terrain", "Advanced", false);

        AddComponentFloatSliderRow<Components::Terrain>(
            advanced, "Target Pixel Error", terrain->TargetPixelError, 1.0f, 32.0f, w, e, n,
            undo, "Change Terrain Target Pixel Error",
            [](Components::Terrain& u, float v) { u.TargetPixelError = std::clamp(v, 1.0f, 32.0f); },
            "Longest triangle edge on screen, in pixels at 1080 rows; it scales with the view's "
            "height, so every resolution of the same aspect ratio draws the same triangle count. "
            "Lower = finer distant ground, at about 1/value^2 triangles. Default 11. On a "
            "heightfield terrain, near the camera the triangles stop at the height data's own "
            "spacing; a planet refines procedurally.");

        // CBT debug visualization: tints each bisector triangle so the live LEB bisection
        // structure is visible (planar + planet). Not serialized — a transient authoring aid.
        {
            // One option per TerrainDebugView value: a mode the shader implements but the list
            // omits would be rendered while the dropdown showed a different mode's label.
            static const std::vector<Dropdown::Option> kDebugViewOptions = {
                {"0", "Off"},
                {"1", "Facets (CBT triangulation)"},
                {"2", "Atlas slots (resident window)"},
            };
            // The options carry ENUM VALUES; the dropdown wants a list INDEX. Resolving by value
            // keeps the list order independent of the enum numbering.
            const std::string debugValue = std::to_string(static_cast<uint32_t>(terrain->DebugView));
            int debugIdx = 0;
            for (size_t oi = 0; oi < kDebugViewOptions.size(); ++oi)
                if (kDebugViewOptions[oi].value == debugValue)
                    debugIdx = static_cast<int>(oi);
            auto* debugDD = InspectorUI::AddDropdownRow(advanced, "Debug View", kDebugViewOptions, debugIdx,
                "Facets tints each CBT triangle by its slot so the live triangulation "
                "(density, splits) reads on the surface. Atlas slots tints each pixel by the "
                "atlas slot its splat/normal taps resolve to, grey outside the resident window; "
                "it shades normally on terrain that is not atlas-backed. Off = normal shading.");
            debugDD->SetOnValueChanged([w, e, n](const std::string& value) {
                auto* comp = w->GetComponent<Components::Terrain>(e);
                if (!comp) return;
                Components::Terrain updated = *comp;
                updated.DebugView = static_cast<Components::TerrainDebugView>(std::stoi(value));
                Editor::CommitComponentUpdate(w, e, n, updated);
            });
        }

        // ---- Rendering ----
        UIElement* rendering = AddSection(ctx.Parent, "Terrain", "Terrain Rendering");

        AddToggleRow(rendering, "Cast Shadows", terrain->CastShadows,
            [w, e, n](bool v) {
                auto* comp = w->GetComponent<Components::Terrain>(e);
                if (!comp) return;
                Components::Terrain updated = *comp;
                updated.CastShadows = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
            });

        AddToggleRow(rendering, "Receive Shadows", terrain->ReceiveShadows,
            [w, e, n](bool v) {
                auto* comp = w->GetComponent<Components::Terrain>(e);
                if (!comp) return;
                Components::Terrain updated = *comp;
                updated.ReceiveShadows = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
            });

        // Physics collision is controlled by adding a HeightFieldColliderShape
        // component to the entity, not by a flag on the Terrain component.
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::Terrain>(std::move(fn));

    // Planet Relief: the planet's base procedural noise, split out of Terrain so its
    // dials read as authoring (not a heightmap control). Only spherical terrains carry
    // the component, so this section appears exactly when relief applies.
    InspectorFn reliefFn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* relief = ctx.World->GetComponent<Components::TerrainPlanetRelief>(ctx.Entity);
        if (!relief)
        {
            InspectorUI::AddLine(ctx.Parent, "(TerrainPlanetRelief missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        using namespace InspectorDrag;

        AddComponentFloatRowWithDrag<Components::TerrainPlanetRelief>(
            ctx.Parent, "Amplitude (m)", relief->Amplitude, w, e, n,
            [](Components::TerrainPlanetRelief& u, float v) { u.Amplitude = std::max(0.0f, v); },
            "How tall the planet's base hills and valleys are, in metres. Around 1/40 of the "
            "radius reads well from orbit.",
            {}, 0.0f, 5000.0f);

        AddComponentFloatRowWithDrag<Components::TerrainPlanetRelief>(
            ctx.Parent, "Frequency", relief->Frequency, w, e, n,
            [](Components::TerrainPlanetRelief& u, float v) { u.Frequency = std::max(0.0f, v); },
            "How often features repeat around the globe — higher = smaller, more frequent hills "
            "(wavelength ~= 2*pi*R / frequency).",
            {}, 0.0f, 200.0f);

        AddComponentFloatRowWithDrag<Components::TerrainPlanetRelief>(
            ctx.Parent, "Detail Octaves", static_cast<float>(relief->Octaves), w, e, n,
            [](Components::TerrainPlanetRelief& u, float v) {
                u.Octaves = static_cast<uint32>(std::clamp(static_cast<int>(std::lround(v)), 1, 8));
            },
            "Layers of finer noise stacked on the base shape for surface detail (1-8).",
            {}, 1.0f, 8.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::TerrainPlanetRelief>(
        std::move(reliefFn));

    InspectorFn grassFn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* grass = ctx.World->GetComponent<Components::TerrainGrass>(ctx.Entity);
        if (!grass)
        {
            InspectorUI::AddLine(ctx.Parent, "(TerrainGrass missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;

        using namespace InspectorDrag;

        // Every alpha-dependent control below reads this one predicate — the same one extraction
        // uses to set the draw gate — so a control cannot claim to be inactive while the draw
        // still honours it. With no renderer to ask it answers "needs alpha": annotating a control
        // as inactive on a guess is worse than leaving it unannotated.
        const bool needsAlpha = TerrainECS::TerrainGrassNeedsAlpha(
            EngineCore::GetInstance().IsInitialized()
                ? EngineCore::GetInstance().GetRenderServices()
                : nullptr,
            *grass, /*answerWithoutServices=*/true);

        // needsAlpha is a SNAPSHOT, and the answer moves without any edit this inspector can see:
        // editing a bound texture on disk bumps its load epoch, the probe re-answers, and the draw
        // flips on the next frame. Rebuilding only on inspector-driven commits would leave the note
        // and the Alpha Cutoff row describing the previous answer — visible-but-inert one way,
        // hidden-but-live the other. That is the drift this predicate exists to prevent, arriving
        // through the disk instead of through a control.
        //
        // Re-checked per frame rather than by subscribing to texture reloads: the probe is cached
        // per texture per epoch, so a steady-state check is a hash lookup, and this also catches
        // every other way the answer can move (asset re-registration, services arriving late) that
        // a reload subscription for today's bound GUIDs would miss.
        //
        // The refresh request is deferred by the inspector, so asking for it from inside
        // TickSimulationRefresh's loop over these callbacks is safe.
        if (ctx.FrameRefreshCallbacks && ctx.RequestInspectorRefresh)
        {
            auto refreshRequested = std::make_shared<bool>(false);
            ctx.FrameRefreshCallbacks->push_back(
                [w, e, needsAlpha, refreshRequested, host = ctx.Parent,
                 requestRefresh = ctx.RequestInspectorRefresh]()
                {
                    if (*refreshRequested || !host || !w || !e.IsValid() || !w->IsValid(e))
                        return;
                    const auto* live = w->GetComponent<Components::TerrainGrass>(e);
                    if (!live)
                        return;
                    const bool nowNeedsAlpha = TerrainECS::TerrainGrassNeedsAlpha(
                        EngineCore::GetInstance().IsInitialized()
                            ? EngineCore::GetInstance().GetRenderServices()
                            : nullptr,
                        *live, /*answerWithoutServices=*/true);
                    if (nowNeedsAlpha == needsAlpha)
                        return;
                    *refreshRequested = true;
                    requestRefresh();
                });
        }

        // GRASS: the dials with distinct, visible, non-overlapping effects — the ones real tuning
        // turns. Everything below the fold is either expert or a stylisation escape hatch, and
        // nothing up here can make the field objectively wrong.
        UIElement* grassSection = AddSection(ctx.Parent, "TerrainGrass", "Grass");

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "blades / m² (at camera)", grass->BladesPerSquareMeter, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.BladesPerSquareMeter = std::max(0.0f, v); },
            "Blade count per square metre at the camera, independent of terrain size", {}, 0.0f, 64.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Range (m)", grass->Range, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.Range = std::max(0.0f, v); },
            Editor::TerrainGrassVocabulary::RangeTooltip().c_str(), {}, 0.0f, 2000.0f);

        // The authored range is a ceiling. Run this terrain's own authored numbers through the SAME
        // fit the renderer uses and say so when it comes back shorter, because nothing else in the
        // editor does: the row would otherwise read 500 while the field stops at 369.
        //
        // The density fed in is the MODE's, matching TerrainRenderFeature's summary and the
        // placement compute. Reading BladesPerSquareMeter unconditionally made the card case say
        // the opposite of what shipped: a card terrain at 0.35 cards/m2 delivers its full 500 m,
        // and the note claimed 369.
        {
            TerrainGrass::GrassPlacementParams fitInput;
            fitInput.NearDensity = grass->TextureGrass ? grass->TextureCardsPerSquareMeter
                                                       : grass->BladesPerSquareMeter;
            fitInput.FarRadius = grass->Range;
            fitInput.Falloff = grass->DensityFalloff;
            const auto plan = TerrainGrass::GrassFitPlacementToBudget(
                fitInput, TerrainGrass::TerrainGrassRenderFeature::kMaxInstancesPerView);
            if (const std::string note = Editor::TerrainGrassVocabulary::RangeFitNote(
                    grass->Range, plan.Params.FarRadius, grass->TextureGrass);
                !note.empty())
                InspectorUI::AddInfoCard(grassSection, note);
        }

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Blade Height", grass->BladeHeight, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.BladeHeight = std::max(0.0f, v); },
            "Average blade height in world units", {}, 0.0f, 20.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Blade Width", grass->BladeWidth, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.BladeWidth = std::max(0.0f, v); },
            "Average blade width in world units", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Clump Size", grass->ClumpSize, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.ClumpSize = std::max(0.0f, v); },
            "Edge of a tuft in metres, on a coarse lattice whose blades share a height, a facing "
            "and a colour. 0 or less turns clumping off and restores the plain field", {}, 0.0f, 8.0f);

        AddGrassColorRow(grassSection, "Tip Color",
            "The canopy colour, and so THE colour of the field: a blade is one gradient from the "
            "ground it stands in at the root to this at the tip",
            1, "Tip", w, e, n, ctx.OpenColorPickerWindow);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Hue Variation", grass->HueVariation, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.HueVariation = std::clamp(v, 0.0f, 1.0f); },
            "How many different greens the field is: hue spread between whole tufts, where 1 is a "
            "30-degree swing either way, with blades inside a tuft breaking up against each other "
            "at a fixed share of the same amount. Reach for it when a field reads as one colour "
            "lit unevenly rather than as many plants. It costs almost no brightness, so its only "
            "ceiling is how much spread still reads as ONE plant: far enough up, neighbouring "
            "blades read as two species mixed rather than one sward", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Root Shade", grass->RootShade, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.RootShade = std::clamp(v, 0.0f, 1.0f); },
            "How far the shade ramp dips low on the blade, and it cannot move the contact itself. "
            "At the shipped Root Fade span it changes NOTHING: the ramp acts over the lowest 22% "
            "of the blade, which is where the blade's colour schedule weights the canopy to a few "
            "percent, so every value of this dial renders the same field. It shapes a blade only "
            "once Root Fade End is pulled down far enough to give that range some weight — and a "
            "Root Fade Start at or above 0.22 strands it the other way, by discarding the canopy "
            "colour below the fade start", {}, 0.0f, 1.0f);

        // One two-thumb slider for the fade: the low thumb is Root Fade Start, the high thumb is
        // Root Fade End, and each end's value sits on its side of the track. Both thumbs report
        // through the same value callbacks, so one handler reads both ends off the slider and
        // writes the pair.
        {
            UIElement* row = InspectorUI::AddRow(grassSection);
            InspectorUI::AddLabel(row, "Root Fade",
                     "Material look, not lighting: where a blade's COLOUR leaves the ground it grows "
                     "out of. The lighting handover always runs the whole blade. Below the low "
                     "thumb the blade is the ground colour; above the high thumb it is entirely the "
                     "tip colour. A low thumb below 0 starts the blend under the root, so the root "
                     "already carries some tip colour. A short span steepens the colour ramp, up "
                     "to 1/span, rather than adding a band");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->AddClass("inspector-slider-with-value");

            auto makeValueLabel = [fieldContainer]() {
                auto owned = std::make_unique<Label>();
                Label* label = owned.get();
                label->AddClass("inspector-range-value");
                fieldContainer->AddChild(std::move(owned));
                return label;
            };
            auto formatFade = [](float v) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.2f", v);
                return std::string(buf);
            };

            Label* startLabel = makeValueLabel();
            auto sliderOwned = std::make_unique<Slider>();
            Slider* fade = sliderOwned.get();
            fade->AddClass("property-slider");
            fade->SetMin(Components::kMinTerrainGrassRootFadeStart);
            fade->SetMax(1.0f);
            fade->SetShowValueBubble(false);
            fieldContainer->AddChild(std::move(sliderOwned));
            Label* endLabel = makeValueLabel();

            auto applyFade = [w, e, n, fade, startLabel, endLabel, formatFade](bool commit)
            {
                auto* comp = w->GetComponent<Components::TerrainGrass>(e);
                if (!comp)
                    return;
                Components::TerrainGrass updated = *comp;
                updated.RootFadeStart =
                    std::clamp(fade->GetRangeStart(), Components::kMinTerrainGrassRootFadeStart,
                               1.0f - Components::kMinTerrainGrassRootFadeSpan);
                updated.RootFadeEnd = std::clamp(
                    fade->GetValue(), updated.RootFadeStart + Components::kMinTerrainGrassRootFadeSpan,
                    1.0f);
                startLabel->SetText(formatFade(updated.RootFadeStart));
                endLabel->SetText(formatFade(updated.RootFadeEnd));
                if (commit)
                    Editor::CommitComponentUpdate(w, e, n, updated);
                else
                    Editor::PreviewComponentUpdate(w, e, n, updated);
            };
            fade->SetRangeMode(true);
            fade->SetRangeValues(grass->RootFadeStart, grass->RootFadeEnd);
            startLabel->SetText(formatFade(grass->RootFadeStart));
            endLabel->SetText(formatFade(grass->RootFadeEnd));
            // Install the change callbacks only after the range is seeded, so the seeding
            // notification cannot commit an undo entry every time the panel rebuilds.
            fade->SetOnValueChanging([applyFade](const float&) { applyFade(false); });
            fade->SetOnValueChanged([applyFade](const float&) { applyFade(true); });
        }

        // Authored in RADIANS on the component; degrees in the panel, because a heading is a
        // compass reading and nobody drags one in radians.
        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Wind Direction (deg)", grass->WindDirection * kGrassRadToDeg, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindDirection = v * kGrassDegToRad; },
            "Wind heading in degrees", {}, -360.0f, 360.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Wind Strength", grass->WindStrength, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindStrength = std::max(0.0f, v); },
            "How hard the wind pushes: the gust lean amplitude, and the gate on the flutter and "
            "the gust shading with it. 0 is a bit-still meadow", {}, 0.0f, 5.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            grassSection, "Gust Speed", grass->WindGustSpeed, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindGustSpeed = std::max(0.0f, v); },
            "Speed of the broad wind noise flow", {}, 0.0f, 8.0f);

        // Strength and colour in ONE row: the tint is meaningless without the term it tints, and
        // two rows for one effect is exactly the surface this pass exists to shrink.
        if (FloatField* backlight = AddComponentFloatRowWithDrag<Components::TerrainGrass>(
                grassSection, "Backlight", grass->Translucency, w, e, n,
                [](Components::TerrainGrass& u, float v) { u.Translucency = std::max(0.0f, v); },
                "Sunlight through the blade, on the face turned away from the sun, and the colour it carries",
                {}, 0.0f, 2.0f))
        {
            // The class is what makes it one row: a field container lays its float field out
            // full-width, so a swatch appended after it wraps onto a second line and the "one row"
            // silently becomes two. The stylesheet answers this class (inspector.css).
            if (UIElement* field = backlight->GetParent())
            {
                field->AddClass("inspector-grass-value-and-swatch");
                AddGrassColorSwatch(field, 2, "Color", w, e, n, ctx.OpenColorPickerWindow);
            }
        }

        // ADVANCED: legitimate controls that are not look dials — quality/perf budgets, placement
        // plumbing, seeds, the measured shading pair, and the stylisation escape hatches.
        UIElement* advanced = AddSection(ctx.Parent, "TerrainGrass", "Advanced", false);

        {
            static const std::vector<Dropdown::Option> kGrassRenderModeOptions = {
                {"0", "Dither"},
                {"1", "Blend"},
            };
            const int selectedIdx =
                (grass->RenderMode == Components::TerrainGrassRenderMode::Blend) ? 1 : 0;
            const std::string modeTooltip =
                Editor::TerrainGrassVocabulary::RenderModeTooltip(needsAlpha);
            auto* modeDD = InspectorUI::AddDropdownRow(advanced, "Render Mode", kGrassRenderModeOptions,
                selectedIdx, modeTooltip.c_str());
            modeDD->SetOnValueChanged([w, e, n](const std::string& value) {
                auto* comp = w->GetComponent<Components::TerrainGrass>(e);
                if (!comp)
                    return;
                Components::TerrainGrass updated = *comp;
                updated.RenderMode = std::stoi(value) == 1
                    ? Components::TerrainGrassRenderMode::Blend
                    : Components::TerrainGrassRenderMode::Dither;
                Editor::CommitComponentUpdate(w, e, n, updated);
            });
        }

        // The draw mode outranks the authored one when no active terrain carries alpha, so the
        // dropdown above can be selected and still not describe what draws.
        if (const std::string inactive =
                Editor::TerrainGrassVocabulary::RenderModeInactiveNote(needsAlpha);
            !inactive.empty())
            InspectorUI::AddInfoCard(advanced, inactive);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Density Falloff", grass->DensityFalloff, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.DensityFalloff = std::max(0.0f, v); },
            "Falloff exponent: 1 is linear to Range, higher spends more of the budget near the camera",
            {}, 0.0f, 6.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Placement Seed", grass->PlacementSeed, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.PlacementSeed = v; },
            "Hashed per cell and slot, so the same seed reproduces the same blades");

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Clump Height Variance", grass->ClumpHeightVariance, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.ClumpHeightVariance = std::clamp(v, 0.0f, 1.0f); },
            "How far a clump's height departs from Blade Height, plus or minus this fraction. "
            "0 makes every clump the same height as every other", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Clump Alignment", grass->ClumpAlignment, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.ClumpAlignment = std::clamp(v, 0.0f, 1.0f); },
            "How far each blade's yaw is pulled toward its clump's common facing: 0 leaves every "
            "blade independent, 1 makes a whole tuft face one way", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Clump Gather", grass->ClumpGather, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.ClumpGather = std::clamp(v, 0.0f, 1.0f); },
            "How far each blade is pulled toward its clump's centre. Tightens tufts without "
            "thinning the field — blades per square metre is unchanged, only where they stand",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Max Width Ratio", grass->MaxWidthRatio, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.MaxWidthRatio = std::max(0.0f, v); },
            "Aspect ceiling: width never exceeds Blade Height times this, so short blades cannot "
            "become squares. Raise it if Blade Width has stopped having an effect", {}, 0.0f, 0.5f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Blade Segments", static_cast<float>(grass->BladeSegments), w, e, n,
            [](Components::TerrainGrass& u, float v) {
                const int rounded = static_cast<int>(std::lround(v));
                u.BladeSegments = static_cast<uint32>(std::clamp(
                    rounded,
                    static_cast<int>(Components::kMinTerrainGrassBladeSegments),
                    static_cast<int>(Components::kMaxTerrainGrassBladeSegments)));
            },
            "Height subdivisions per grass blade", {},
            static_cast<float>(Components::kMinTerrainGrassBladeSegments),
            static_cast<float>(Components::kMaxTerrainGrassBladeSegments));

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Random Scale", grass->RandomScale, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.RandomScale = std::max(0.0f, v); },
            "Per-blade random scale amount", {}, 0.0f, 2.0f);

        {
            static const std::vector<Dropdown::Option> kGrassLayerOptions = []
            {
                std::vector<Dropdown::Option> options;
                options.reserve(TerrainLayers::kCount);
                for (uint32_t i = 0; i < TerrainLayers::kCount; ++i)
                {
                    const std::string index = std::to_string(i);
                    options.push_back({index, "Layer " + index + " (" + TerrainLayers::kNames[i] + ")", {}});
                }
                return options;
            }();
            int selectedIdx = std::clamp(static_cast<int>(grass->LayerIndex), 0,
                                         static_cast<int>(TerrainLayers::kCount) - 1);
            auto* grassLayerDD = InspectorUI::AddDropdownRow(advanced, "Layer", kGrassLayerOptions, selectedIdx,
                "Terrain material layer used to spawn procedural grass");
            grassLayerDD->SetOnValueChanged([w, e, n](const std::string& value) {
                auto* comp = w->GetComponent<Components::TerrainGrass>(e);
                if (!comp) return;
                Components::TerrainGrass updated = *comp;
                updated.LayerIndex = static_cast<uint32>(
                    std::clamp(std::stoi(value), 0, static_cast<int>(TerrainLayers::kCount) - 1));
                Editor::CommitComponentUpdate(w, e, n, updated);
            });
        }

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Mask Threshold", grass->MaskThreshold, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.MaskThreshold = std::clamp(v, 0.0f, 1.0f); },
            "Minimum grass texture mask weight", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Gust Scale", grass->WindGustScale, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindGustScale = std::max(0.0001f, v); },
            "World-space scale for wind gust noise", {}, 0.001f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Resting Lean", grass->WindRestingLean, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindRestingLean = v; },
            "Baseline lean before gusts are applied", {}, -1.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Flutter Amount", grass->WindFlutterAmount, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindFlutterAmount = std::max(0.0f, v); },
            "Small high-frequency blade flutter amount", {}, 0.0f, 2.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Flutter Speed", grass->WindFlutterSpeed, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindFlutterSpeed = std::max(0.0f, v); },
            "Small high-frequency blade flutter speed", {}, 0.0f, 12.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Wind Seed", grass->WindSeed, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.WindSeed = v; },
            "Offset for procedural wind noise", {}, -1000.0f, 1000.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Brightness", grass->Brightness, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.Brightness = std::max(0.0f, v); },
            "Flat trim on the canopy albedo. The front door for a field that reads too dark is Tip "
            "Color; this is for matching a ground palette, and above 1 it pushes albedo out of the "
            "physical range the contact is calibrated in", {}, 0.0f, 3.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Random Brightness", grass->RandomBrightness, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.RandomBrightness = std::max(0.0f, v); },
            "Per-blade random brightness amount", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Blade Normal Form", grass->BladeNormalForm, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.BladeNormalForm = std::clamp(v, 0.0f, 1.0f); },
            "How much of a blade's own near-horizontal normal is used to shade it across its width. "
            "1 gives the strongest lit and shaded sides and costs about a fifth of the field's "
            "brightness, because a horizontal normal catches far less of a high sun; 0 shades every "
            "blade on the up-dominant canopy normal, so the field is as bright as flat ground and "
            "has no per-blade form",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Blade Scatter Gain", grass->BladeScatterGain, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.BladeScatterGain = std::max(0.0f, v); },
            "Hands back the brightness the form above gives up, standing in for the light that "
            "reaches a blade off its neighbours. Exactly zero at the blade base and in the far "
            "field, where nothing has been given up. It lifts the darkest parts of a blade hardest, "
            "so it trades the field's mean brightness against its blade-scale contrast. Lower it if "
            "a distant vista reads too bright; 0 disables it. Raising it past the default to cure a "
            "dark-looking field brings back a bright band across the mid distance",
            {}, 0.0f, 2.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            advanced, "Grounding Strength", grass->GroundingStrength, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.GroundingStrength = std::clamp(v, 0.0f, 1.0f); },
            "Master gate over how far the blade reads as growing out of its ground: the gust "
            "shading, the root-shade ramp and the base's blend into the terrain colour. Nothing "
            "to do with cast shadows. At 0 a blade never adopts its ground's colour at all, so "
            "this is a stylisation escape hatch rather than a look dial",
            {}, 0.0f, 1.0f);

        AddToggleRow(advanced, "Splat Root Color", grass->UseSplatRootColor,
            [w, e, n, requestRefresh = ctx.RequestInspectorRefresh](bool v)
            {
                Components::TerrainGrass updated = *w->GetComponent<Components::TerrainGrass>(e);
                updated.UseSplatRootColor = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                if (requestRefresh)
                    requestRefresh();
            },
            "Use the terrain material color under the blade for the grass root color");

        // Only reachable while the toggle above is off: with it on the root takes the ground's own
        // resolved albedo and this field is not read at all.
        if (!grass->UseSplatRootColor)
        {
            AddGrassColorRow(advanced, "Root Color",
                "The flat colour a blade's base takes when it is NOT reading the ground under it",
                0, "Root", w, e, n, ctx.OpenColorPickerWindow);
        }

        UIElement* texture = AddSection(ctx.Parent, "TerrainGrass", "Texture");

        AddToggleRow(texture, "Texture Grass", grass->TextureGrass,
            [w, e, n, requestRefresh = ctx.RequestInspectorRefresh](bool v)
            {
                Components::TerrainGrass updated = *w->GetComponent<Components::TerrainGrass>(e);
                updated.TextureGrass = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                // Geometric blades sample no texture, so this toggle decides whether the alpha
                // controls below mean anything at all.
                if (requestRefresh)
                    requestRefresh();
            },
            "Use the texture card mesh path instead of raw procedural blades");

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "cards / m² (at camera)", grass->TextureCardsPerSquareMeter, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.TextureCardsPerSquareMeter = std::max(0.0f, v); },
            "Texture card count per square metre at the camera", {}, 0.0f, 16.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "Texture Size", grass->TextureSize, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.TextureSize = std::max(0.0f, v); },
            "Texture card square size in world units", {}, 0.0f, 10.0f);

        AddGrassTextureField(texture, "Albedo", grass, w, e, n, ctx.Thumbnails,
            grass->AlbedoTextureAssetGuid.ToGuid(),
            [](Components::TerrainGrass& u, const GUID& guid)
            {
                u.AlbedoTextureAssetGuid.Set(guid);
            },
            "Optional color or color atlas texture multiplied with grass colors. Binding one "
            "whose alpha is not provably opaque is what makes the Alpha Cutoff row appear.",
            ctx.RequestInspectorRefresh);

        AddGrassTextureField(texture, "Alpha", grass, w, e, n, ctx.Thumbnails,
            grass->AlphaTextureAssetGuid.ToGuid(),
            [](Components::TerrainGrass& u, const GUID& guid)
            {
                u.AlphaTextureAssetGuid.Set(guid);
            },
            "Optional separate alpha atlas used to cut out grass clumps. Binding one always "
            "makes the Alpha Cutoff row appear: it is sampled through red, so its content "
            "cannot be probed the way an albedo's alpha channel can.",
            ctx.RequestInspectorRefresh);

        AddGrassTextureField(texture, "Normal", grass, w, e, n, ctx.Thumbnails,
            grass->NormalTextureAssetGuid.ToGuid(),
            [](Components::TerrainGrass& u, const GUID& guid)
            {
                u.NormalTextureAssetGuid.Set(guid);
            },
            "Optional grass normal map or normal atlas",
            // Gates no row: a normal map has no bearing on whether alpha is live.
            {});

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "Atlas Columns", static_cast<float>(grass->AtlasColumns), w, e, n,
            [](Components::TerrainGrass& u, float v) {
                u.AtlasColumns = static_cast<uint32>(std::clamp(static_cast<int>(std::lround(v)), 1, 16));
                u.AtlasTileCount = std::clamp(u.AtlasTileCount, 1u, std::max(1u, u.AtlasColumns * u.AtlasRows));
            },
            "Number of atlas columns", {}, 1.0f, 16.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "Atlas Rows", static_cast<float>(grass->AtlasRows), w, e, n,
            [](Components::TerrainGrass& u, float v) {
                u.AtlasRows = static_cast<uint32>(std::clamp(static_cast<int>(std::lround(v)), 1, 16));
                u.AtlasTileCount = std::clamp(u.AtlasTileCount, 1u, std::max(1u, u.AtlasColumns * u.AtlasRows));
            },
            "Number of atlas rows", {}, 1.0f, 16.0f);

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "Atlas Tiles", static_cast<float>(grass->AtlasTileCount), w, e, n,
            [](Components::TerrainGrass& u, float v) {
                const uint32 maxTiles = std::max(1u, u.AtlasColumns * u.AtlasRows);
                u.AtlasTileCount = static_cast<uint32>(std::clamp(static_cast<int>(std::lround(v)), 1, static_cast<int>(maxTiles)));
            },
            "Number of atlas cells to randomly choose from", {}, 1.0f, 256.0f);

        // Alpha Cutoff resolves nothing unless a bound blade texture actually carries soft alpha,
        // so it appears exactly when it can change a pixel. The same predicate drives the draw
        // mode, which is what keeps the control's presence and its effect in step: a texture whose
        // alpha probes uniformly opaque leaves the whole alpha path inert, and the row with it.
        if (needsAlpha)
        {
            AddComponentFloatRowWithDrag<Components::TerrainGrass>(
                texture, "Alpha Cutoff", grass->AlphaCutoff, w, e, n,
                [](Components::TerrainGrass& u, float v) { u.AlphaCutoff = std::clamp(v, 0.0f, 1.0f); },
                "Cutout threshold for albedo alpha or separate alpha texture. Shown only while a "
                "bound texture has alpha that could not be proven uniformly opaque — a bound "
                "alpha map always counts, and an albedo is judged by content only for some KTX2 "
                "(RGBA8 and UASTC; BC and ETC1S are refused rather than guessed at); "
                "PNG and TGA are judged on whether they carry an alpha channel at all.",
                {}, 0.0f, 1.0f);
        }

        AddComponentFloatRowWithDrag<Components::TerrainGrass>(
            texture, "Normal Strength", grass->NormalStrength, w, e, n,
            [](Components::TerrainGrass& u, float v) { u.NormalStrength = std::max(0.0f, v); },
            "Blend strength for the optional grass normal map", {}, 0.0f, 2.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::TerrainGrass>(std::move(grassFn));

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip = "Switch this component on or off. Off places no grass on this terrain, so it "
                                 "costs no placement and no draw.";
    Editor::EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::TerrainGrass>(), std::move(traits));
}

} // namespace GameEngine
