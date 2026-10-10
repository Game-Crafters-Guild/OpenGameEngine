// The terrain material library inspector: the growable list, and the property the list exists to
// protect — every row addresses its material by STABLE SLOT ID, so removing a row cannot make a
// later edit land on a different material than the card the user is looking at.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "EditorContextMenu/InterceptableContextMenu.h"
#include "InspectorRegistry.h"
#include "Inspectors/TerrainMaterialLibraryInspector.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include "Assets/TerrainMaterialLibraryAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>

using namespace GameEngine;

namespace
{

// The libraries of this test process, removed at exit: a concurrent ctest process running the
// same tests writes its own.
const std::filesystem::path& LibraryDirectory()
{
    static const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    return directory.Path();
}

std::filesystem::path MakeLibraryPath(const std::string& name)
{
    const std::filesystem::path path = LibraryDirectory() / (name + ".terrainmatlib");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// Fills a library with the given slot IDs, in the given order, and writes it — so a test can
// assert that display order and identity are different things. The asset is neither copyable nor
// movable, so it is constructed by the caller.
void FillLibraryWithSlots(TerrainMaterialLibraryAsset& lib,
                          const std::vector<std::uint8_t>& slotIds)
{
    for (std::uint8_t slotId : slotIds)
    {
        TerrainMaterialEntry entry{};
        entry.SlotId = slotId;
        entry.Name = "Slot " + std::to_string(static_cast<int>(slotId));
        lib.EditMaterials().push_back(entry);
    }
    EXPECT_TRUE(lib.Save());
}

InspectorFn* GetLibraryInspector()
{
    RegisterTerrainMaterialLibraryInspector();
    return InspectorRegistry::Get().TryGetAssetInspector(AssetType::TerrainMaterialLibrary);
}

// The card's header options menu is gated on a window, and Show() hands the menu straight to the
// interceptor without dereferencing it. A sentinel is enough to open the path; nothing here
// touches a real window.
Platform::Window* SentinelWindow()
{
    return reinterpret_cast<Platform::Window*>(std::uintptr_t{1});
}

void BuildInto(UIElement& root, TerrainMaterialLibraryAsset& lib)
{
    InspectorFn* fn = GetLibraryInspector();
    ASSERT_NE(fn, nullptr);
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &lib;
    ctx.Window = SentinelWindow();
    (*fn)(ctx);
}

void BuildInto(UIElement& root, TerrainMaterialLibraryAsset& lib,
               OpenColorPickerWindowFn openPicker)
{
    InspectorFn* fn = GetLibraryInspector();
    ASSERT_NE(fn, nullptr);
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &lib;
    ctx.Window = SentinelWindow();
    ctx.OpenColorPickerWindow = std::move(openPicker);
    (*fn)(ctx);
}

// The interceptor is a process-wide static, so it must come back down however a test leaves —
// including through a failed ASSERT — or every later menu in the binary stays swallowed.
class InterceptorGuard
{
public:
    explicit InterceptorGuard(InterceptableContextMenu::Interceptor interceptor)
    {
        InterceptableContextMenu::SetInterceptor(std::move(interceptor));
    }
    InterceptorGuard(const InterceptorGuard&) = delete;
    InterceptorGuard& operator=(const InterceptorGuard&) = delete;
    ~InterceptorGuard() { InterceptableContextMenu::SetInterceptor(nullptr); }
};

// The dispatch the UI module's own control tests use (Engine/Modules/UI/Tests/DropdownTests.cpp,
// ButtonTests.cpp): hand the element an event and let its registered handlers run.
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

Button* FindButton(UIElement& root, const std::string& id)
{
    return dynamic_cast<Button*>(root.FindById(id));
}

// Asks for the removal the way the card offers it: the header's options button publishes its item
// tree through InterceptableContextMenu — the decorator every editor menu rides, and the one the
// debug server drives menus with — and the captured handler runs the command. Leaves the card's
// confirmation revealed; the caller applies or cancels it.
//
// Its ASSERT_* returns from this function, not from the test, so call it through
// ASSERT_NO_FATAL_FAILURE — otherwise a card that never opened its menu carries on into the
// apply step and reports whatever that does instead.
void AskToRemoveFromTheHeaderMenu(UIElement& root, int slotId)
{
    const std::string prefix = "terrainmatlib-slot-" + std::to_string(slotId);
    Button* options = FindButton(root, prefix + "-options");
    ASSERT_NE(options, nullptr) << prefix << ": the card carries no header options";

    std::optional<InterceptableContextMenu::Capture> captured;
    InterceptorGuard guard(
        [&captured](InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });

    options->TriggerClick();
    ASSERT_TRUE(captured.has_value()) << prefix << ": the header options opened no menu";

    const InterceptableContextMenu::CapturedItem* remove = nullptr;
    for (const InterceptableContextMenu::CapturedItem& item : captured->Items)
        if (item.Path == "Remove Material")
            remove = &item;
    ASSERT_NE(remove, nullptr) << prefix << ": the header menu offers no Remove Material";
    EXPECT_TRUE(remove->Enabled) << prefix << ": Remove Material is greyed out";

    ASSERT_TRUE(static_cast<bool>(captured->Invoke));
    captured->Invoke(remove->CommandId);
}

} // namespace

// Every row is keyed by slot ID, and a library whose display order does not match its slot order
// still finds each material's controls — the case an index-keyed inspector gets wrong.
TEST(TerrainMaterialLibraryInspector, BuildsOneCardPerMaterialKeyedBySlotId)
{
    const auto path = MakeLibraryPath("inspector_cards");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {7, 2});

    UIElement root("div");
    BuildInto(root, lib);

    EXPECT_NE(root.FindById("terrainmatlib-slot-7-name"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-7-albedo"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-7-tiling"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-7-hex"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-7-retired"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-2-name"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-slot-2-options"), nullptr);
    EXPECT_NE(root.FindById("terrainmatlib-add"), nullptr);

    // No card for a slot nobody holds — the array positions are 0 and 1 here.
    EXPECT_EQ(root.FindById("terrainmatlib-slot-0-name"), nullptr);
    EXPECT_EQ(root.FindById("terrainmatlib-slot-1-name"), nullptr);
}

// Add takes the LOWEST FREE slot ID, never the next array position, so a new material cannot
// inherit the painted ground of a material that used to sit at that index.
TEST(TerrainMaterialLibraryInspector, AddTakesTheLowestFreeSlotIdAndWritesTheFile)
{
    const auto path = MakeLibraryPath("inspector_add");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {0, 2});

    UIElement root("div");
    BuildInto(root, lib);

    Button* add = FindButton(root, "terrainmatlib-add");
    ASSERT_NE(add, nullptr);
    add->TriggerClick();

    ASSERT_EQ(lib.GetMaterials().size(), 3u);
    EXPECT_EQ(lib.GetMaterials().back().SlotId, 1u) << "the hole at slot 1 is the lowest free ID";

    // The edit is written, not only applied in memory: the renderer reads the file's parse.
    TerrainMaterialLibraryAsset reloaded(GUID::Generate(), path);
    ASSERT_TRUE(reloaded.Load());
    ASSERT_NE(reloaded.FindBySlotId(1), nullptr);
}

// Remove deletes the material the card names, addressed by slot — not the entry sitting at that
// card's array position.
TEST(TerrainMaterialLibraryInspector, RemoveDeletesBySlotIdNotByPosition)
{
    const auto path = MakeLibraryPath("inspector_remove");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {5, 3, 9});

    UIElement root("div");
    BuildInto(root, lib);

    ASSERT_NO_FATAL_FAILURE(AskToRemoveFromTheHeaderMenu(root, 3));
    Button* apply = FindButton(root, "terrainmatlib-slot-3-remove-apply");
    ASSERT_NE(apply, nullptr);
    apply->TriggerClick();

    ASSERT_EQ(lib.GetMaterials().size(), 2u);
    EXPECT_EQ(lib.FindBySlotId(3), nullptr);
    ASSERT_NE(lib.FindBySlotId(5), nullptr);
    ASSERT_NE(lib.FindBySlotId(9), nullptr);

    TerrainMaterialLibraryAsset reloaded(GUID::Generate(), path);
    ASSERT_TRUE(reloaded.Load());
    EXPECT_EQ(reloaded.FindBySlotId(3), nullptr);
    EXPECT_NE(reloaded.FindBySlotId(9), nullptr);
}

// Removing a row shifts every later row's POSITION. A rebuilt inspector must still address the
// survivors by their own IDs — this is the regression an index-keyed list would show as "editing
// one material changed a different one".
TEST(TerrainMaterialLibraryInspector, SurvivingCardsKeepTheirSlotIdsAfterARemove)
{
    const auto path = MakeLibraryPath("inspector_shift");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {5, 3, 9});

    UIElement first("div");
    BuildInto(first, lib);
    ASSERT_NO_FATAL_FAILURE(AskToRemoveFromTheHeaderMenu(first, 5));
    Button* apply = FindButton(first, "terrainmatlib-slot-5-remove-apply");
    ASSERT_NE(apply, nullptr);
    apply->TriggerClick(); // drops array position 0

    UIElement rebuilt("div");
    BuildInto(rebuilt, lib);
    EXPECT_NE(rebuilt.FindById("terrainmatlib-slot-3-name"), nullptr);
    EXPECT_NE(rebuilt.FindById("terrainmatlib-slot-9-name"), nullptr);
    EXPECT_EQ(rebuilt.FindById("terrainmatlib-slot-5-name"), nullptr);
}

// An empty library still offers the one control that gets a user out of it.
TEST(TerrainMaterialLibraryInspector, AnEmptyLibraryStillOffersAdd)
{
    const auto path = MakeLibraryPath("inspector_empty");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {});

    UIElement root("div");
    BuildInto(root, lib);

    Button* add = FindButton(root, "terrainmatlib-add");
    ASSERT_NE(add, nullptr);
    add->TriggerClick();
    ASSERT_EQ(lib.GetMaterials().size(), 1u);
    EXPECT_EQ(lib.GetMaterials().front().SlotId, 0u);
}


// The tint picker's apply path. The picker speaks sRGB BYTES and a material's tint is LINEAR, so
// this is a transfer-function boundary: getting it wrong does not fail, it just ships a swatch
// that disagrees with the ground the terrain paints.
//
// The expected value is computed here from the sRGB EOTF itself rather than copied from the code
// under test, so this is an independent oracle. Same transfer function the renderer applies --
// Engine/Modules/Rendering/Shaders/UI/text_mask_gamma.glsl:77 (SrgbToLinear) and
// Engine/Include/Components/Rendering/LightPhotometry.h:43 -- i.e. IEC 61966-2-1:
//   linear = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4)
namespace
{
float SrgbByteToLinearOracle(std::uint32_t byteValue)
{
    const float s = static_cast<float>(byteValue) / 255.0f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
} // namespace

TEST(TerrainMaterialLibraryInspector, PickerTintIsDecodedToLinearAndRoundTripsToTheSwatch)
{
    const auto path = MakeLibraryPath("inspector_tint_apply");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {4});

    // A distinct prior tint, so undo restoring it cannot be confused with the default.
    TerrainMaterialEntry* authored = FindTerrainMaterialBySlotIdMutable(lib.EditMaterials(), 4);
    ASSERT_NE(authored, nullptr);
    authored->AlbedoR = 0.25f;
    authored->AlbedoG = 0.5f;
    authored->AlbedoB = 0.75f;
    ASSERT_TRUE(lib.Save());

    // What the picker hands back for sRGB (255, 13, 13). 13 is the load-bearing channel: 255
    // decodes to 1.0 either way, so only a mid-dark channel can tell a decode from a passthrough.
    constexpr std::uint32_t kPickedArgb = 0xFFFF0D0Du;
    constexpr std::uint32_t kPickedByte = 0x0Du;

    Editor::UndoRedoService undo;
    ApplyTerrainMaterialTintFromPicker(&lib, &undo, /*requestRefresh*/ {}, /*slotId*/ 4,
                                       kPickedArgb);

    const TerrainMaterialEntry* applied = lib.FindBySlotId(4);
    ASSERT_NE(applied, nullptr);

    const float expectedDark = SrgbByteToLinearOracle(kPickedByte);
    EXPECT_FLOAT_EQ(applied->AlbedoR, 1.0f);
    EXPECT_NEAR(applied->AlbedoG, expectedDark, 1e-6f);
    EXPECT_NEAR(applied->AlbedoB, expectedDark, 1e-6f);

    // The failure this test exists for: storing the byte's gamma-encoded value instead of its
    // linear one. At byte 13 the two differ by more than 12x, which is a visibly wrong colour.
    // Oracle sanity, stated against the two REFERENCE values only so it reports on the test's own
    // discriminating power rather than firing a second time when the decode is what broke.
    const float gammaPassthrough = static_cast<float>(kPickedByte) / 255.0f;
    EXPECT_GT(gammaPassthrough, expectedDark * 10.0f)
        << "this byte no longer separates linear from gamma; pick a darker probe channel";

    // Round-trip: the swatch redrawn from the stored tint shows the colour the user picked.
    EXPECT_EQ(TerrainMaterialTintToSwatchArgb(*applied), kPickedArgb)
        << "the swatch would show a different colour from the one applied";

    // The edit is written, not only applied in memory -- extraction shades from the file's parse.
    {
        TerrainMaterialLibraryAsset reloaded(GUID::Generate(), path);
        ASSERT_TRUE(reloaded.Load());
        const TerrainMaterialEntry* persisted = reloaded.FindBySlotId(4);
        ASSERT_NE(persisted, nullptr);
        EXPECT_NEAR(persisted->AlbedoG, expectedDark, 1e-6f);
    }

    // One whole-document undo entry restores the tint the material had before the picker.
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    const TerrainMaterialEntry* restored = lib.FindBySlotId(4);
    ASSERT_NE(restored, nullptr);
    EXPECT_FLOAT_EQ(restored->AlbedoR, 0.25f);
    EXPECT_FLOAT_EQ(restored->AlbedoG, 0.5f);
    EXPECT_FLOAT_EQ(restored->AlbedoB, 0.75f);
}


// Clicking a material's swatch must open the picker AT THAT MATERIAL'S COLOUR. Nothing had ever
// observed this path: the picker is an OS window and the editor's IPC click injection does not
// reach the card subtree, so "the swatch is wired" and "the click never arrived" were
// indistinguishable from outside.
//
// BOUNDARY, stated plainly. This exercises element -> registered handler -> OpenPicker in
// process, using UIElement::DispatchEvent — the same entry the UI module's own control tests use.
// It therefore covers the swatch's mouse-down registration, its button filter, and the argument
// it opens the picker with. It does NOT cover hit-testing (which element a pointer at (x,y)
// resolves to), UIManager routing/bubbling, the native picker window, or IPC click injection —
// that last one is the separately filed tooling defect, and it stays unproven by this test.
TEST(TerrainMaterialLibraryInspector, TintSwatchMouseDownOpensThePickerAtTheMaterialsColour)
{
    const auto path = MakeLibraryPath("inspector_tint_click");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {0});

    // A distinct tint, so the ARGB the picker opens with can only have come from this material.
    TerrainMaterialEntry* authored = FindTerrainMaterialBySlotIdMutable(lib.EditMaterials(), 0);
    ASSERT_NE(authored, nullptr);
    authored->AlbedoR = 0.25f;
    authored->AlbedoG = 0.5f;
    authored->AlbedoB = 0.75f;

    int opened = 0;
    std::uint32_t openedArgb = 0;
    float openedIntensity = 0.0f;
    UIElement root("div");
    BuildInto(root, lib,
              [&](std::uint32_t argb, float intensity, ColorPickerCallbacks)
              {
                  ++opened;
                  openedArgb = argb;
                  openedIntensity = intensity;
              });

    UIElement* swatch = root.FindById("terrainmatlib-slot-0-tint");
    ASSERT_NE(swatch, nullptr) << "the swatch element is gone; the rest of this test is vacuous";

    // Control FIRST, so a stub that fires for any dispatch cannot pass the assertion below.
    // A non-left button is not a click...
    SendMouse(*swatch, kEventMouseDown, /*button*/ 1);
    EXPECT_EQ(opened, 0) << "a right-click opened the colour picker";
    // ...and neither is a mouse-down on a sibling row that carries no tint handler.
    UIElement* nameRow = root.FindById("terrainmatlib-slot-0-name");
    ASSERT_NE(nameRow, nullptr);
    SendMouse(*nameRow, kEventMouseDown, 0);
    EXPECT_EQ(opened, 0) << "an unrelated element opened the tint picker";

    // The real click.
    SendMouse(*swatch, kEventMouseDown, 0);

    EXPECT_EQ(opened, 1) << "the swatch's mouse-down never reached OpenPicker";
    EXPECT_EQ(openedArgb, TerrainMaterialTintToSwatchArgb(*lib.FindBySlotId(0)))
        << "the picker opened on a colour that is not this material's";
    EXPECT_FLOAT_EQ(openedIntensity, 1.0f);
}


// The card is headlined by the material's NAME; the slot ID it also has to show is secondary text
// beside it, not part of the title.
TEST(TerrainMaterialLibraryInspector, TheCardHeadlineIsTheNameAndTheSlotIsSecondary)
{
    const auto path = MakeLibraryPath("inspector_card_headline");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    TerrainMaterialEntry entry{};
    entry.SlotId = 3;
    entry.Name = "Wet Sand";
    lib.EditMaterials().push_back(entry);
    ASSERT_TRUE(lib.Save());

    UIElement root("div");
    BuildInto(root, lib);

    Foldout* card = dynamic_cast<Foldout*>(root.FindById("terrainmatlib-slot-3-card"));
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->GetTitle(), "Wet Sand") << "the slot ID is back in the headline";

    Label* note = dynamic_cast<Label*>(root.FindById("terrainmatlib-slot-3-slotnote"));
    ASSERT_NE(note, nullptr) << "the slot ID vanished; it is what painted terrain stores";
    EXPECT_EQ(note->GetText(), "Slot 3");
}


// Retire and Remove do different things to a terrain that shades from the material, and the cards
// used to be indistinguishable: retired state lived in a toggle a dozen rows down. A retired card
// has to read as withdrawn from the header alone.
//
// The dimming is the stylesheet's, under the compound selector `.terrain-material-card.retired`
// (UI/theme/inspector.css), so both halves of that selector are asserted: dropping either class
// leaves the card at full strength. What the sheet then declares is checked separately by
// TheRetiredBadgeHoldsTheEditorsFontFloor below; this test asserts only the marks the build emits
// and the badge's own copy.
TEST(TerrainMaterialLibraryInspector, ARetiredCardIsDimmedAndBadgedWhileALiveOneIsNeither)
{
    const auto path = MakeLibraryPath("inspector_retired_state");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {4, 6});
    TerrainMaterialEntry* retired = FindTerrainMaterialBySlotIdMutable(lib.EditMaterials(), 6);
    ASSERT_NE(retired, nullptr);
    retired->Retired = true;
    ASSERT_TRUE(lib.Save());

    UIElement root("div");
    BuildInto(root, lib);

    // Control first: the live card carries neither mark, so a build that stamped every card
    // cannot pass the assertions below.
    UIElement* live = root.FindById("terrainmatlib-slot-4-card");
    ASSERT_NE(live, nullptr);
    EXPECT_FALSE(live->HasClass("retired")) << "a live material is dimmed";
    EXPECT_EQ(root.FindById("terrainmatlib-slot-4-retiredbadge"), nullptr);

    UIElement* card = root.FindById("terrainmatlib-slot-6-card");
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->HasClass("terrain-material-card"))
        << "the retired card is drawn at full strength: it is outside the dimming selector";
    EXPECT_TRUE(card->HasClass("retired")) << "the retired card is drawn at full strength";

    Label* badge = dynamic_cast<Label*>(root.FindById("terrainmatlib-slot-6-retiredbadge"));
    ASSERT_NE(badge, nullptr) << "nothing on the card says it is retired";
    EXPECT_EQ(badge->GetText(), "RETIRED");
    EXPECT_TRUE(badge->HasClass("terrain-material-retired-badge"))
        << "the badge carries no class, so nothing gives it the pill it is read as";
}


namespace
{

// The theme sheet as the editor loads it: StageEditorAssets puts it beside the Editor binary, so
// it lives under this test's OWN configuration at <build>/bin/<Config>/Apps/Editor/. Anchored to
// the test executable rather than the working directory — a spelled-out configuration list skips
// silently for a configuration nobody named, and lets a sibling's stale sheet answer for the one
// under test.
std::filesystem::path FindStagedInspectorCss()
{
    namespace fs = std::filesystem;
    fs::path stagedConfigDir = GameEngine::TestPaths::ExecutableDirectory();
    if (stagedConfigDir.filename() == "Tests")
        stagedConfigDir = stagedConfigDir.parent_path();

    for (const char* relative :
         {"Apps/Editor/Assets/UI/theme/inspector.css",
          "Apps/Editor/Editor.app/Contents/Resources/Assets/UI/theme/inspector.css"})
    {
        const fs::path candidate = stagedConfigDir / relative;
        if (fs::exists(candidate))
            return candidate;
    }
    return {};
}

// The px font-size `selector`'s own rule block declares, or nullopt when the selector or the
// declaration is absent. Read from inside the block, so a size belonging to a neighbouring rule
// cannot answer for this one; occurrences that are only a prefix of a longer class name are
// skipped.
std::optional<float> RuleFontSizePx(const std::string& sheet, const std::string& selector)
{
    constexpr std::string_view kFontSize = "font-size:";
    for (size_t at = sheet.find(selector); at != std::string::npos;
         at = sheet.find(selector, at + 1))
    {
        const size_t after = at + selector.size();
        if (after < sheet.size() &&
            (std::isalnum(static_cast<unsigned char>(sheet[after])) != 0 || sheet[after] == '-' ||
             sheet[after] == '_'))
            continue;

        const size_t open = sheet.find('{', after);
        const size_t close = (open == std::string::npos) ? std::string::npos
                                                         : sheet.find('}', open);
        if (close == std::string::npos)
            return std::nullopt;

        const std::string block = sheet.substr(open, close - open);
        const size_t sizeAt = block.find(kFontSize);
        if (sizeAt == std::string::npos)
            return std::nullopt;
        return std::strtof(block.c_str() + sizeAt + kFontSize.size(), nullptr);
    }
    return std::nullopt;
}

} // namespace

// The editor's 12px typography floor, on the element that most tempts a smaller one. The size is
// declared in the stylesheet rather than by the build, so this reads the sheet the editor loads.
TEST(TerrainMaterialLibraryInspector, TheRetiredBadgeHoldsTheEditorsFontFloor)
{
    const std::filesystem::path css = FindStagedInspectorCss();
    // EditorTests does not depend on StageEditorAssets, so a tests-only build legitimately has no
    // staged sheet to check against.
    if (css.empty())
        GTEST_SKIP() << "staged inspector.css not found beside the Editor binary "
                        "(build the Editor target to stage it)";

    std::ifstream in(css);
    const std::string sheet((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    const std::optional<float> fontPx = RuleFontSizePx(sheet, ".terrain-material-retired-badge");
    ASSERT_TRUE(fontPx.has_value())
        << ".terrain-material-retired-badge declares no font-size in " << css.string()
        << ", so the badge takes an inherited size this guard cannot see";
    EXPECT_GE(*fontPx, 12.0f) << "the badge is under the editor's 12px font floor ("
                              << css.string() << ")";
}


// Remove asks first, and the question states what the deletion does to the terrains bound to this
// slot — the two consequences a user cannot see from the card: bound channels fall back to their
// built-in material, and the freed slot can be inherited by whatever is added next.
TEST(TerrainMaterialLibraryInspector, RemoveConfirmsFirstAndStatesTheEffectOnBoundTerrains)
{
    const auto path = MakeLibraryPath("inspector_remove_confirm");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    TerrainMaterialEntry entry{};
    entry.SlotId = 2;
    entry.Name = "Cobble";
    lib.EditMaterials().push_back(entry);
    ASSERT_TRUE(lib.Save());

    UIElement root("div");
    BuildInto(root, lib);

    UIElement* confirm = root.FindById("terrainmatlib-slot-2-remove-confirm");
    ASSERT_NE(confirm, nullptr);
    EXPECT_EQ(confirm->Overrides().Get(Style::Display), DisplayMode::None)
        << "the confirmation is showing before anything was asked";

    ASSERT_NO_FATAL_FAILURE(AskToRemoveFromTheHeaderMenu(root, 2));

    // The menu item deletes nothing on its own — the whole point of the step.
    EXPECT_EQ(lib.GetMaterials().size(), 1u) << "Remove Material deleted without asking";
    EXPECT_EQ(confirm->Overrides().Get(Style::Display), DisplayMode::Flex)
        << "the confirmation never appeared";

    // What it says. Asserted as the facts it must carry, not as a sentence: the material's name,
    // the slot, the built-in fallback, and that the slot can be taken again.
    std::string text;
    for (const std::unique_ptr<UIElement>& child : confirm->GetChildren())
        if (const Label* label = dynamic_cast<const Label*>(child.get()))
            text += label->GetText();
    EXPECT_NE(text.find("Cobble"), std::string::npos);
    EXPECT_NE(text.find("slot 2"), std::string::npos);
    EXPECT_NE(text.find("built-in"), std::string::npos)
        << "the confirmation does not say what bound terrains fall back to";
    EXPECT_NE(text.find("Retire"), std::string::npos)
        << "the confirmation does not name the alternative that keeps that ground";

    // Cancel puts the card back and keeps the material.
    Button* cancel = FindButton(root, "terrainmatlib-slot-2-remove-cancel");
    ASSERT_NE(cancel, nullptr);
    cancel->TriggerClick();
    EXPECT_EQ(confirm->Overrides().Get(Style::Display), DisplayMode::None);
    ASSERT_EQ(lib.GetMaterials().size(), 1u) << "Cancel removed the material";

    // Confirm removes it, and writes the file the renderer parses.
    ASSERT_NO_FATAL_FAILURE(AskToRemoveFromTheHeaderMenu(root, 2));
    Button* apply = FindButton(root, "terrainmatlib-slot-2-remove-apply");
    ASSERT_NE(apply, nullptr);
    apply->TriggerClick();

    EXPECT_TRUE(lib.GetMaterials().empty());
    TerrainMaterialLibraryAsset reloaded(GUID::Generate(), path);
    ASSERT_TRUE(reloaded.Load());
    EXPECT_EQ(reloaded.FindBySlotId(2), nullptr);
}


// Normal, ORM, AO and Normal Strength were once stored but unshaded, and each row carried a
// "(not yet sampled — Phase B)" note so the panel could not imply otherwise. The surface samples
// all four now — CBT_MaterialNormal and CBT_MaterialOrm run per material, mat.Ao scales the ORM red
// channel and mat.NormalStrength scales the tangent XY — so the note became the lie it existed to
// prevent, and a panel that contradicted its own ORM tooltip.
//
// Inverted rather than deleted: the note is gone, and this is what stops it coming back with the
// next field that ships ahead of its shading. A row that genuinely is not sampled should say so in
// its own words on its own row, not by reviving a blanket Phase B marker.
TEST(TerrainMaterialLibraryInspector, NoFieldIsMarkedUnsampled)
{
    const auto path = MakeLibraryPath("inspector_phaseb_notes");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {1});

    UIElement root("div");
    BuildInto(root, lib);

    for (const char* field :
         {"normal", "orm", "ao", "normalstrength", "roughness", "tiling", "albedo"})
    {
        const std::string id = std::string("terrainmatlib-slot-1-") + field + "-phaseb";
        EXPECT_EQ(root.FindById(id), nullptr)
            << field << " is marked unsampled, but the terrain surface samples it";
    }

    // The rows themselves stay — the fields are editable, only the disclaimer is gone.
    for (const char* field : {"normal", "orm", "ao", "normalstrength"})
    {
        const std::string id = std::string("terrainmatlib-slot-1-") + field;
        EXPECT_NE(root.FindById(id), nullptr) << field << " row disappeared with its note";
    }
}

// Double-clicking a row's label resets its field to the row's authored `defaultValue`
// (InspectorDragHelpers.h AddFloatRowWithDrag). That reset value is a SECOND copy of the entry's
// default, hand-written at the call site, and it is observable ONLY by performing the gesture --
// AddFloatRowWithDrag returns just the FloatField and FloatField has no GetDefaultValue, so nothing
// could see the two copies disagree. When they do, one double-click silently re-applies whatever the
// default used to be, on a field the record contract says means something else.
//
// The expectation is READ OUT OF TerrainMaterialEntry rather than spelled out here: transcribing the
// numbers would pin the inspector to a snapshot of the struct and pass by construction after the next
// flip. Every scalar row is covered, not just the one that drifted, so a row added later is caught by
// the same test rather than by the next audit.
//
// Each field is seeded to a value that is neither its own default nor any other row's, so a reset
// that does nothing cannot pass, and the assertion reads the LIBRARY (the gesture's commit path)
// rather than the widget.
namespace
{

template <typename T>
T* FindDescendantByClass(UIElement& root, const char* className)
{
    if (root.HasClass(className))
    {
        if (T* self = dynamic_cast<T*>(&root))
            return self;
    }
    for (const auto& child : root.GetChildren())
    {
        if (!child)
            continue;
        if (T* found = FindDescendantByClass<T>(*child, className))
            return found;
    }
    return nullptr;
}

// The row's draggable label, reached from the field's id: field -> .inspector-field -> .inspector-row.
Label* FindRowLabelForField(UIElement& root, const std::string& fieldId)
{
    UIElement* field = root.FindById(fieldId);
    if (!field)
        return nullptr;
    UIElement* fieldCell = field->GetParent();
    UIElement* row = fieldCell ? fieldCell->GetParent() : nullptr;
    return row ? FindDescendantByClass<Label>(*row, "inspector-label") : nullptr;
}

} // namespace

TEST(TerrainMaterialLibraryInspector, LabelDoubleClickResetsEachScalarToItsEntryStructDefault)
{
    struct ScalarRow
    {
        const char* IdSuffix;
        float TerrainMaterialEntry::*Member;
        float Seed; // deliberately not equal to any row's default
    };
    const ScalarRow kRows[] = {
        {"tiling", &TerrainMaterialEntry::Tiling, 3.5f},
        {"roughness", &TerrainMaterialEntry::Roughness, 0.3f},
        {"ao", &TerrainMaterialEntry::Ao, 0.4f},
        {"normalstrength", &TerrainMaterialEntry::NormalStrength, 2.5f},
        {"variation", &TerrainMaterialEntry::VariationStrength, 0.6f},
        {"variationhue", &TerrainMaterialEntry::VariationHue, 0.7f},
        {"variationscale", &TerrainMaterialEntry::VariationScale, 0.8f},
    };

    // The oracle: the struct's own initializers, not numbers copied into this test.
    const TerrainMaterialEntry kDefaults{};

    const auto path = MakeLibraryPath("inspector_row_defaults");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {7});
    {
        TerrainMaterialEntry* seeded =
            FindTerrainMaterialBySlotIdMutable(lib.EditMaterials(), 7);
        ASSERT_NE(seeded, nullptr);
        for (const ScalarRow& row : kRows)
        {
            ASSERT_NE(row.Seed, kDefaults.*row.Member)
                << row.IdSuffix << ": the seed equals the default, so a reset that does nothing "
                   "would pass";
            seeded->*row.Member = row.Seed;
        }
        ASSERT_TRUE(lib.Save());
    }

    UIElement root("div");
    BuildInto(root, lib);

    for (const ScalarRow& row : kRows)
    {
        const std::string id = std::string("terrainmatlib-slot-7-") + row.IdSuffix;
        Label* label = FindRowLabelForField(root, id);
        ASSERT_NE(label, nullptr) << id << ": no draggable label on this row";

        // The gesture the reset branch listens for: two mouse-downs inside the double-click window.
        SendMouse(*label, kEventMouseDown);
        SendMouse(*label, kEventMouseUp);
        SendMouse(*label, kEventMouseDown);

        const TerrainMaterialEntry* applied = lib.FindBySlotId(7);
        ASSERT_NE(applied, nullptr);
        EXPECT_FLOAT_EQ(applied->*row.Member, kDefaults.*row.Member)
            << id << ": the row's double-click reset does not agree with TerrainMaterialEntry's "
                     "own default for this field";
    }
}

// The projection is a dropdown on the card. Choosing Planar writes the file (the renderer shades
// from the file's parse), and the edit is one undo step back to Triplanar.
TEST(TerrainMaterialLibraryInspector, ProjectionDropdownWritesPlanarAndUndoRestoresTriplanar)
{
    const auto path = MakeLibraryPath("inspector_projection");
    TerrainMaterialLibraryAsset lib(GUID::Generate(), path);
    FillLibraryWithSlots(lib, {6});

    Editor::UndoRedoService undo;
    UIElement root("div");
    InspectorFn* fn = GetLibraryInspector();
    ASSERT_NE(fn, nullptr);
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &lib;
    ctx.Window = SentinelWindow();
    ctx.Undo = &undo;
    (*fn)(ctx);

    auto* projection = dynamic_cast<EnumField<TerrainMaterialProjection>*>(
        root.FindById("terrainmatlib-slot-6-projection"));
    ASSERT_NE(projection, nullptr) << "the card offers no Projection dropdown";
    EXPECT_EQ(projection->GetDropdown()->GetSelectedIndex(), 0)
        << "a new material must show Triplanar, the default every existing material renders with";

    projection->GetDropdown()->SetSelectedIndex(1);
    ASSERT_NE(lib.FindBySlotId(6), nullptr);
    EXPECT_EQ(lib.FindBySlotId(6)->Projection, TerrainMaterialProjection::Planar);
    {
        TerrainMaterialLibraryAsset reloaded(GUID::Generate(), path);
        ASSERT_TRUE(reloaded.Load());
        ASSERT_NE(reloaded.FindBySlotId(6), nullptr);
        EXPECT_EQ(reloaded.FindBySlotId(6)->Projection, TerrainMaterialProjection::Planar)
            << "the dropdown changed the material in memory only; the terrain never sees it";
    }

    ASSERT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    ASSERT_NE(lib.FindBySlotId(6), nullptr);
    EXPECT_EQ(lib.FindBySlotId(6)->Projection, TerrainMaterialProjection::Triplanar);
}
