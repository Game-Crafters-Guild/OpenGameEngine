#include "Terrain/TerrainRoleMaterials.h"

#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/TerrainMaterialLibraryInspector.h"
#include "Terrain/TerrainLayers.h"
#include "UndoRedo/UndoRedoService.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Core/Engine.h"
#include "Platform/SystemMetrics.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace GameEngine::Editor::TerrainRoleMaterials
{
namespace
{

constexpr const char* kRoleNameTooltip =
    "The splat channel this row binds. Channel names are fixed — they label the slope and "
    "altitude rules that place them; the material's name is the one you author.";

constexpr const char* kRolePickerTooltip =
    "Which of the library's materials this channel shades with. Creating one here binds it to "
    "this channel and selects the channel for painting.";

constexpr const char* kLayerRoleTooltip =
    "Which material each of the splat's four channels shades with. Where a channel appears is "
    "decided by slope, altitude and paint strokes; these bindings decide what it looks like.";

// Card metrics. The swatch is wide and short so a row of cards reads as a palette rather than as
// four tiles, and the name sits under it.
//
// A card may stretch up to half again its base width to close out a row: cards of one fixed width
// leave whatever the row cannot divide as dead space on the right, and past 1.5x a card stops
// looking like a card. Capped rather than free growth, because two materials on a panel-wide row
// would otherwise take half the inspector each.
constexpr float kCardWidthPx = 84.0f;
constexpr float kCardMaxWidthPx = 126.0f;
constexpr float kCardSwatchHeightPx = 52.0f;
constexpr float kCardPaddingPx = 6.0f;
constexpr float kCardRadiusPx = 6.0f;
constexpr float kSwatchRadiusPx = 4.0f;
constexpr float kGridGapPx = 8.0f;
// The panel's label size (.inspector-label), not the 12px floor: these names are the smallest text
// in the section and they carry a material's identity, so they read at the size its labels do.
constexpr float kCardNameFontPx = 13.0f;
// Two lines, reserved rather than grown into: every card keeps one height whatever its name is, so
// a row of them stays a grid. A third line is clipped at the bottom — the UI has no line-clamp or
// text-overflow, so an ellipsis cannot be asked for, and losing the tail of a long name beats
// losing its start.
constexpr float kCardNameLineHeightPx = 17.0f;
constexpr float kCardNameBlockHeightPx = 2.0f * kCardNameLineHeightPx;
constexpr float kAddTileGlyphFontPx = 22.0f;
constexpr float kSwatchChipFontPx = 12.0f;

constexpr std::uint32_t kCardBorderArgb = 0xFF3A3A3Au;
constexpr std::uint32_t kSwatchBorderArgb = 0xFF202020u;
constexpr std::uint32_t kAddTileGlyphArgb = 0xFF9A9A9Au;
// The chip sits on an arbitrary tint, so it carries its own contrast: a near-opaque dark plate
// under light text, legible over white and over black alike.
constexpr std::uint32_t kSwatchChipBackgroundArgb = 0xB8141414u;
constexpr std::uint32_t kSwatchChipTextArgb = 0xFFE8E8E8u;

// What a material is called when it is called nothing: the library inspector shows the same word
// on an unnamed card, so a nameless material reads the same in both surfaces.
std::string MaterialDisplayName(const TerrainMaterialEntry& entry)
{
    return entry.Name.empty() ? std::string("Material") : entry.Name;
}

void StyleCardSurface(UIElement& card)
{
    card.Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::FlexBasis, StyleLength::Px(kCardWidthPx))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::MinWidth, StyleLength::Px(kCardWidthPx))
        .Set(Style::MaxWidth, StyleLength::Px(kCardMaxWidthPx))
        .Set(Style::PaddingTop, StyleLength::Px(kCardPaddingPx))
        .Set(Style::PaddingRight, StyleLength::Px(kCardPaddingPx))
        .Set(Style::PaddingBottom, StyleLength::Px(kCardPaddingPx))
        .Set(Style::PaddingLeft, StyleLength::Px(kCardPaddingPx))
        .Set(Style::BorderRadius,
             CornerRadiiTLTRBRBL{kCardRadiusPx, kCardRadiusPx, kCardRadiusPx, kCardRadiusPx})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor,
             BorderColorsTRBL{kCardBorderArgb, kCardBorderArgb, kCardBorderArgb, kCardBorderArgb})
        // Whatever a name spills past its two reserved lines is caught here rather than painted
        // over the card beside it.
        .Set(Style::OverflowProp, Overflow::Hidden)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

Label* AddCardName(UIElement& card, const std::string& text, const std::string& id)
{
    auto name = std::make_unique<Label>();
    Label* raw = name.get();
    raw->SetId(id);
    raw->AddClass("inspector-text");
    raw->SetText(text);
    raw->Overrides()
        .Set(Style::FontSize, StyleLength::Px(kCardNameFontPx))
        .Set(Style::LineHeight, kCardNameLineHeightPx)
        .Set(Style::MarginTop, StyleLength::Px(5.0f))
        .Set(Style::Height, StyleLength::Px(kCardNameBlockHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kCardNameBlockHeightPx))
        .Set(Style::MaxHeight, StyleLength::Px(kCardNameBlockHeightPx))
        // Wraps onto a second line rather than being cut mid-word, and breaks a word too long for
        // the card instead of running past it. Left-aligned so a name that does overrun keeps its
        // BEGINNING: a centred single line loses both ends and reads as neither.
        .Set(Style::WhiteSpaceProp, WhiteSpace::Normal)
        .Set(Style::OverflowWrapProp, OverflowWrap::BreakWord)
        .Set(Style::TextAlignProp, TextAlign::Left)
        .Set(Style::OverflowProp, Overflow::Hidden);
    card.AddChild(std::move(name));
    return raw;
}

// Mouse-down state for one card. Held by the handler rather than by the element, because the
// element is rebuilt whenever the inspector is and the state belongs to the live control.
using ClickStamp = std::shared_ptr<std::chrono::steady_clock::time_point>;

void RegisterDoubleClickActivation(UIElement& card, const std::function<void()>& activate)
{
    ClickStamp lastClick = std::make_shared<std::chrono::steady_clock::time_point>();
    card.RegisterEventHandler(
        kEventMouseDown,
        [lastClick, activate](UIEvent& event)
        {
            // A card with nowhere to navigate consumes nothing: swallowing the press would take it
            // from whatever else would have handled it and give nothing back.
            if (event.Button != 0 || !activate)
                return;
            event.Stop();

            const auto now = std::chrono::steady_clock::now();
            if ((now - *lastClick) < Platform::GetDoubleClickInterval())
            {
                // Cleared so a third click opens a new pair instead of activating again.
                *lastClick = {};
                activate();
                return;
            }
            *lastClick = now;
        });
}

void AddMaterialCard(UIElement& grid, const TerrainMaterialEntry& entry,
                     const std::function<void()>& activate)
{
    const std::string slot = std::to_string(static_cast<int>(entry.SlotId));
    const std::string id = "terrain-material-" + slot;
    const std::string name = MaterialDisplayName(entry);

    auto card = std::make_unique<UIElement>();
    UIElement* raw = card.get();
    raw->SetId(id + "-card");
    StyleCardSurface(*raw);
    raw->SetTooltip(name + " — slot " + slot +
                    ", which is what painted ground stores. Double-click to edit its name, "
                    "textures and values in the library.");

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    swatchRaw->SetId(id + "-swatch");
    swatchRaw->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::FlexStart)
        .Set(Style::Height, StyleLength::Px(kCardSwatchHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kCardSwatchHeightPx))
        .Set(Style::BorderRadius,
             CornerRadiiTLTRBRBL{kSwatchRadiusPx, kSwatchRadiusPx, kSwatchRadiusPx,
                                 kSwatchRadiusPx})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kSwatchBorderArgb, kSwatchBorderArgb,
                                                  kSwatchBorderArgb, kSwatchBorderArgb})
        // The tint is linear and every CSS colour is an sRGB byte, so the card shows the encoded
        // value the library inspector's own swatch does.
        .Set(Style::BackgroundColor, TerrainMaterialTintToSwatchArgb(entry));

    // The swatch is a colour, and a colour can land anywhere — including a flat white that is
    // indistinguishable from a picture that failed to load. The chip is what tells those apart: a
    // white swatch under it reads "the tint is white", which is the truth, rather than "broken".
    auto chip = std::make_unique<Label>();
    chip->SetId(id + "-tintchip");
    chip->SetText("Tint");
    chip->Overrides()
        .Set(Style::FontSize, StyleLength::Px(kSwatchChipFontPx))
        .Set(Style::Color, kSwatchChipTextArgb)
        .Set(Style::BackgroundColor, kSwatchChipBackgroundArgb)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::MarginTop, StyleLength::Px(4.0f))
        .Set(Style::MarginLeft, StyleLength::Px(4.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(4.0f))
        .Set(Style::PaddingRight, StyleLength::Px(4.0f))
        .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap);
    swatchRaw->AddChild(std::move(chip));

    raw->AddChild(std::move(swatch));

    AddCardName(*raw, name, id + "-name");
    RegisterDoubleClickActivation(*raw, activate);
    grid.AddChild(std::move(card));
}

void AddAddTile(UIElement& grid, const std::function<void()>& add)
{
    auto tile = std::make_unique<UIElement>();
    UIElement* raw = tile.get();
    raw->SetId("terrain-material-add");
    StyleCardSurface(*raw);
    raw->SetTooltip("Appends a material taking the lowest free slot ID, so it can never inherit a "
                    "retired material's painted ground.");

    auto glyph = std::make_unique<Label>();
    glyph->SetId("terrain-material-add-glyph");
    glyph->SetText("+");
    glyph->Overrides()
        .Set(Style::Height, StyleLength::Px(kCardSwatchHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kCardSwatchHeightPx))
        .Set(Style::FontSize, StyleLength::Px(kAddTileGlyphFontPx))
        .Set(Style::Color, kAddTileGlyphArgb)
        .Set(Style::TextAlignProp, TextAlign::Center);
    raw->AddChild(std::move(glyph));

    AddCardName(*raw, "Add", "terrain-material-add-name");

    // One click, unlike the cards: this tile is an action, and a "+" that ignores the first click
    // reads as a dead control. A tile with no action swallows nothing, as a card with no
    // destination does not.
    raw->RegisterEventHandler(kEventMouseDown,
                              [add](UIEvent& event)
                              {
                                  if (event.Button != 0 || !add)
                                      return;
                                  event.Stop();
                                  add();
                              });
    grid.AddChild(std::move(tile));
}

// The whole document, captured before an edit and rewritten on undo.
UndoRedoService::SnapshotTarget MakeLibrarySnapshotTarget(TerrainMaterialLibraryAsset* library,
                                                          const std::string& label)
{
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [library](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        if (!library)
            return false;
        String text;
        if (!ReadFileTextShared(library->GetPath(), text))
            return false;
        out.assign(text.begin(), text.end());
        return true;
    };

    target.Apply = [library](const UndoRedoService::SnapshotTarget::Snapshot& snapshot) -> bool
    {
        if (!library)
            return false;
        std::ofstream out(library->GetPath(), std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        if (!snapshot.empty())
            out.write(reinterpret_cast<const char*>(snapshot.data()),
                      static_cast<std::streamsize>(snapshot.size()));
        out.close();
        return library->Load();
    };

    return target;
}

} // namespace

TerrainMaterialLibraryAsset* ResolveLibrary(const Components::Terrain& terrain)
{
    const GUID guid = terrain.MaterialLibraryGuid.ToGuid();
    if (guid.IsNull())
        return nullptr;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return nullptr;

    auto asset = engine.GetAssetManager().GetAsset(guid);
    if (!asset || asset->GetType() != AssetType::TerrainMaterialLibrary)
        return nullptr;
    return static_cast<TerrainMaterialLibraryAsset*>(asset.get());
}

void EditLibraryUndoable(TerrainMaterialLibraryAsset* library, UndoRedoService* undo,
                         const std::string& label, const std::function<void()>& requestRefresh,
                         const std::function<void(std::vector<TerrainMaterialEntry>&)>& mutate)
{
    if (!library || !mutate)
        return;

    UndoRedoService::InteractiveEdit edit;
    if (undo)
        edit = undo->BeginInteractiveEdit(label, MakeLibrarySnapshotTarget(library, label));

    mutate(library->EditMaterials());
    (void)library->Save();

    if (edit)
        edit.Commit();
    if (requestRefresh)
        requestRefresh();
}

std::string RoleLabel(const Components::Terrain& terrain,
                      const TerrainMaterialLibraryAsset* library, std::uint32_t role)
{
    if (role >= TerrainLayers::kCount)
        return {};
    if (library)
    {
        if (const TerrainMaterialEntry* entry =
                library->FindBySlotId(terrain.LayerRoleSlot[role]);
            entry && !entry->Name.empty())
            return entry->Name;
    }
    return TerrainLayers::kNames[role];
}

GUID RoleAlbedoTexture(const Components::Terrain& terrain,
                       const TerrainMaterialLibraryAsset* library, std::uint32_t role)
{
    if (role >= TerrainLayers::kCount)
        return {};
    if (library)
    {
        const TerrainMaterialEntry* entry = library->FindBySlotId(terrain.LayerRoleSlot[role]);
        return entry ? entry->AlbedoTexture : GUID{};
    }
    return terrain.LayerAlbedoTexture[role].ToGuid();
}

std::vector<Dropdown::Option> BuildRoleOptions(const Components::Terrain& terrain,
                                               const TerrainMaterialLibraryAsset* library)
{
    std::vector<Dropdown::Option> options;
    options.reserve(TerrainLayers::kCount);
    for (std::uint32_t role = 0; role < TerrainLayers::kCount; ++role)
        options.push_back({std::to_string(role), RoleLabel(terrain, library, role)});
    return options;
}

std::vector<Dropdown::Option> BuildMaterialOptions(const TerrainMaterialLibraryAsset* library)
{
    std::vector<Dropdown::Option> options;
    bool canCreate = true;
    if (library)
    {
        // Counted before any label is built: whether a name needs its slot spelled out is a
        // property of the whole live list, not of the entry being labelled.
        std::unordered_map<std::string, int> nameUses;
        for (const TerrainMaterialEntry& entry : library->GetMaterials())
            if (!entry.Retired)
                ++nameUses[MaterialDisplayName(entry)];

        for (const TerrainMaterialEntry& entry : library->GetMaterials())
        {
            if (entry.Retired)
                continue;
            const std::string slot = std::to_string(static_cast<int>(entry.SlotId));
            const std::string name = MaterialDisplayName(entry);
            options.push_back({slot, nameUses[name] > 1 ? name + " (slot " + slot + ")" : name});
        }
        canCreate = library->NextFreeSlotId() != TerrainMaterialLibraryAsset::kInvalidSlotId;
    }
    if (canCreate)
        options.push_back({kCreateMaterialOptionValue, "+ Create new material…"});
    return options;
}

std::uint32_t AddLibraryMaterial(TerrainMaterialLibraryAsset* library, UndoRedoService* undo,
                                 const std::function<void()>& requestRefresh)
{
    std::uint32_t created = kInvalidTerrainMaterialSlotId;
    EditLibraryUndoable(library, undo, "Add Terrain Material", requestRefresh,
                        [&created](std::vector<TerrainMaterialEntry>& entries)
                        {
                            const std::uint32_t slot = NextFreeTerrainMaterialSlotId(entries);
                            if (slot == kInvalidTerrainMaterialSlotId)
                                return;
                            TerrainMaterialEntry entry{};
                            entry.SlotId = static_cast<std::uint8_t>(slot);
                            entry.Name = "Material " + std::to_string(slot);
                            entries.push_back(std::move(entry));
                            created = slot;
                        });
    return created;
}

int SelectedMaterialOption(const Components::Terrain& terrain,
                           const TerrainMaterialLibraryAsset* library, std::uint32_t role)
{
    if (!library || role >= TerrainLayers::kCount)
        return -1;

    const std::uint8_t boundSlot = terrain.LayerRoleSlot[role];
    int index = 0;
    for (const TerrainMaterialEntry& entry : library->GetMaterials())
    {
        if (entry.Retired)
            continue;
        if (entry.SlotId == boundSlot)
            return index;
        ++index;
    }
    return -1;
}

UIElement* AddMaterialGrid(UIElement* parent, const TerrainMaterialLibraryAsset* library,
                           const std::function<void()>& onAdd,
                           const std::function<void(const std::filesystem::path&)>& openAsset)
{
    if (!parent || !library)
        return nullptr;

    auto grid = std::make_unique<UIElement>();
    UIElement* raw = grid.get();
    raw->SetId("terrain-material-grid");
    raw->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::FlexWrap, true)
        .Set(Style::Gap, StyleLength::Px(kGridGapPx))
        .Set(Style::MarginTop, StyleLength::Px(4.0f))
        .Set(Style::MarginBottom, StyleLength::Px(6.0f))
        // Stretched, so the grid takes the panel's width in the inspector's column and its cards
        // have a width to wrap against. Content-sized, they would all sit on one line.
        .Set(Style::AlignSelf, AlignItems::Stretch);

    // The library's own path, resolved once: a card opens the library asset, which is what shows
    // its materials in the Inspector.
    const std::filesystem::path path = library->GetPath();
    std::function<void()> activate;
    if (openAsset && !path.empty())
        activate = [openAsset, path] { openAsset(path); };

    for (const TerrainMaterialEntry& entry : library->GetMaterials())
    {
        if (entry.Retired)
            continue;
        AddMaterialCard(*raw, entry, activate);
    }

    if (library->NextFreeSlotId() != TerrainMaterialLibraryAsset::kInvalidSlotId)
        AddAddTile(*raw, onAdd);

    parent->AddChild(std::move(grid));
    return raw;
}

LayerRoleBlock AddLayerRoleBlock(UIElement* parent, const Components::Terrain& terrain,
                                   const TerrainMaterialLibraryAsset* library)
{
    LayerRoleBlock block;
    if (!parent || !library)
        return block;

    // Heads exactly like every other heading in this panel: one bar style, whatever level a block
    // sits at. The only thing that sets this one apart is what is under it.
    block.Block = InspectorUI::AddComponentSection(parent, "Terrain/Layer Materials",
                                                  "Layer Materials");
    if (!block.Block)
        return block;
    block.Block->SetId("terrain-layer-roles");
    block.Block->SetTooltip(kLayerRoleTooltip);
    UIElement* content = block.Block->GetContentContainer();

    // One option list for all four rows: every row offers the same library, and building it four
    // times would be four chances for two rows to disagree about what the library holds.
    const std::vector<Dropdown::Option> options = BuildMaterialOptions(library);
    block.Pickers.reserve(TerrainLayers::kCount);

    for (std::uint32_t role = 0; role < TerrainLayers::kCount; ++role)
    {
        const std::string id = "terrain-role-" + std::to_string(role);

        UIElement* row = InspectorUI::AddRow(content);
        row->SetId(id + "-row");

        Label* name = InspectorUI::AddLabel(row, TerrainLayers::kNames[role], kRoleNameTooltip);
        if (name)
            name->SetId(id + "-name");

        UIElement* field = InspectorUI::AddFieldContainer(row);
        auto picker = std::make_unique<Dropdown>();
        picker->SetId(id + "-material");
        picker->AddClass("inspector-dropdown");
        picker->SetTooltip(kRolePickerTooltip);
        picker->SetOptions(options, SelectedMaterialOption(terrain, library, role));
        block.Pickers.push_back(picker.get());
        field->AddChild(std::move(picker));
    }

    return block;
}

Button* AddOpenLibraryRow(UIElement* parent, const TerrainMaterialLibraryAsset* library,
                          const std::function<void(const std::filesystem::path&)>& openAsset)
{
    if (!parent || !library || !openAsset)
        return nullptr;

    const std::filesystem::path path = library->GetPath();
    if (path.empty())
        return nullptr;

    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, "");
    UIElement* field = InspectorUI::AddFieldContainer(row);

    auto button = std::make_unique<Button>();
    Button* raw = button.get();
    raw->SetId("terrain-open-material-library");
    raw->SetText("Edit Materials");
    raw->SetTooltip("Selects the library asset, which opens it in the Inspector: one list of "
                    "materials, shared by every terrain bound to it.");
    raw->RegisterEventHandler(kEventButtonClick, [openAsset, path](UIEvent&) { openAsset(path); });
    field->AddChild(std::move(button));
    return raw;
}

} // namespace GameEngine::Editor::TerrainRoleMaterials
