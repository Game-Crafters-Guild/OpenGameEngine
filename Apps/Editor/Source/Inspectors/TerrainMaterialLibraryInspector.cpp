#include "Inspectors/TerrainMaterialLibraryInspector.h"

#include "EditorContextMenu/UIContextMenu.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorTextureSlotPreview.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Terrain/TerrainRoleMaterials.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Core/Engine.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace GameEngine
{
namespace
{

// The picker and every CSS colour are sRGB bytes; an entry's tint is linear, so it has to be
// encoded on the way out and decoded on the way in or the swatch shows a different colour from
// the terrain.
uint32_t EncodeSrgbByte(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint32_t>(s * 255.0f + 0.5f);
}

float DecodeSrgbByte(uint32_t byteValue)
{
    const float s = std::clamp(static_cast<float>(byteValue) / 255.0f, 0.0f, 1.0f);
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

// What the card says beside its name: the slot ID the painted terrain stores, kept off the
// headline because the name is what the author reads a list of materials by, and — for a
// tombstone — a badge, because a toggle buried in the rows is not a state a card can be read for.
void AddCardHeaderNotes(Foldout& card, const TerrainMaterialEntry& entry,
                        const std::string& idPrefix)
{
    UIElement* header = card.GetHeader();
    if (!header)
        return;

    auto slot = std::make_unique<Label>();
    slot->SetId(idPrefix + "-slotnote");
    slot->AddClass("terrain-material-slot-note");
    slot->SetText("Slot " + std::to_string(entry.SlotId));
    header->AddChild(std::move(slot));

    if (!entry.Retired)
        return;

    auto badge = std::make_unique<Label>();
    badge->SetId(idPrefix + "-retiredbadge");
    badge->AddClass("terrain-material-retired-badge");
    badge->SetText("RETIRED");
    header->AddChild(std::move(badge));
}

std::string FormatTint(const TerrainMaterialEntry& entry)
{
    return InspectorUI::FormatColorRgb(entry.AlbedoR, entry.AlbedoG, entry.AlbedoB);
}

// The undoable whole-document library edit is shared with the terrain's role-binding UI, which
// creates materials through the same path — see Terrain/TerrainRoleMaterials.h.
using Editor::TerrainRoleMaterials::EditLibraryUndoable;

struct CardContext
{
    TerrainMaterialLibraryAsset* Library = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    std::function<void()> RequestRefresh;
    IThumbnailProvider* Thumbnails = nullptr;
    OpenColorPickerWindowFn OpenPicker;
    Platform::Window* Window = nullptr;
};

// One float row that writes a single entry field. The entry is addressed by SLOT ID, not by row
// index: a remove elsewhere in the list shifts positions, and a stale index would edit a
// different material than the card the user is looking at.
void AddEntryFloatRow(UIElement* parent, const CardContext& card, uint8_t slotId,
                      const char* labelText, float initial, float defaultValue, float minValue,
                      float maxValue, const char* tooltip, const std::string& id,
                      float TerrainMaterialEntry::*field, const char* undoLabel)
{
    auto commit = [card, slotId, field, undoLabel](float v)
    {
        EditLibraryUndoable(card.Library, card.Undo, undoLabel, card.RequestRefresh,
                            [slotId, field, v](std::vector<TerrainMaterialEntry>& entries)
                            {
                                if (TerrainMaterialEntry* e =
                                        FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                    e->*field = v;
                            });
    };
    FloatField* f = InspectorDrag::AddFloatRowWithDrag(
        parent, labelText, initial, [](float) {}, commit, defaultValue, tooltip, minValue,
        maxValue);
    if (f)
        f->SetId(id);
}

void AddEntryTextureRow(UIElement* parent, const CardContext& card, uint8_t slotId,
                        const char* labelText, const GUID& initial, const char* tooltip,
                        const std::string& id, GUID TerrainMaterialEntry::*field,
                        const char* undoLabel)
{
    const auto commit = [card, slotId, field, undoLabel](const GUID& guid)
    {
        EditLibraryUndoable(card.Library, card.Undo, undoLabel, card.RequestRefresh,
                            [slotId, field, guid](std::vector<TerrainMaterialEntry>& entries)
                            {
                                if (TerrainMaterialEntry* e =
                                        FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                    e->*field = guid;
                            });
    };

    AssetField* f = InspectorUI::AddAssetFieldRow(
        parent, labelText, initial, {AssetType::Texture},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(), commit, card.Thumbnails,
        tooltip);
    if (f)
        f->SetId(id);

    InspectorUI::AddTextureSlotPreview(parent, initial, commit);
}

void AddTintRow(UIElement* parent, const CardContext& card, const TerrainMaterialEntry& entry,
                const std::string& id)
{
    InspectorUI::ColorSwatchRow row = InspectorUI::AddColorSwatchRow(
        parent, "Tint",
        "Linear colour. Multiplies a bound albedo texture, and IS the colour without one.",
        TerrainMaterialTintToSwatchArgb(entry), FormatTint(entry));
    UIElement* swatchRaw = row.Swatch;
    Label* valueLabelRaw = row.Value;
    if (!swatchRaw || !valueLabelRaw)
        return;
    swatchRaw->SetId(id);

    const uint8_t slotId = entry.SlotId;
    auto onClick = [card, slotId, swatchRaw](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Stop();
        if (!card.OpenPicker || !card.Library)
            return;
        const TerrainMaterialEntry* current =
            FindTerrainMaterialBySlotId(card.Library->GetMaterials(), slotId);
        if (!current)
            return;

        ColorPickerCallbacks callbacks;
        // Live preview repaints the swatch only: extraction shades from a cached parse of the
        // file, so nothing reaches the terrain until the apply below writes and reloads it.
        callbacks.onValueChanging = [swatchRaw](uint32_t argb, float) {
            InspectorUI::StyleColorSwatch(swatchRaw, argb | 0xFF000000u);
        };
        callbacks.onApply = [card, slotId](uint32_t argb, float)
        {
            ApplyTerrainMaterialTintFromPicker(card.Library, card.Undo, card.RequestRefresh,
                                               slotId, argb);
        };
        callbacks.onCancel = [card] {
            if (card.RequestRefresh)
                card.RequestRefresh();
        };
        card.OpenPicker(TerrainMaterialTintToSwatchArgb(*current), 1.0f, std::move(callbacks));
    };
    swatchRaw->RegisterEventHandler(kEventMouseDown, onClick);
    valueLabelRaw->RegisterEventHandler(kEventMouseDown, onClick);
}

constexpr EnumEntry<TerrainMaterialProjection> kProjections[] = {
    {TerrainMaterialProjection::Triplanar, "Triplanar"},
    {TerrainMaterialProjection::Planar, "Planar"},
};

void CommitProjection(const CardContext& card, uint8_t slotId, TerrainMaterialProjection projection)
{
    EditLibraryUndoable(card.Library, card.Undo, "Change Terrain Material Projection",
                        card.RequestRefresh,
                        [slotId, projection](std::vector<TerrainMaterialEntry>& entries)
                        {
                            if (TerrainMaterialEntry* e =
                                    FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                e->Projection = projection;
                        });
}

void AddProjectionRow(UIElement* parent, const CardContext& card, const TerrainMaterialEntry& entry,
                      const std::string& id)
{
    EnumField<TerrainMaterialProjection>* field = InspectorUI::AddEnumRow(
        parent, "Projection", kProjections, entry.Projection,
        "How the material's textures are laid onto the ground. Triplanar: from three sides, blended "
        "by slope; for tiling textures such as rock, grass and sand. Planar: straight down at the "
        "terrain's own coordinates, so one image covers the terrain the way its heightmap does; "
        "for an orthophoto or a painted basemap, north row first. Cliffs show a Planar image "
        "stretched. Heightfield terrains only: a planet shades a Planar material as Triplanar.");
    if (!field)
        return;
    field->SetId(id);
    const uint8_t slotId = entry.SlotId;
    field->SetOnValueChanged([card, slotId](TerrainMaterialProjection v)
                             { CommitProjection(card, slotId, v); });
}

// Remove is two steps, because what it does to bound terrains is not visible from the card. The
// consequence stated here is the one the engine implements: a role whose slot holds no entry
// resolves to the built-in material for that role (TerrainExtractionSystem::AppendTerrainMaterials),
// and the emptied slot returns to the pool the lowest-free rule allocates from
// (NextFreeTerrainMaterialSlotId), which is exactly what retiring prevents.
//
// Returns the confirmation block, hidden until the card's header menu asks for it.
UIElement* AddRemoveConfirmBlock(UIElement* content, const CardContext& card,
                                 const TerrainMaterialEntry& entry,
                                 const std::string& displayName, const std::string& idPrefix)
{
    if (!content)
        return nullptr;

    const std::uint8_t slotId = entry.SlotId;
    const std::string slot = std::to_string(static_cast<int>(slotId));

    auto confirmBlock = std::make_unique<UIElement>();
    UIElement* confirm = confirmBlock.get();
    confirm->SetId(idPrefix + "-remove-confirm");
    confirm->Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    InspectorUI::AddTextBlock(
        confirm,
        "Remove \"" + displayName + "\"? Every terrain channel bound to slot " + slot +
            " falls back to its built-in material: the ground stays painted, it just stops "
            "shading with this one. Slot " + slot +
            " returns to the pool, so a material added later can take it and inherit that "
            "ground. Retire instead to keep both.",
        "inspector-text");

    UIElement* confirmRow = InspectorUI::AddRow(confirm);
    InspectorUI::AddLabel(confirmRow, "");
    UIElement* confirmField = InspectorUI::AddFieldContainer(confirmRow);
    confirmField->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    auto apply = std::make_unique<Button>();
    apply->SetId(idPrefix + "-remove-apply");
    apply->AddClass("inspector-button");
    apply->SetText("Remove");
    apply->RegisterEventHandler(kEventButtonClick, [card, slotId](UIEvent&)
        {
            EditLibraryUndoable(card.Library, card.Undo, "Remove Terrain Material",
                                card.RequestRefresh,
                                [slotId](std::vector<TerrainMaterialEntry>& entries)
                                {
                                    entries.erase(
                                        std::remove_if(entries.begin(), entries.end(),
                                                       [slotId](const TerrainMaterialEntry& e)
                                                       { return e.SlotId == slotId; }),
                                        entries.end());
                                });
        });
    confirmField->AddChild(std::move(apply));

    auto cancel = std::make_unique<Button>();
    cancel->SetId(idPrefix + "-remove-cancel");
    cancel->AddClass("inspector-button");
    cancel->SetText("Cancel");
    cancel->RegisterEventHandler(kEventButtonClick, [confirm](UIEvent&)
        { confirm->Overrides().Set(Style::Display, DisplayMode::None); });
    confirmField->AddChild(std::move(cancel));

    content->AddChild(std::move(confirmBlock));
    return confirm;
}

// The card's own actions live behind the header's options icon, where every other inspector
// section keeps them — a destructive button sitting in the value rows reads as one more field.
void AddCardHeaderMenu(Foldout& cardUi, const CardContext& card, UIElement* removeConfirm,
                       const std::string& idPrefix)
{
    UIElement* header = cardUi.GetHeader();
    if (!header || !removeConfirm)
        return;

    auto options = std::make_unique<Button>();
    options->SetId(idPrefix + "-options");
    options->AddClass("icon-button");
    options->AddClass("inspector-section-header-options");
    options->SetTooltip("Material options");

    Foldout* foldout = &cardUi;
    Platform::Window* window = card.Window;
    options->RegisterEventHandler(kEventButtonClick, [window, foldout, removeConfirm](UIEvent& event)
        {
            UIElement& button = *event.CurrentTarget;
            if (!window)
                return;

            static std::shared_ptr<INativeContextMenu> menu;
            if (!menu)
                menu = CreateContextMenu();
            if (!menu)
                return;

            constexpr uint32_t kCmdRemove = 1;
            menu->Clear();
            menu->AddItem(0, "Remove Material", kCmdRemove);
            menu->SetItemIcon(kCmdRemove, EditorIcons::kTrash);
            menu->SetCommandHandler([foldout, removeConfirm](uint32_t command)
                {
                    if (command != kCmdRemove)
                        return;
                    foldout->SetExpanded(true);
                    removeConfirm->Overrides().Set(Style::Display, DisplayMode::Flex);
                });

            const int x = static_cast<int>(button.GetLayoutX());
            const int y = static_cast<int>(button.GetLayoutY() + button.GetLayoutHeight());
            menu->Show(window, x, y);
        });
    header->AddChild(std::move(options));
}

void AddMaterialCard(UIElement* parent, const CardContext& card, const TerrainMaterialEntry& entry)
{
    if (!parent)
        return;

    const uint8_t slotId = entry.SlotId;
    const std::string idPrefix = "terrainmatlib-slot-" + std::to_string(slotId);

    const std::string displayName = entry.Name.empty() ? std::string("Material") : entry.Name;

    auto foldout = std::make_unique<Foldout>();
    foldout->SetId(idPrefix + "-card");
    foldout->AddClass("rp-foldout");
    foldout->AddClass("terrain-material-card");
    foldout->SetTitle(displayName);
    foldout->SetExpanded(true);
    if (entry.Retired)
        foldout->AddClass("retired");
    AddCardHeaderNotes(*foldout, entry, idPrefix);
    UIElement* content = foldout->GetContentContainer();

    {
        UIElement* row = InspectorUI::AddRow(content);
        InspectorUI::AddLabel(row, "Name",
                                "Display name only. Two materials may share one; the slot ID above "
                                "is what the painted terrain stores.");
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
        auto nameField = std::make_unique<TextField>();
        nameField->SetId(idPrefix + "-name");
        nameField->SetValue(entry.Name);
        nameField->SetOnValueChanged(
            [card, slotId](const std::string& v)
            {
                EditLibraryUndoable(card.Library, card.Undo, "Rename Terrain Material",
                                    card.RequestRefresh,
                                    [slotId, v](std::vector<TerrainMaterialEntry>& entries)
                                    {
                                        if (TerrainMaterialEntry* e =
                                                FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                            e->Name = v;
                                    });
            });
        fieldContainer->AddChild(std::move(nameField));
    }

    AddTintRow(content, card, entry, idPrefix + "-tint");

    AddEntryTextureRow(content, card, slotId, "Albedo", entry.AlbedoTexture,
                       "Base color, laid on by the Projection below. Empty leaves the material on "
                       "its tint.",
                       idPrefix + "-albedo", &TerrainMaterialEntry::AlbedoTexture,
                       "Change Terrain Material Albedo");
    AddEntryTextureRow(content, card, slotId, "Normal", entry.NormalTexture,
                       "Tangent-space normal map, laid on by the same Projection as the albedo.",
                       idPrefix + "-normal", &TerrainMaterialEntry::NormalTexture,
                       "Change Terrain Material Normal");
    AddEntryTextureRow(content, card, slotId, "ORM", entry.OrmTexture,
                       "Packed map: R = ambient occlusion, G = roughness, B = metallic.",
                       idPrefix + "-orm", &TerrainMaterialEntry::OrmTexture,
                       "Change Terrain Material ORM");

    AddProjectionRow(content, card, entry, idPrefix + "-projection");
    AddEntryFloatRow(content, card, slotId, "Tiling", entry.Tiling, 1.0f, 0.0f, 64.0f,
                     "Triplanar: repeat rate against the terrain's Base Tiling (2 = twice as many "
                     "repeats, so half the feature size; 1 = the terrain's own rate). Planar: "
                     "repeats across the whole terrain (1 = one image over the whole terrain).",
                     idPrefix + "-tiling", &TerrainMaterialEntry::Tiling,
                     "Change Terrain Material Tiling");
    AddEntryFloatRow(content, card, slotId, "Roughness", entry.Roughness, 1.0f, 0.0f, 1.0f,
                     "The material's roughness. Multiplies a bound ORM map's green channel, or is "
                     "the value on its own when none is bound.",
                     idPrefix + "-roughness", &TerrainMaterialEntry::Roughness,
                     "Change Terrain Material Roughness");
    AddEntryFloatRow(content, card, slotId, "AO", entry.Ao, 1.0f, 0.0f, 1.0f,
                     "Ambient occlusion, same texture-or-value rule as Roughness.",
                     idPrefix + "-ao", &TerrainMaterialEntry::Ao,
                     "Change Terrain Material AO");
    AddEntryFloatRow(content, card, slotId, "Normal Strength", entry.NormalStrength, 1.0f, 0.0f,
                     4.0f, "Scales the bound normal map. No effect without one.",
                     idPrefix + "-normalstrength", &TerrainMaterialEntry::NormalStrength,
                     "Change Terrain Material Normal Strength");

    AddEntryFloatRow(content, card, slotId, "Variation", entry.VariationStrength, 0.0f, 0.0f, 1.0f,
                     "Procedural brightness jitter that breaks up a flat tint. 0 = off.",
                     idPrefix + "-variation", &TerrainMaterialEntry::VariationStrength,
                     "Change Terrain Material Variation");
    AddEntryFloatRow(content, card, slotId, "Variation Hue", entry.VariationHue, 0.0f, 0.0f, 1.0f,
                     "Chroma jitter paired with Variation.", idPrefix + "-variationhue",
                     &TerrainMaterialEntry::VariationHue, "Change Terrain Material Variation Hue");
    AddEntryFloatRow(content, card, slotId, "Variation Scale", entry.VariationScale, 0.0f, 0.0f,
                     1.0f, "Noise frequency in 1/m. 0 turns variation off entirely.",
                     idPrefix + "-variationscale", &TerrainMaterialEntry::VariationScale,
                     "Change Terrain Material Variation Scale");

    Toggle* hex = InspectorDrag::AddToggleRow(
        content, "Hex Tiling", entry.HexTiling,
        [card, slotId](bool v)
        {
            EditLibraryUndoable(card.Library, card.Undo, "Toggle Terrain Material Hex Tiling",
                                card.RequestRefresh,
                                [slotId, v](std::vector<TerrainMaterialEntry>& entries)
                                {
                                    if (TerrainMaterialEntry* e =
                                            FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                        e->HexTiling = v;
                                });
        },
        "Breaks visible repetition by blending three hash-rotated copies of the texture on a "
        "hexagonal lattice. Costs about 3x this material's texture samples. Leave it off for a "
        "Planar image of the terrain such as an orthophoto: it shuffles the image.");
    if (hex)
        hex->SetId(idPrefix + "-hex");

    Toggle* ormMetallic = InspectorDrag::AddToggleRow(
        content, "ORM Has Metallic", entry.OrmHasMetallic,
        [card, slotId](bool v)
        {
            EditLibraryUndoable(card.Library, card.Undo,
                                "Toggle Terrain Material ORM Metallic", card.RequestRefresh,
                                [slotId, v](std::vector<TerrainMaterialEntry>& entries)
                                {
                                    if (TerrainMaterialEntry* e =
                                            FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                        e->OrmHasMetallic = v;
                                });
        },
        "Read the ORM map's blue channel as metallic. Leave it off unless the map really packs "
        "metal there: most terrain ORM maps leave blue as padding, and shading padding as metallic "
        "turns the surface into a black mirror.");
    if (ormMetallic)
        ormMetallic->SetId(idPrefix + "-ormhasmetallic");

    Toggle* retired = InspectorDrag::AddToggleRow(
        content, "Retired", entry.Retired,
        [card, slotId](bool v)
        {
            EditLibraryUndoable(card.Library, card.Undo, "Retire Terrain Material",
                                card.RequestRefresh,
                                [slotId, v](std::vector<TerrainMaterialEntry>& entries)
                                {
                                    if (TerrainMaterialEntry* e =
                                            FindTerrainMaterialBySlotIdMutable(entries, slotId))
                                        e->Retired = v;
                                });
        },
        "Hides the material from pickers while already-painted ground keeps shading with it. Its "
        "slot ID stays reserved, so a new material cannot inherit those texels.");
    if (retired)
        retired->SetId(idPrefix + "-retired");

    AddCardHeaderMenu(*foldout,
                      card,
                      AddRemoveConfirmBlock(content, card, entry, displayName, idPrefix),
                      idPrefix);

    parent->AddChild(std::move(foldout));
}

} // namespace

std::uint32_t TerrainMaterialTintToSwatchArgb(const TerrainMaterialEntry& entry)
{
    return (0xFFu << 24) | (EncodeSrgbByte(entry.AlbedoR) << 16) |
           (EncodeSrgbByte(entry.AlbedoG) << 8) | EncodeSrgbByte(entry.AlbedoB);
}

void ApplyTerrainMaterialTintFromPicker(TerrainMaterialLibraryAsset* library,
                                        Editor::UndoRedoService* undo,
                                        const std::function<void()>& requestRefresh,
                                        std::uint8_t slotId, std::uint32_t argb)
{
    EditLibraryUndoable(library, undo, "Change Terrain Material Tint", requestRefresh,
                        [slotId, argb](std::vector<TerrainMaterialEntry>& entries)
                        {
                            TerrainMaterialEntry* e =
                                FindTerrainMaterialBySlotIdMutable(entries, slotId);
                            if (!e)
                                return;
                            e->AlbedoR = DecodeSrgbByte((argb >> 16) & 0xFFu);
                            e->AlbedoG = DecodeSrgbByte((argb >> 8) & 0xFFu);
                            e->AlbedoB = DecodeSrgbByte(argb & 0xFFu);
                        });
}

void RegisterTerrainMaterialLibraryInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.Object)
            return;
        auto* base = static_cast<Asset*>(ctx.Object);
        if (base->GetType() != AssetType::TerrainMaterialLibrary)
            return;
        auto* lib = static_cast<TerrainMaterialLibraryAsset*>(base);

        CardContext card;
        card.Library = lib;
        card.Undo = ctx.Undo;
        card.RequestRefresh = ctx.RequestInspectorRefresh;
        card.Thumbnails = ctx.Thumbnails;
        card.OpenPicker = ctx.OpenColorPickerWindow;
        card.Window = ctx.Window;

        const std::vector<TerrainMaterialEntry>& materials = lib->GetMaterials();
        if (materials.empty() && ctx.ShowInfoCards)
        {
            InspectorUI::AddTextBlock(ctx.Parent,
                                      "No materials yet. Add one, then paint with it from the "
                                      "terrain brush.",
                                      "inspector-text");
        }

        for (const TerrainMaterialEntry& entry : materials)
            AddMaterialCard(ctx.Parent, card, entry);

        UIElement* row = InspectorUI::AddRow(ctx.Parent);
        InspectorUI::AddLabel(row, "");
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
        auto add = std::make_unique<Button>();
        add->SetId("terrainmatlib-add");
        add->AddClass("inspector-button");
        add->SetText("Add Material");

        const std::uint32_t nextSlot = lib->NextFreeSlotId();
        if (nextSlot == TerrainMaterialLibraryAsset::kInvalidSlotId)
        {
            add->SetEnabled(false);
            add->SetTooltip("All 256 slot IDs are taken. Remove a retired material to free one.");
        }
        else
        {
            add->SetTooltip("Appends a material taking the lowest free slot ID, so it can never "
                            "inherit a retired material's painted ground.");
            // `nextSlot` above only decides whether the button is live. Which slot the material
            // takes is resolved inside the edit, against the list being mutated — see
            // Terrain/TerrainRoleMaterials.h, the path the terrain panel's add tile shares.
            add->RegisterEventHandler(kEventButtonClick, [card](UIEvent&) {
                    (void)Editor::TerrainRoleMaterials::AddLibraryMaterial(card.Library, card.Undo,
                                                                           card.RequestRefresh);
                });
        }
        fieldContainer->AddChild(std::move(add));
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::TerrainMaterialLibrary,
                                                    std::move(fn));
}

} // namespace GameEngine
