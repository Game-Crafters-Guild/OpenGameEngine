// The terrain panel's material surface: the library's materials as the grid the panel shows, the
// channel-role binding under them, and the picker semantics both read from. All of them read the
// terrain's role slots and the library's entries together, and all of them have to agree with the
// runtime resolve — a panel that shows one material while the surface shades another is the
// failure this covers.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "Inspectors/TerrainMaterialLibraryInspector.h"
#include "Platform/SystemMetrics.h"
#include "Terrain/TerrainRoleMaterials.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace GameEngine;
namespace Roles = GameEngine::Editor::TerrainRoleMaterials;

namespace
{

constexpr const char* kLibraryGuid = "8a1c2d3e-4f50-4617-8293-a4b5c6d7e8f9";
// Stands in for a bound albedo texture. Never resolved against a registry here — these tests
// pin which GUID a channel reports, not what the editor then does with it.
constexpr const char* kAlbedoGuid = "1f2e3d4c-5b6a-4798-8172-0a1b2c3d4e5f";

TerrainMaterialEntry MakeEntry(std::uint8_t slotId, const std::string& name, bool retired = false)
{
    TerrainMaterialEntry entry{};
    entry.SlotId = slotId;
    entry.Name = name;
    entry.Retired = retired;
    return entry;
}

// A writable path per process: the add tile's edit goes through the library's Save, and two
// concurrent EditorTests runs sharing one file would each overwrite the other's document.
std::filesystem::path MakeLibraryPath(const std::string& name)
{
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("ge_terrain_role_materials_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / (name + ".terrainmatlib");
    std::filesystem::remove(path, ec);
    return path;
}

std::string RoleRowId(int role, const char* suffix)
{
    return "terrain-role-" + std::to_string(role) + suffix;
}

Label* RoleRowName(UIElement& root, int role)
{
    return dynamic_cast<Label*>(root.FindById(RoleRowId(role, "-name")));
}

Dropdown* RoleRowPicker(UIElement& root, int role)
{
    return dynamic_cast<Dropdown*>(root.FindById(RoleRowId(role, "-material")));
}

std::string CardId(int slot, const char* suffix)
{
    return "terrain-material-" + std::to_string(slot) + suffix;
}

// What a picker OFFERS, read off the items the control built rather than off the options vector it
// was handed — the question is what the user can pick, not what the caller passed.
std::vector<std::string> RolePickerOptionLabels(const Dropdown& picker)
{
    std::vector<std::string> labels;
    const UIElement* items = picker.GetItemsContainer();
    if (!items)
        return labels;
    for (const auto& child : items->GetChildren())
        if (const Label* label = dynamic_cast<const Label*>(child.get()))
            labels.push_back(label->GetText());
    return labels;
}

// The dispatch the UI module's own control tests use: hand the element an event and let its
// registered handlers run.
void SendMouse(UIElement& el, EventId id, int button = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = 0.0f;
    e.Y = 0.0f;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

} // namespace

// A channel is named by the material bound to it. Falling back to the semantic name for every
// channel — which is all the process-lifetime static dropdown could ever do — hides the binding
// completely.
TEST(TerrainRoleMaterials, ChannelIsNamedByTheMaterialBoundToIt)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(9, "Wet Sand"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[0] = 9; // grass channel shades with Wet Sand

    EXPECT_EQ(Roles::RoleLabel(terrain, &library, 0), "Wet Sand");
    // Slot 1 exists in no entry, so the channel keeps its semantic name.
    EXPECT_EQ(Roles::RoleLabel(terrain, &library, 1), "Rock");
    // No library at all is the unmigrated terrain: every channel is semantic.
    EXPECT_EQ(Roles::RoleLabel(terrain, nullptr, 0), "Grass");
}

// The rule row shows the result material's ALBEDO beside its picker, and the row carries no
// other identity: a name answers "which entry", an image answers "what will this look like",
// and two materials called "Rock 02" and "Rock 03" are indistinguishable until the ground bakes.
//
// The three ways this comes back null are one answer for the widget — no element at all — but
// they are different facts, so each is pinned. A blank chip for any of them would read as "this
// rule paints white", which a material can genuinely be authored to do.
TEST(TerrainRoleMaterials, ChannelAlbedoComesFromTheMaterialBoundToIt)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    TerrainMaterialEntry textured = MakeEntry(9, "Wet Sand");
    textured.AlbedoTexture = GUID(kAlbedoGuid);
    library.EditMaterials().push_back(textured);
    library.EditMaterials().push_back(MakeEntry(4, "Flat Tint")); // binds no albedo

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[0] = 9;
    terrain.LayerRoleSlot[1] = 4;
    terrain.LayerRoleSlot[2] = 200; // no entry holds this slot

    EXPECT_EQ(Roles::RoleAlbedoTexture(terrain, &library, 0), GUID(kAlbedoGuid));
    // An untextured material shades from its tint, so there is no image to show.
    EXPECT_TRUE(Roles::RoleAlbedoTexture(terrain, &library, 1).IsNull());
    // A slot left over from a removed material shades the built-in.
    EXPECT_TRUE(Roles::RoleAlbedoTexture(terrain, &library, 2).IsNull());
    // Out of range: the four splat channels are all there are.
    EXPECT_TRUE(Roles::RoleAlbedoTexture(terrain, &library, 4).IsNull());
}

// Without a library the terrain's own per-layer field is what the surface shades from
// (TerrainMaterialAuthoring's legacy record), so the row reads it there rather than showing an
// unmigrated terrain nothing at all.
TEST(TerrainRoleMaterials, AnUnmigratedChannelTakesItsAlbedoFromTheTerrainsOwnField)
{
    Components::Terrain terrain{};
    terrain.LayerAlbedoTexture[2].Set(GUID(kAlbedoGuid));

    EXPECT_EQ(Roles::RoleAlbedoTexture(terrain, nullptr, 2), GUID(kAlbedoGuid));
    EXPECT_TRUE(Roles::RoleAlbedoTexture(terrain, nullptr, 0).IsNull());
}

// A bound library is the sole authority for what a channel shades with (Terrain.h), so the
// legacy field must not leak through one that resolves. A panel showing a texture the surface
// will not sample is the class of failure this file exists to prevent.
TEST(TerrainRoleMaterials, ABoundLibraryShadowsTheTerrainsLegacyAlbedoField)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Flat Tint")); // binds no albedo

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[0] = 0;
    terrain.LayerAlbedoTexture[0].Set(GUID(kAlbedoGuid));

    EXPECT_TRUE(Roles::RoleAlbedoTexture(terrain, &library, 0).IsNull());
}

TEST(TerrainRoleMaterials, ChannelOptionsAreTheFourChannelsInChannelOrder)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(2, "Cobble"));

    Components::Terrain terrain{};
    const std::vector<Dropdown::Option> options = Roles::BuildRoleOptions(terrain, &library);

    ASSERT_EQ(options.size(), 4u);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(options[static_cast<size_t>(i)].value, std::to_string(i));
    EXPECT_EQ(options[2].label, "Cobble");
    EXPECT_EQ(options[0].label, "Grass");
}

// A tombstone keeps painted ground shading and disappears from pickers. Offering one here would
// re-bind a channel to a material the author explicitly retired.
TEST(TerrainRoleMaterials, RetiredMaterialsAreNotOfferedAndCreateIsLast)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(1, "Old Mud", /*retired*/ true));
    library.EditMaterials().push_back(MakeEntry(2, "Cobble"));

    const std::vector<Dropdown::Option> options = Roles::BuildMaterialOptions(&library);

    ASSERT_EQ(options.size(), 3u); // two live materials + create
    EXPECT_EQ(options[0].value, "0");
    EXPECT_EQ(options[1].value, "2");
    EXPECT_EQ(options.back().value, Roles::kCreateMaterialOptionValue);
    for (const Dropdown::Option& option : options)
        EXPECT_EQ(option.label.find("Old Mud"), std::string::npos);
}

// A slot ID the author never typed has no business on a row that a name already identifies — it
// read as the panel hardcoding numbers. It earns its place only where the name stops being an
// answer, which is when a second LIVE material carries it.
TEST(TerrainRoleMaterials, AMaterialIsLabelledByNameUntilTwoLiveOnesShareIt)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(3, "Cobble"));
    library.EditMaterials().push_back(MakeEntry(7, "Cobble"));
    library.EditMaterials().push_back(MakeEntry(9, "Old Mud", /*retired*/ true));

    const std::vector<Dropdown::Option> options = Roles::BuildMaterialOptions(&library);
    ASSERT_EQ(options.size(), 4u); // three live materials + create

    EXPECT_EQ(options[0].label, "Meadow") << "a name only one material carries needs no slot";
    EXPECT_EQ(options[1].label, "Cobble (slot 3)");
    EXPECT_EQ(options[2].label, "Cobble (slot 7)");

    // A retired namesake is not a collision: it is not offered, so nothing it could be confused
    // with is on the list.
    TerrainMaterialLibraryAsset shadowed(GUID(kLibraryGuid), "unused.terrainmatlib");
    shadowed.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    shadowed.EditMaterials().push_back(MakeEntry(4, "Meadow", /*retired*/ true));
    const std::vector<Dropdown::Option> live = Roles::BuildMaterialOptions(&shadowed);
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0].label, "Meadow");
}

// The option index has to count the rows the picker SHOWS, not the rows the library holds: a
// retired row between the top and the bound material would otherwise select its neighbour.
TEST(TerrainRoleMaterials, SelectedOptionSkipsTheRetiredRowsThePickerHides)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(1, "Old Mud", /*retired*/ true));
    library.EditMaterials().push_back(MakeEntry(2, "Cobble"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[0] = 2; // Cobble — row 2, but only option 1

    EXPECT_EQ(Roles::SelectedMaterialOption(terrain, &library, 0), 1);

    const std::vector<Dropdown::Option> options = Roles::BuildMaterialOptions(&library);
    ASSERT_GT(options.size(), 1u);
    EXPECT_EQ(options[1].value, "2");
}

// A channel bound to a slot no entry holds — a purged material — shows nothing selected rather
// than pointing at whichever material happens to sit at that index.
TEST(TerrainRoleMaterials, ChannelBoundToAPurgedSlotSelectsNothing)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(1, "Cobble"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[3] = 200;

    EXPECT_EQ(Roles::SelectedMaterialOption(terrain, &library, 3), -1);
    EXPECT_EQ(Roles::SelectedMaterialOption(terrain, nullptr, 0), -1);
}

// Create has to disappear once every slot ID is spoken for — a create that silently no-ops, or
// worse binds slot 255, is the state the library inspector's Add button already disables for.
TEST(TerrainRoleMaterials, AFullLibraryOffersNoCreateOption)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "unused.terrainmatlib");
    for (int slot = 0; slot < 256; ++slot)
        library.EditMaterials().push_back(
            MakeEntry(static_cast<std::uint8_t>(slot), "Material " + std::to_string(slot)));
    ASSERT_EQ(library.NextFreeSlotId(), TerrainMaterialLibraryAsset::kInvalidSlotId);

    const std::vector<Dropdown::Option> options = Roles::BuildMaterialOptions(&library);
    ASSERT_EQ(options.size(), 256u);
    for (const Dropdown::Option& option : options)
        EXPECT_NE(option.value, Roles::kCreateMaterialOptionValue);
}

// With no library there is nothing to bind to, but creating one is still the way forward, so the
// picker degrades to exactly that one option rather than to an empty list.
TEST(TerrainRoleMaterials, NoLibraryStillOffersCreate)
{
    const std::vector<Dropdown::Option> options = Roles::BuildMaterialOptions(nullptr);
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].value, Roles::kCreateMaterialOptionValue);
}

// ---------------------------------------------------------------------------------------------
// The add path both surfaces take
// ---------------------------------------------------------------------------------------------

// One append, one answer about which slot it took. The library panel's Add button and the terrain
// grid's add tile are the same act, and two copies of it would be two rules for which slot a new
// material lands on — the rule that keeps it off a retired material's painted ground.
TEST(TerrainRoleMaterials, AddingAMaterialTakesTheLowestFreeSlotAndReportsIt)
{
    const std::filesystem::path path = MakeLibraryPath("add_lowest_free");
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), path);
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(2, "Cobble"));

    int refreshed = 0;
    const std::uint32_t slot =
        Roles::AddLibraryMaterial(&library, nullptr, [&refreshed] { ++refreshed; });

    EXPECT_EQ(slot, 1u) << "the gap between 0 and 2 is the lowest free slot";
    ASSERT_EQ(library.GetMaterials().size(), 3u);
    const TerrainMaterialEntry* added = library.FindBySlotId(1);
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->Name, "Material 1");
    EXPECT_FALSE(added->Retired);
    EXPECT_EQ(refreshed, 1) << "the panel was never told to rebuild around the new material";

    // Written, not only applied: the terrain shades from a cached parse of the file, which the
    // reload the write triggers is what drops.
    EXPECT_TRUE(std::filesystem::exists(path));
}

// A library with every ID spoken for has nothing to allocate. Reporting a slot anyway is how a
// caller ends up binding a channel to a material that was never added.
TEST(TerrainRoleMaterials, AddingToAFullLibraryAddsNothingAndReportsNoSlot)
{
    const std::filesystem::path path = MakeLibraryPath("add_full");
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), path);
    for (int slot = 0; slot < 256; ++slot)
        library.EditMaterials().push_back(
            MakeEntry(static_cast<std::uint8_t>(slot), "Material " + std::to_string(slot)));

    EXPECT_EQ(Roles::AddLibraryMaterial(&library, nullptr, {}), kInvalidTerrainMaterialSlotId);
    EXPECT_EQ(library.GetMaterials().size(), 256u);
    EXPECT_EQ(Roles::AddLibraryMaterial(nullptr, nullptr, {}), kInvalidTerrainMaterialSlotId);
}

// ---------------------------------------------------------------------------------------------
// The material grid
// ---------------------------------------------------------------------------------------------

// The acceptance condition the layout exists to meet: the panel's face is the LIBRARY'S MATERIALS,
// one card each, in the library's order — not the four splat channels, which are a binding
// over them. A retired material is not offered, the same rule the pickers follow.
TEST(TerrainRoleMaterials, TheGridIsOneCardPerLiveMaterialThenTheAddTile)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(4, "Old Mud", /*retired*/ true));
    library.EditMaterials().push_back(MakeEntry(7, "Packed Snow"));

    UIElement root("div");
    UIElement* grid = Roles::AddMaterialGrid(&root, &library, {}, {});
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(root.FindById("terrain-material-grid"), grid);

    const auto& cells = grid->GetChildren();
    ASSERT_EQ(cells.size(), 3u) << "two live materials and the add tile";
    EXPECT_EQ(cells[0]->GetId(), CardId(0, "-card"));
    EXPECT_EQ(cells[1]->GetId(), CardId(7, "-card"));
    EXPECT_EQ(cells[2]->GetId(), "terrain-material-add") << "the add tile is not last";

    EXPECT_EQ(root.FindById(CardId(4, "-card")), nullptr) << "a retired material has a card";

    // Wrapping is what makes the grid answer to the panel's width; a single row that overflows is
    // the failure a fixed card width otherwise guarantees.
    EXPECT_EQ(grid->Overrides().Get(Style::FlexWrap), std::optional<bool>(true));
}

// The row has to reach the width the fields beside it reach. Cards of one fixed width divide it
// with a remainder, and the remainder showed as a dead third on the right of the only row.
//
// BOUNDARY: this asserts the inputs Yoga wraps and distributes on — the grid's stretch and wrap,
// and each cell's basis, growth and cap. It does NOT measure the laid-out result: no layout pass
// runs in this process, so the number of rows at a given panel width is not observable here and is
// not claimed.
TEST(TerrainRoleMaterials, CardsShareOutTheRowRatherThanLeavingItShort)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    for (int slot = 0; slot < 6; ++slot)
        library.EditMaterials().push_back(
            MakeEntry(static_cast<std::uint8_t>(slot), "Material " + std::to_string(slot)));

    UIElement root("div");
    UIElement* grid = Roles::AddMaterialGrid(&root, &library, {}, {});
    ASSERT_NE(grid, nullptr);

    // Stretched to the section body's width, which is what gives wrapping something to wrap
    // against — content-sized, the row would end wherever its cards happened to end.
    EXPECT_EQ(grid->Overrides().Get(Style::AlignSelf),
              std::optional<AlignItems>(AlignItems::Stretch));

    const auto& cells = grid->GetChildren();
    ASSERT_EQ(cells.size(), 7u) << "six materials and the add tile";
    for (const auto& cell : cells)
    {
        const std::optional<float> grow = cell->Overrides().Get(Style::FlexGrow);
        ASSERT_TRUE(grow.has_value()) << cell->GetId() << " cannot take up slack";
        EXPECT_GT(*grow, 0.0f) << cell->GetId() << " leaves the row short";

        // Bounded on both ends: never below a card, never grown past one.
        const std::optional<StyleLength> basis = cell->Overrides().Get(Style::FlexBasis);
        const std::optional<StyleLength> minimum = cell->Overrides().Get(Style::MinWidth);
        const std::optional<StyleLength> maximum = cell->Overrides().Get(Style::MaxWidth);
        ASSERT_TRUE(basis.has_value() && minimum.has_value() && maximum.has_value())
            << cell->GetId();
        EXPECT_FLOAT_EQ(basis->Value, minimum->Value) << cell->GetId() << " starts off its floor";
        EXPECT_GT(maximum->Value, minimum->Value) << cell->GetId() << " cannot grow at all";
        EXPECT_LE(maximum->Value, minimum->Value * 2.0f)
            << cell->GetId() << " may grow past a card into a banner";
    }
}

// A name too wide for its card runs onto a second line, anchored at its START. The single centred
// line it replaces was clipped at BOTH ends — "Weathered Limestone Scree" showed as "...red
// Limestone Scre", which names no material at all.
//
// Two lines are RESERVED rather than grown into, so every card keeps one height and a row of them
// stays a grid. A third line is clipped: the UI module has no line-clamp and no text-overflow, so
// an ellipsis cannot be asked for — losing the tail of a very long name beats losing its start.
//
// BOUNDARY: this asserts the inputs the text layout acts on, not the laid-out lines. No layout pass
// runs in this process, so "it occupies exactly two lines" is not observable here.
TEST(TerrainRoleMaterials, ALongNameWrapsToASecondLineFromItsStart)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(2, "Weathered Limestone Scree"));
    // A single word with nowhere to break, which is what runs past a card if nothing breaks words.
    library.EditMaterials().push_back(MakeEntry(3, "Unbrokenlimestonescreename"));

    UIElement root("div");
    ASSERT_NE(Roles::AddMaterialGrid(&root, &library, {}, {}), nullptr);

    UIElement* card = root.FindById(CardId(2, "-card"));
    Label* name = dynamic_cast<Label*>(root.FindById(CardId(2, "-name")));
    ASSERT_NE(card, nullptr);
    ASSERT_NE(name, nullptr);

    // The name is carried whole — what the card does is a display decision, not a truncation of
    // the data, so the tooltip and any later widening still have all of it.
    EXPECT_EQ(name->GetText(), "Weathered Limestone Scree");

    // Wraps instead of being cut mid-line...
    EXPECT_EQ(name->Overrides().Get(Style::WhiteSpaceProp),
              std::optional<WhiteSpace>(WhiteSpace::Normal))
        << "the name is still one clipped line";
    // ...starts at the beginning, which is the half the centred clip was eating...
    EXPECT_EQ(name->Overrides().Get(Style::TextAlignProp), std::optional<TextAlign>(TextAlign::Left))
        << "a name that overruns loses its start again";
    // ...and a word with no break in it breaks rather than escaping the card.
    EXPECT_EQ(name->Overrides().Get(Style::OverflowWrapProp),
              std::optional<OverflowWrap>(OverflowWrap::BreakWord))
        << "an unbreakable name runs past its card";

    // Two lines, reserved: fixed height, so a one-word card and a five-word card are the same size.
    const std::optional<StyleLength> height = name->Overrides().Get(Style::Height);
    const std::optional<StyleLength> minHeight = name->Overrides().Get(Style::MinHeight);
    const std::optional<StyleLength> maxHeight = name->Overrides().Get(Style::MaxHeight);
    const std::optional<float> lineHeight = name->Overrides().Get(Style::LineHeight);
    ASSERT_TRUE(height.has_value() && minHeight.has_value() && maxHeight.has_value());
    ASSERT_TRUE(lineHeight.has_value());
    EXPECT_FLOAT_EQ(height->Value, *lineHeight * 2.0f) << "the name block is not two lines tall";
    EXPECT_FLOAT_EQ(minHeight->Value, height->Value);
    EXPECT_FLOAT_EQ(maxHeight->Value, height->Value) << "a third line grows the card past its row";
    // A line has to fit the glyphs it holds, or two lines of reserve show one and a half.
    const std::optional<StyleLength> font = name->Overrides().Get(Style::FontSize);
    ASSERT_TRUE(font.has_value());
    EXPECT_GT(*lineHeight, font->Value) << "the line box is tighter than the type in it";

    // Whatever spills past line two is caught by the card, not painted over its neighbour.
    EXPECT_EQ(name->Overrides().Get(Style::OverflowProp), std::optional<Overflow>(Overflow::Hidden));
    EXPECT_EQ(card->Overrides().Get(Style::OverflowProp), std::optional<Overflow>(Overflow::Hidden))
        << "a long name escapes its card";

    // The unbreakable name gets the same treatment rather than a special case.
    Label* unbroken = dynamic_cast<Label*>(root.FindById(CardId(3, "-name")));
    ASSERT_NE(unbroken, nullptr);
    EXPECT_EQ(unbroken->GetText(), "Unbrokenlimestonescreename");
    EXPECT_EQ(unbroken->Overrides().Get(Style::OverflowWrapProp),
              std::optional<OverflowWrap>(OverflowWrap::BreakWord));
}

// A card has to say which material it is, in both the way you scan a palette and the way you read
// a list: the tint the terrain shades with, and the name the author gave it.
TEST(TerrainRoleMaterials, ACardShowsItsMaterialsTintAndName)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(5, "Packed Snow"));
    TerrainMaterialEntry* authored = FindTerrainMaterialBySlotIdMutable(library.EditMaterials(), 5);
    ASSERT_NE(authored, nullptr);
    authored->AlbedoR = 0.25f;
    authored->AlbedoG = 0.5f;
    authored->AlbedoB = 0.75f;

    UIElement root("div");
    ASSERT_NE(Roles::AddMaterialGrid(&root, &library, {}, {}), nullptr);

    UIElement* swatch = root.FindById(CardId(5, "-swatch"));
    ASSERT_NE(swatch, nullptr);
    // The tint is linear and a background colour is an sRGB byte triple, so the swatch must show
    // the ENCODED value — the same one the library inspector's own swatch shows.
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor),
              std::optional<std::uint32_t>(TerrainMaterialTintToSwatchArgb(*authored)));

    Label* name = dynamic_cast<Label*>(root.FindById(CardId(5, "-name")));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->GetText(), "Packed Snow");
    const std::optional<StyleLength> size = name->Overrides().Get(Style::FontSize);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->Unit, StyleLength::UnitType::Px);
    // Editor UI floor — a card is the smallest text in this panel, so it is where a shrink lands.
    EXPECT_GE(size->Value, 12.0f);
    // And it reads at the panel's own label size (.inspector-label, 13px) rather than at the
    // floor: a name a size below every label beside it looked like a caption, not an identity.
    EXPECT_FLOAT_EQ(size->Value, 13.0f);

    // The slot is still reachable — it is what painted ground stores — without being printed on
    // the card, which is what read as hardcoded.
    UIElement* card = root.FindById(CardId(5, "-card"));
    ASSERT_NE(card, nullptr);
    EXPECT_NE(card->GetTooltip().find("slot 5"), std::string::npos);
    EXPECT_EQ(name->GetText().find("slot"), std::string::npos);
}

// A tint can land on flat white, which is pixel-for-pixel what a picture that failed to load looks
// like. The chip is what makes the card honest: it names the thing the swatch is showing, so a
// white card says "the tint is white" instead of leaving the reader to guess it broke.
TEST(TerrainRoleMaterials, ASwatchSaysItIsATintEvenWhenTheTintIsWhite)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Grass"));
    TerrainMaterialEntry* authored = FindTerrainMaterialBySlotIdMutable(library.EditMaterials(), 0);
    ASSERT_NE(authored, nullptr);
    authored->AlbedoR = 1.0f;
    authored->AlbedoG = 1.0f;
    authored->AlbedoB = 1.0f;

    UIElement root("div");
    ASSERT_NE(Roles::AddMaterialGrid(&root, &library, {}, {}), nullptr);

    // The state that made the fix necessary: this swatch is pure white.
    UIElement* swatch = root.FindById(CardId(0, "-swatch"));
    ASSERT_NE(swatch, nullptr);
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor),
              std::optional<std::uint32_t>(0xFFFFFFFFu));

    Label* chip = dynamic_cast<Label*>(root.FindById(CardId(0, "-tintchip")));
    ASSERT_NE(chip, nullptr) << "a white swatch says nothing about what it is";
    EXPECT_EQ(chip->GetText(), "Tint");
    EXPECT_EQ(chip->GetParent(), swatch) << "the chip is not on the swatch it describes";

    // It rides on an arbitrary colour, so it brings its own contrast rather than inheriting the
    // panel's: an opaque-enough plate under light text reads over white and over black alike.
    const std::optional<std::uint32_t> plate = chip->Overrides().Get(Style::BackgroundColor);
    ASSERT_TRUE(plate.has_value()) << "the chip has no plate, so it vanishes into a pale tint";
    EXPECT_GE(*plate >> 24, 0x80u) << "the plate is too sheer to carry text over any tint";
    EXPECT_TRUE(chip->Overrides().Get(Style::Color).has_value());

    const std::optional<StyleLength> size = chip->Overrides().Get(Style::FontSize);
    ASSERT_TRUE(size.has_value());
    EXPECT_GE(size->Value, 12.0f) << "the chip is under the editor's font floor";
}

// Double click, the idiom the asset browser's own grid uses (GridView activates a cell on the
// second click). A card that opened on a single click would fire while the user was still
// choosing one.
TEST(TerrainRoleMaterials, ACardOpensTheLibraryOnTheSecondClickOnly)
{
    // Instrument first: with a zero interval no pair of clicks is ever a double click, and this
    // test would pass by never activating anything.
    ASSERT_GT(Platform::GetDoubleClickInterval().count(), 0);

    const std::filesystem::path path = "Assets/Terrain/Island Materials.terrainmatlib";
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), path);
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(1, "Cobble"));

    int opened = 0;
    std::filesystem::path openedPath;
    UIElement root("div");
    ASSERT_NE(Roles::AddMaterialGrid(&root, &library, {},
                                     [&](const std::filesystem::path& p)
                                     {
                                         ++opened;
                                         openedPath = p;
                                     }),
              nullptr);

    UIElement* first = root.FindById(CardId(0, "-card"));
    UIElement* second = root.FindById(CardId(1, "-card"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    // Controls first, so a card that opened on any dispatch cannot pass the assertion below.
    SendMouse(*first, kEventMouseDown, /*button*/ 1);
    SendMouse(*first, kEventMouseDown, /*button*/ 1);
    EXPECT_EQ(opened, 0) << "a right-click opened the library";

    SendMouse(*first, kEventMouseDown);
    EXPECT_EQ(opened, 0) << "one click opened the library";

    // Two clicks on DIFFERENT cards are not a double click on either: each card times its own.
    SendMouse(*second, kEventMouseDown);
    EXPECT_EQ(opened, 0) << "clicks on two different cards counted as one double click";

    SendMouse(*second, kEventMouseDown);
    EXPECT_EQ(opened, 1) << "the second click on a card never reached the asset opener";
    EXPECT_EQ(openedPath, path) << "the card opened a path that is not its library's";

    // A third click starts a new pair rather than opening again.
    SendMouse(*second, kEventMouseDown);
    EXPECT_EQ(opened, 1);
}

// One click runs the CALLER'S add action, and that action appends through the shared path — so the
// material the terrain panel adds is the material the library panel's button adds, on the same
// slot rule. The action is the caller's because the grid must not hold the library it was built
// from: a handler outlives the build, and the asset manager's loaded map is a library's only
// strong owner.
TEST(TerrainRoleMaterials, TheAddTileRunsItsAddActionOnOneClick)
{
    const std::filesystem::path path = MakeLibraryPath("grid_add_tile");
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), path);
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));

    int added = 0;
    int refreshed = 0;
    UIElement root("div");
    // What the panel supplies, minus the re-resolve it does first — that half needs a World and an
    // asset manager, and lives in the terrain inspector.
    ASSERT_NE(Roles::AddMaterialGrid(&root, &library,
                                     [&]
                                     {
                                         ++added;
                                         (void)Roles::AddLibraryMaterial(
                                             &library, nullptr, [&refreshed] { ++refreshed; });
                                     },
                                     {}),
              nullptr);

    UIElement* tile = root.FindById("terrain-material-add");
    ASSERT_NE(tile, nullptr);

    // Controls: building the grid must not have added anything, and a non-left button is not a
    // click.
    EXPECT_EQ(added, 0);
    EXPECT_EQ(library.GetMaterials().size(), 1u);
    SendMouse(*tile, kEventMouseDown, /*button*/ 1);
    EXPECT_EQ(added, 0) << "a right-click ran the add action";

    SendMouse(*tile, kEventMouseDown);

    EXPECT_EQ(added, 1) << "the tile's click never reached the add action";
    ASSERT_EQ(library.GetMaterials().size(), 2u) << "the add action appended nothing";
    EXPECT_NE(library.FindBySlotId(1), nullptr) << "the new material did not take the free slot";
    EXPECT_EQ(refreshed, 1) << "the panel was never rebuilt around the new material";
}

// A control that cannot act consumes nothing: swallowing the press takes it from whatever else
// would have handled it and gives nothing back. Both tiles reach this state for real — a card has
// no destination when the inspector host supplies no navigate callback, which is the same
// condition AddOpenLibraryRow refuses to build a button for.
//
// BOUNDARY, stated plainly: this does NOT simulate a library ejected from under the grid. Nothing
// at this layer can — the eject runs through AssetManager against a live project, and a
// hand-dangled pointer would be undefined behaviour rather than a test. What the grid's signature
// now guarantees is that no handler holds the asset at all.
TEST(TerrainRoleMaterials, CardsAndTheAddTileSwallowNothingWhenTheyCannotAct)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));

    UIElement inert("div");
    ASSERT_NE(Roles::AddMaterialGrid(&inert, &library, {}, {}), nullptr);

    UIElement* card = inert.FindById(CardId(0, "-card"));
    UIElement* tile = inert.FindById("terrain-material-add");
    ASSERT_NE(card, nullptr);
    ASSERT_NE(tile, nullptr);

    for (UIElement* target : {card, tile})
    {
        UIEvent e{};
        e.Id = kEventMouseDown;
        e.Button = 0;
        e.Target = target;
        e.CurrentTarget = target;
        target->DispatchEvent(e);
        EXPECT_FALSE(e.Handled) << target->GetId() << " swallowed a press it does nothing with";
    }

    // Positive control: the same dispatch on a card that CAN navigate is handled, so the
    // assertions above cannot be passing because nothing ever sets the flag.
    UIElement wired("div");
    ASSERT_NE(Roles::AddMaterialGrid(&wired, &library, {}, [](const std::filesystem::path&) {}),
              nullptr);
    UIElement* live = wired.FindById(CardId(0, "-card"));
    ASSERT_NE(live, nullptr);

    UIEvent handled{};
    handled.Id = kEventMouseDown;
    handled.Button = 0;
    handled.Target = live;
    handled.CurrentTarget = live;
    live->DispatchEvent(handled);
    EXPECT_TRUE(handled.Handled) << "a card with a destination did not take the press";
}

// A tile that cannot add anything must not be drawn — the same rule the picker's create option and
// the library panel's disabled button follow.
TEST(TerrainRoleMaterials, NoAddTileWhenEverySlotIsTaken)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    for (int slot = 0; slot < 256; ++slot)
        library.EditMaterials().push_back(
            MakeEntry(static_cast<std::uint8_t>(slot), "Material " + std::to_string(slot)));

    UIElement root("div");
    UIElement* grid = Roles::AddMaterialGrid(&root, &library, {}, {});
    ASSERT_NE(grid, nullptr);

    EXPECT_EQ(grid->GetChildren().size(), 256u) << "the cards are the materials, and nothing else";
    EXPECT_EQ(root.FindById("terrain-material-add"), nullptr);
}

// An unmigrated terrain resolves no library: there are no materials to show, and it authors its
// channels through the per-layer cards instead.
TEST(TerrainRoleMaterials, NoGridWithoutALibrary)
{
    UIElement root("div");
    EXPECT_EQ(Roles::AddMaterialGrid(&root, nullptr, {}, {}), nullptr);
    EXPECT_TRUE(root.GetChildren().empty());
    EXPECT_EQ(root.FindById("terrain-material-grid"), nullptr);
}

// ---------------------------------------------------------------------------------------------
// The layer-role block
// ---------------------------------------------------------------------------------------------

// The binding is plumbing under the materials, not the panel's face, so it arrives shut — and one
// plain row per channel, in splat channel order.
TEST(TerrainRoleMaterials, TheLayerRoleBlockStartsOpenWithOneRowPerChannel)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));

    Components::Terrain terrain{};
    UIElement root("div");

    const Roles::LayerRoleBlock block = Roles::AddLayerRoleBlock(&root, terrain, &library);
    ASSERT_NE(block.Block, nullptr);
    ASSERT_EQ(block.Pickers.size(), 4u);

    EXPECT_EQ(root.FindById("terrain-layer-roles"), block.Block);
    EXPECT_EQ(block.Block->GetTitle(), "Layer Materials");
    EXPECT_TRUE(block.Block->IsExpanded())
        << "the binding is shut away where nothing tells the author it exists";
    // One heading style across the panel, whatever level a block sits at: the layer-role block
    // heads the same way the sections around it do, not in the control's default plate.
    EXPECT_TRUE(block.Block->HasClass("rp-foldout"))
        << "the block no longer takes the inspector's shared foldout treatment";
    EXPECT_TRUE(block.Block->HasClass("inspector-component-section"))
        << "the block heads in its own style instead of the panel's one heading style";

    const char* const kRoleNames[] = {"Grass", "Rock", "Dirt", "Snow"};
    UIElement* content = block.Block->GetContentContainer();
    ASSERT_NE(content, nullptr);
    ASSERT_EQ(content->GetChildren().size(), 4u) << "the block is not four rows";

    for (int role = 0; role < 4; ++role)
    {
        EXPECT_EQ(content->GetChildren()[static_cast<size_t>(role)]->GetId(),
                  RoleRowId(role, "-row"));

        Label* name = RoleRowName(root, role);
        ASSERT_NE(name, nullptr) << "channel " << role << " has no name";
        EXPECT_EQ(name->GetText(), kRoleNames[role]);
        // The inspector's label style is what puts this at the panel's label size; a name that
        // dropped it would be styled by nothing and could land under the 12px floor.
        EXPECT_TRUE(name->HasClass("inspector-label"));

        EXPECT_EQ(RoleRowPicker(root, role), block.Pickers[static_cast<size_t>(role)])
            << "channel " << role << "'s returned picker is not the one in its row";
    }
}

// The classes above only mean something if a rule answers to them, and a class with no rule behind
// it fails exactly the way the bar it replaces looked: silently. Source-level, because a stylesheet
// is an asset the panel resolves at runtime rather than anything this process links — the sibling
// guards in this target (TerrainMaterialPanelVocabularyTests, PanelDefaultTabIconTests) scan the
// tree for the same reason.
//
// What is locked is that the header stops being the control's default plate and pulls out to the
// panel edges, not the exact declarations: restyle freely, but not back into the default.
TEST(TerrainRoleMaterials, TheSectionHeaderRuleExistsInTheInspectorStylesheet)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/UI/theme/inspector.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "the inspector stylesheet did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    const size_t rule = css.find(".rp-foldout.inspector-component-section > .foldout-header {");
    ASSERT_NE(rule, std::string::npos)
        << "nothing styles the classes the block asks for, so it keeps the control's default bar";

    const size_t end = css.find('}', rule);
    ASSERT_NE(end, std::string::npos);
    const std::string body = css.substr(rule, end - rule);
    EXPECT_NE(body.find("margin"), std::string::npos)
        << "the bar no longer pulls out of the component body's indent";

    // Positive control: the shared treatment it is worn with still repaints the control's default
    // plate, so the pair above is doing work rather than restating what the control already does.
    const size_t shared = css.find(".rp-foldout > .foldout-header {");
    ASSERT_NE(shared, std::string::npos) << "the shared foldout header rule is gone";
    const size_t sharedEnd = css.find('}', shared);
    ASSERT_NE(sharedEnd, std::string::npos);
    EXPECT_NE(css.substr(shared, sharedEnd - shared).find("background-color"), std::string::npos)
        << "the shared treatment no longer paints the header, so the section bar is unstyled";

    const std::filesystem::path base =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/UI/controls/Foldout.css";
    std::ifstream baseIn(base, std::ios::binary);
    ASSERT_TRUE(baseIn.good()) << "the foldout stylesheet did not read; the control is unproven";
    std::ostringstream baseSs;
    baseSs << baseIn.rdbuf();
    EXPECT_NE(baseSs.str().find("background-color: rgba(255, 255, 255, 0.15)"), std::string::npos)
        << "the default header is no longer a plate, so this override may be dead weight";
}

// Ordinary inspector rows, and nothing else. A tinted row reads as a defect in a panel whose
// every other row is untinted, and the block has no table for a stripe to belong to.
TEST(TerrainRoleMaterials, LayerRoleRowsCarryNoRowTint)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));

    Components::Terrain terrain{};
    UIElement root("div");
    ASSERT_EQ(Roles::AddLayerRoleBlock(&root, terrain, &library).Pickers.size(), 4u);

    for (int role = 0; role < 4; ++role)
    {
        UIElement* row = root.FindById(RoleRowId(role, "-row"));
        ASSERT_NE(row, nullptr) << "channel " << role;
        EXPECT_FALSE(row->Overrides().Get(Style::BackgroundColor).has_value())
            << "channel " << role << " carries a row tint";
        EXPECT_TRUE(row->HasClass("inspector-row"))
            << "channel " << role << " is not an ordinary inspector row";
    }
}

// Both identities readable at once: the channel is named by its row, the material it shades with by
// the closed picker — and the closed picker says the material's NAME, with no slot number the
// author never typed.
TEST(TerrainRoleMaterials, AClosedPickerNamesTheMaterialAndNothingElse)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(5, "Packed Snow"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[3] = 5; // the Snow channel shades with a material called otherwise

    UIElement root("div");
    ASSERT_EQ(Roles::AddLayerRoleBlock(&root, terrain, &library).Pickers.size(), 4u);

    Label* name = RoleRowName(root, 3);
    Dropdown* picker = RoleRowPicker(root, 3);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(picker, nullptr);

    EXPECT_EQ(name->GetText(), "Snow");
    EXPECT_EQ(picker->GetSelectedValue(), "5") << "the row binds a slot its channel does not hold";
    ASSERT_NE(picker->GetHeaderLabel(), nullptr);
    EXPECT_EQ(picker->GetHeaderLabel()->GetText(), "Packed Snow");
}

// The block changed the layout, not the picker. Every row offers exactly what the picker-semantics
// tests above pin: live entries by name, retired omitted, create last.
TEST(TerrainRoleMaterials, EveryRowOffersTheSameLibraryTheSemanticsDescribe)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    library.EditMaterials().push_back(MakeEntry(0, "Meadow"));
    library.EditMaterials().push_back(MakeEntry(1, "Old Mud", /*retired*/ true));
    library.EditMaterials().push_back(MakeEntry(2, "Cobble"));

    Components::Terrain terrain{};
    UIElement root("div");
    ASSERT_EQ(Roles::AddLayerRoleBlock(&root, terrain, &library).Pickers.size(), 4u);

    const std::vector<Dropdown::Option> expected = Roles::BuildMaterialOptions(&library);
    ASSERT_EQ(expected.size(), 3u); // two live materials + create

    for (int role = 0; role < 4; ++role)
    {
        Dropdown* picker = RoleRowPicker(root, role);
        ASSERT_NE(picker, nullptr) << "channel " << role;
        const std::vector<std::string> offered = RolePickerOptionLabels(*picker);
        ASSERT_EQ(offered.size(), expected.size()) << "channel " << role;
        for (size_t option = 0; option < offered.size(); ++option)
            EXPECT_EQ(offered[option], expected[option].label)
                << "channel " << role << " option " << option;
        for (const std::string& label : offered)
            EXPECT_EQ(label.find("Old Mud"), std::string::npos)
                << "channel " << role << " offers a retired material";
    }

    // Identity bindings, so each channel shows the material sitting in its own slot. Channel 1's
    // slot holds the retired entry the picker hides, which is the -1 SelectedMaterialOption case.
    EXPECT_EQ(RoleRowPicker(root, 0)->GetSelectedValue(), "0");
    EXPECT_EQ(RoleRowPicker(root, 2)->GetSelectedValue(), "2");
}

// An unmigrated terrain has no materials to bind, and authors its channels through the per-layer
// cards instead. Four empty pickers would be four controls that cannot do anything.
TEST(TerrainRoleMaterials, NoLayerRoleBlockWithoutALibrary)
{
    Components::Terrain terrain{};
    UIElement root("div");

    const Roles::LayerRoleBlock block = Roles::AddLayerRoleBlock(&root, terrain, nullptr);
    EXPECT_EQ(block.Block, nullptr);
    EXPECT_TRUE(block.Pickers.empty());
    EXPECT_TRUE(root.GetChildren().empty());
    EXPECT_EQ(root.FindById("terrain-role-0-row"), nullptr);
}

// The terrain panel's route into the library. The panel told the user to "open the library asset"
// and afforded nothing that did it, so what this covers is that the button hands the LIBRARY'S OWN
// path to the editor's navigate-to-and-select-asset callback — selecting an asset is what shows it
// in the Inspector (EditorApplication wires AssetsPanel::SetOnSelectAssets to ShowSelectedAssets).
//
// BOUNDARY: this exercises button -> callback in process. It does not cover the AssetsPanel
// navigation itself, nor the Inspector rebuild that selection triggers.
TEST(TerrainRoleMaterials, OpenLibraryButtonHandsTheLibrarysPathToTheAssetOpener)
{
    const std::filesystem::path path = "Assets/Terrain/Island Materials.terrainmatlib";
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), path);

    int opened = 0;
    std::filesystem::path openedPath;
    UIElement root("div");

    Button* button = Roles::AddOpenLibraryRow(&root, &library,
                                              [&](const std::filesystem::path& p)
                                              {
                                                  ++opened;
                                                  openedPath = p;
                                              });
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(root.FindById("terrain-open-material-library"), button)
        << "the row is unreachable by id, so nothing can drive it";
    EXPECT_EQ(opened, 0) << "building the row already opened something";

    button->TriggerClick();

    EXPECT_EQ(opened, 1);
    EXPECT_EQ(openedPath, path) << "the button opened a path that is not this library's";
}

// A button that cannot do its job must not be drawn. Both halves are missing states the terrain
// panel really reaches: an unmigrated terrain resolves no library, and an inspector host that
// supplies no navigate callback would give a dead button.
TEST(TerrainRoleMaterials, OpenLibraryRowIsNotBuiltWithoutALibraryOrAnOpener)
{
    TerrainMaterialLibraryAsset library(GUID(kLibraryGuid), "Assets/Lib.terrainmatlib");
    auto opener = [](const std::filesystem::path&) {};

    UIElement noLibrary("div");
    EXPECT_EQ(Roles::AddOpenLibraryRow(&noLibrary, nullptr, opener), nullptr);
    EXPECT_EQ(noLibrary.FindById("terrain-open-material-library"), nullptr);

    UIElement noOpener("div");
    EXPECT_EQ(Roles::AddOpenLibraryRow(&noOpener, &library, {}), nullptr);
    EXPECT_EQ(noOpener.FindById("terrain-open-material-library"), nullptr);

    // A library with no path on disk cannot be selected in the asset browser either.
    TerrainMaterialLibraryAsset unsaved(GUID(kLibraryGuid), "");
    UIElement noPath("div");
    EXPECT_EQ(Roles::AddOpenLibraryRow(&noPath, &unsaved, opener), nullptr);
}
