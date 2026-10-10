#include <gtest/gtest.h>

#include "Core/WindowInputRouter.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorInputActions.h"
#include "UI/Controls/ItemSizeSlider.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/UIEvents.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace GameEngine;
using Editor::DefaultShortcutBindings;
using Editor::LoadShortcutBindings;
using Editor::SaveShortcutBindings;
using Editor::SettingsStore;
using Editor::ShortcutBinding;
using Editor::ShortcutCatalogEntry;
using Editor::GetShortcutCatalog;

// The store never touches disk in these tests: Load()/Save() are not called,
// so LoadShortcutBindings/SaveShortcutBindings operate on the in-memory JSON.
SettingsStore MakeStore()
{
    return SettingsStore(std::filesystem::path("EditorShortcutsTests_never_written.json"));
}

ShortcutCatalogEntry ActionEntry()
{
    ShortcutCatalogEntry e;
    e.groupLabel = "Test";
    e.displayName = "Action Entry";
    e.context = 0x1234;
    e.action = 0x5678;
    e.defaultKey = Input::kKeyCode_S;
    e.defaultMods = Input::kModControl;
    return e;
}

ShortcutCatalogEntry ManualEntry()
{
    ShortcutCatalogEntry e;
    e.groupLabel = "Test";
    e.displayName = "Manual Entry";
    e.defaultKey = Input::kKeyCode_D;
    e.defaultMods = 0;
    return e;
}

ShortcutCatalogEntry DualDefaultEntry()
{
    ShortcutCatalogEntry e = ActionEntry();
    e.defaultKey2 = Input::kKeyCode_S;
    e.defaultMods2 = Input::kModSuper;
    return e;
}

const ShortcutCatalogEntry* FindCatalogEntry(std::string_view group, std::string_view name)
{
    for (const ShortcutCatalogEntry& entry : GetShortcutCatalog())
    {
        if (std::string_view(entry.groupLabel) == group && std::string_view(entry.displayName) == name)
            return &entry;
    }
    return nullptr;
}

void SetUserDataRoot(const std::string& root)
{
#ifdef _WIN32
    _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.c_str());
#else
    if (root.empty())
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
    else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.c_str(), 1);
#endif
}

// The process-wide binding cache loads the editor preferences file. For one test, point the
// preferences at an empty user-data directory so lookups see the catalog defaults and not this
// machine's rebinds; afterwards put the previous root back. Saving any binding drops what is
// cached, on the way in and on the way out.
struct ScopedCatalogDefaultBindings
{
    std::string Previous;

    ScopedCatalogDefaultBindings()
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            Previous = previous;
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "EditorShortcutsTests_default_bindings";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        SetUserDataRoot(root.string());
        DropCachedBindings();
    }

    ~ScopedCatalogDefaultBindings()
    {
        SetUserDataRoot(Previous);
        DropCachedBindings();
    }

    static void DropCachedBindings()
    {
        auto store = MakeStore();
        SaveShortcutBindings(store, ManualEntry(), {});
    }
};

std::string StoreKeyOf(const SettingsStore& store)
{
    // The single key the entry serialized under.
    for (auto it = store.Json().begin(); it != store.Json().end(); ++it)
    {
        if (it.key().rfind("shortcuts.", 0) == 0)
            return it.key();
    }
    return {};
}
} // namespace

TEST(EditorShortcutsBindings, MissingKeyFallsBackToTheDefaultChord)
{
    const auto store = MakeStore();
    const auto bindings = LoadShortcutBindings(store, ActionEntry());
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_EQ(bindings[0].key, Input::kKeyCode_S);
    EXPECT_EQ(bindings[0].mods, Input::kModControl);
}

TEST(EditorShortcutsBindings, MissingKeyFallsBackToBothDefaultChords)
{
    const auto store = MakeStore();
    const auto bindings = LoadShortcutBindings(store, DualDefaultEntry());
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_EQ(bindings[0].mods, Input::kModControl);
    EXPECT_EQ(bindings[1].mods, Input::kModSuper);
    EXPECT_EQ(bindings[1].key, Input::kKeyCode_S);
}

TEST(EditorShortcutsBindings, SaveThenLoadRoundTripsACustomChordList)
{
    auto store = MakeStore();
    const auto entry = ActionEntry();
    const std::vector<ShortcutBinding> custom = {
        {Input::kKeyCode_K, Input::kModAlt},
        {Input::kKeyCode_L, Input::kModControl | Input::kModShift},
    };
    SaveShortcutBindings(store, entry, custom);
    EXPECT_EQ(LoadShortcutBindings(store, entry), custom);
}

TEST(EditorShortcutsBindings, SavingTheDefaultSetRemovesTheOverrideKey)
{
    auto store = MakeStore();
    const auto entry = DualDefaultEntry();
    SaveShortcutBindings(store, entry, {{Input::kKeyCode_K, 0}});
    ASSERT_FALSE(StoreKeyOf(store).empty());
    // Restoring exactly the two-default set counts as default: no override row.
    SaveShortcutBindings(store, entry, DefaultShortcutBindings(entry));
    EXPECT_TRUE(StoreKeyOf(store).empty());
}

TEST(EditorShortcutsBindings, SavingAnEmptyListRemovesTheOverrideKey)
{
    auto store = MakeStore();
    const auto entry = ActionEntry();
    SaveShortcutBindings(store, entry, {{Input::kKeyCode_K, 0}});
    SaveShortcutBindings(store, entry, {});
    EXPECT_TRUE(StoreKeyOf(store).empty());
}

TEST(EditorShortcutsBindings, LegacyObjectFormStillLoads)
{
    auto store = MakeStore();
    const auto entry = ActionEntry();
    // Write an override in array form, then rewrite it as the legacy single
    // object {"key":K,"mods":M} under the same store key.
    SaveShortcutBindings(store, entry, {{Input::kKeyCode_K, 0}});
    const std::string key = StoreKeyOf(store);
    ASSERT_FALSE(key.empty());
    store.Json()[key] = nlohmann::json{{"key", Input::kKeyCode_Q}, {"mods", Input::kModShift}};
    const auto bindings = LoadShortcutBindings(store, entry);
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_EQ(bindings[0].key, Input::kKeyCode_Q);
    EXPECT_EQ(bindings[0].mods, Input::kModShift);
}

TEST(EditorShortcutsBindings, MalformedEntriesAreSkippedAndFallBackToDefaults)
{
    auto store = MakeStore();
    const auto entry = ActionEntry();
    SaveShortcutBindings(store, entry, {{Input::kKeyCode_K, 0}});
    const std::string key = StoreKeyOf(store);
    ASSERT_FALSE(key.empty());
    // No parseable element: string, object without "key", non-integer key.
    store.Json()[key] = nlohmann::json::array(
        {"Ctrl+S", nlohmann::json{{"mods", 2}}, nlohmann::json{{"key", "S"}}});
    const auto bindings = LoadShortcutBindings(store, entry);
    EXPECT_EQ(bindings, DefaultShortcutBindings(entry));
}

TEST(EditorShortcutsBindings, ManualEntriesKeyByGroupAndName)
{
    auto store = MakeStore();
    SaveShortcutBindings(store, ManualEntry(), {{Input::kKeyCode_K, 0}});
    const std::string key = StoreKeyOf(store);
    EXPECT_EQ(key, "shortcuts.manual.Test.Manual Entry");
}

TEST(EditorShortcutsFormat, FormatsModifierOrderAndSentinels)
{
    EXPECT_EQ(Editor::FormatShortcutBinding(0, Input::kModControl), "Unbound");
    EXPECT_EQ(Editor::FormatShortcutBinding(Input::kKeyCode_S,
                                            Input::kModControl | Input::kModShift | Input::kModAlt),
              "Ctrl+Alt+Shift+S");
    const std::string scroll =
        Editor::FormatShortcutBinding(Editor::kShortcutKeyMouseScroll, Input::kModControl);
    EXPECT_EQ(scroll, "Ctrl+Scroll");
}

TEST(EditorShortcutsFormat, CountsModifiersAndClassifiesModifierKeys)
{
    EXPECT_EQ(Editor::CountShortcutModifiers(0), 0);
    EXPECT_EQ(Editor::CountShortcutModifiers(Input::kModControl | Input::kModShift), 2);
    EXPECT_EQ(Editor::CountShortcutModifiers(Input::kModControl | Input::kModShift |
                                             Input::kModAlt | Input::kModSuper),
              4);
    EXPECT_TRUE(Editor::IsModifierKey(Input::kKeyCode_LeftShift));
    EXPECT_TRUE(Editor::IsModifierKey(Input::kKeyCode_RightSuper));
    EXPECT_FALSE(Editor::IsModifierKey(Input::kKeyCode_S));
}

TEST(EditorShortcutsCapture, SuppressionFollowsTheArmedCountAndConsumedLatch)
{
    ASSERT_FALSE(Editor::ShouldSuppressEditorShortcutActions());

    Editor::SetShortcutCaptureActive(true);
    EXPECT_TRUE(Editor::ShouldSuppressEditorShortcutActions());
    Editor::SetShortcutCaptureActive(true);
    Editor::SetShortcutCaptureActive(false);
    EXPECT_TRUE(Editor::ShouldSuppressEditorShortcutActions()) << "count is 2-1=1, still armed";
    Editor::SetShortcutCaptureActive(false);
    EXPECT_FALSE(Editor::ShouldSuppressEditorShortcutActions());

    // Extra disarms must not go negative and re-suppress on the next arm pair.
    Editor::SetShortcutCaptureActive(false);
    Editor::SetShortcutCaptureActive(true);
    EXPECT_TRUE(Editor::ShouldSuppressEditorShortcutActions());
    Editor::SetShortcutCaptureActive(false);
    EXPECT_FALSE(Editor::ShouldSuppressEditorShortcutActions());

    // The consumed-key latch suppresses exactly one frame.
    Editor::NoteShortcutCaptureConsumedKey();
    EXPECT_FALSE(Editor::AllowShortcutActionsThisFrame()) << "latched frame is suppressed";
    EXPECT_TRUE(Editor::AllowShortcutActionsThisFrame()) << "latch clears after one sample";
}


// The catalog's one platform-conditional default. Finder renames on Return, so
// macOS binds Return alongside F2; every other platform leaves Return to the
// Assets panel's open behaviour, as Explorer and the Linux file managers do.
// F2 is the binding both platforms share.
TEST(EditorShortcutsBindings, AssetsRenameBindsReturnOnMacOsOnly)
{
    const ShortcutCatalogEntry* rename = nullptr;
    for (const ShortcutCatalogEntry& entry : GetShortcutCatalog())
    {
        if (std::string_view(entry.groupLabel) == "Assets" &&
            std::string_view(entry.displayName) == "Rename")
        {
            rename = &entry;
            break;
        }
    }
    ASSERT_NE(rename, nullptr);

    const std::vector<ShortcutBinding> defaults = DefaultShortcutBindings(*rename);
    ASSERT_FALSE(defaults.empty());
    EXPECT_EQ(defaults[0].key, Input::kKeyCode_F2);
    EXPECT_EQ(defaults[0].mods, 0);

#ifdef __APPLE__
    ASSERT_EQ(defaults.size(), 2u);
    EXPECT_EQ(defaults[1].key, Input::kKeyCode_Enter);
    EXPECT_EQ(defaults[1].mods, 0) << "an unmodified Return, so it reads as the Finder gesture";
#else
    EXPECT_EQ(defaults.size(), 1u) << "Return keeps its open behaviour off macOS";
#endif
}

TEST(EditorShortcutsBindings, StoredBareWheelCannotReplaceItemResizeDefaults)
{
    auto store = MakeStore();
    for (const auto& entry : GetShortcutCatalog())
    {
        if (std::string_view(entry.groupLabel) != "View Navigation" ||
            std::string_view(entry.displayName) != "Resize Items")
            continue;
        SaveShortcutBindings(store, entry, {{Editor::kShortcutKeyMouseScroll, 0}});
        EXPECT_EQ(LoadShortcutBindings(store, entry), DefaultShortcutBindings(entry));
        for (const auto& binding : LoadShortcutBindings(store, entry))
            EXPECT_NE(binding.mods & Input::kModShortcutMask, 0);
        return;
    }
    FAIL() << "Resize Items shortcut missing from catalog";
}

TEST(EditorShortcutsBindings, OrdinaryScrollActionsStillAllowBareWheel)
{
    auto store = MakeStore();
    auto entry = ManualEntry();
    SaveShortcutBindings(store, entry, {{Editor::kShortcutKeyMouseScroll, 0}});
    const auto loaded = LoadShortcutBindings(store, entry);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].key, Editor::kShortcutKeyMouseScroll);
    EXPECT_EQ(loaded[0].mods, 0);
}

// Resize Items and Asset Preview's Cycle Animation both default to Ctrl+Scroll, but they act on
// different panels, so re-recording either default must not raise the conflict prompt whose
// "reassign" strips the other action's binding.
TEST(EditorShortcutsConflicts, WheelBindingsInDifferentPanelsDoNotConflict)
{
    ScopedCatalogDefaultBindings defaults;
    const ShortcutCatalogEntry* resize = FindCatalogEntry("View Navigation", "Resize Items");
    const ShortcutCatalogEntry* cycle = FindCatalogEntry("Asset Preview", "Cycle Animation");
    ASSERT_NE(resize, nullptr);
    ASSERT_NE(cycle, nullptr);

    for (const ShortcutBinding& binding : DefaultShortcutBindings(*resize))
        EXPECT_EQ(Editor::FindShortcutConflict(*resize, binding.key, binding.mods), nullptr)
            << Editor::FormatShortcutBinding(binding.key, binding.mods);
    for (const ShortcutBinding& binding : DefaultShortcutBindings(*cycle))
        EXPECT_EQ(Editor::FindShortcutConflict(*cycle, binding.key, binding.mods), nullptr)
            << Editor::FormatShortcutBinding(binding.key, binding.mods);
}

// The positive control for the test above: inside one panel a wheel binding still collides, so
// Cycle Animation cannot take the bare wheel that Dolly In/Out already owns.
TEST(EditorShortcutsConflicts, WheelBindingsInOnePanelStillConflict)
{
    ScopedCatalogDefaultBindings defaults;
    const ShortcutCatalogEntry* cycle = FindCatalogEntry("Asset Preview", "Cycle Animation");
    const ShortcutCatalogEntry* dolly = FindCatalogEntry("Asset Preview", "Dolly In/Out");
    ASSERT_NE(cycle, nullptr);
    ASSERT_NE(dolly, nullptr);
    EXPECT_EQ(Editor::FindShortcutConflict(*cycle, Editor::kShortcutKeyMouseScroll, 0), dolly);
}

// Keys stay global: only the wheel is scoped to a panel.
TEST(EditorShortcutsConflicts, KeyBindingsStillConflictAcrossGroups)
{
    ScopedCatalogDefaultBindings defaults;
    const ShortcutCatalogEntry* resize = FindCatalogEntry("View Navigation", "Resize Items");
    const ShortcutCatalogEntry* saveScene = FindCatalogEntry("Global", "Save Scene");
    ASSERT_NE(resize, nullptr);
    ASSERT_NE(saveScene, nullptr);
    const ShortcutBinding saveDefault = DefaultShortcutBindings(*saveScene).front();
    EXPECT_EQ(Editor::FindShortcutConflict(*resize, saveDefault.key, saveDefault.mods), saveScene);
}

// The size slider's tooltip names the Resize Items chord the gesture really uses, and follows a
// rebind made in Keyboard Shortcuts. The Settings page saves the bindings before it writes the
// preferences file, so the tooltip must take them from the save, not from disk.
TEST(ItemSizeSliderTooltip, NamesTheLiveBindingAndFollowsARebind)
{
    ScopedCatalogDefaultBindings defaults;
    const ShortcutCatalogEntry& resizeItems = Editor::ItemResizeGestureCatalogEntry();
    ASSERT_STREQ(resizeItems.displayName, "Resize Items");
    const std::string defaultChord = Editor::FormatShortcutBinding(resizeItems.defaultKey, resizeItems.defaultMods);
    const std::string reboundChord = Editor::FormatShortcutBinding(Editor::kShortcutKeyMouseScroll, Input::kModAlt);
    ASSERT_NE(defaultChord, reboundChord);

    EditorUI::ItemSizeSlider slider;
    EXPECT_NE(slider.GetTooltip().find(defaultChord), std::string::npos) << slider.GetTooltip();

    auto store = MakeStore();
    SaveShortcutBindings(store, ManualEntry(), {{Input::kKeyCode_E, 0}});
    EXPECT_NE(slider.GetTooltip().find(defaultChord), std::string::npos)
        << "another entry's save changed the tooltip: " << slider.GetTooltip();

    SaveShortcutBindings(store, resizeItems, {{Editor::kShortcutKeyMouseScroll, Input::kModAlt}});
    EXPECT_NE(slider.GetTooltip().find(reboundChord), std::string::npos) << slider.GetTooltip();
    EXPECT_EQ(slider.GetTooltip().find(defaultChord), std::string::npos) << slider.GetTooltip();

    SaveShortcutBindings(store, resizeItems, {});
    EXPECT_NE(slider.GetTooltip().find(defaultChord), std::string::npos)
        << "clearing the rebind did not bring back the default chord: " << slider.GetTooltip();
}

namespace
{
// One wheel step up over the slider, as the UI manager delivers it.
void WheelOver(EditorUI::ItemSizeSlider& slider)
{
    UIEvent e{};
    e.Id = kEventScroll;
    e.ScrollY = -30.0f;
    slider.DispatchEvent(e);
}
} // namespace

// The wheel still reaches a disabled element, so the size slider refuses the resize gesture
// itself while it, or a container around it, is disabled.
TEST(ItemSizeSliderGesture, ADisabledSliderOrContainerIgnoresTheResizeGesture)
{
    struct RestoreMatcher { ~RestoreMatcher() { UI::SetItemResizeGestureMatcher(nullptr); } } restore;
    UI::SetItemResizeGestureMatcher([](int) { return true; });
    UIElement container;
    auto ownedSlider = std::make_unique<EditorUI::ItemSizeSlider>();
    EditorUI::ItemSizeSlider& slider = *ownedSlider;
    container.AddChild(std::move(ownedSlider));
    int resizes = 0;
    slider.SetOnResizeGesture([&resizes](float) { ++resizes; });

    WheelOver(slider);
    EXPECT_EQ(resizes, 1) << "the enabled slider ignored the gesture";
    slider.SetEnabled(false);
    WheelOver(slider);
    EXPECT_EQ(resizes, 1) << "a disabled slider resized the items";
    slider.SetEnabled(true);
    container.SetEnabled(false);
    WheelOver(slider);
    EXPECT_EQ(resizes, 1) << "a slider inside a disabled container resized the items";
}

// ---------------------------------------------------------------------------
// Which editor shortcuts a game in Play cannot take. The editor's registration
// (RegisterEditorInputActions) flags its modifier chords hostReserved, and the
// window router gives those to the editor ahead of the game; a bare key stays
// the game's first.
// ---------------------------------------------------------------------------
namespace
{
struct EditorAndGame
{
    Input::InputSystem Editor;
    Input::InputSystem Game{Input::SinkRole::Gameplay};
    WindowInputRouterConfig Config;

    EditorAndGame()
    {
        EditorInput::RegisterEditorInputActions(Editor);
        Config.getInput = [this] { return &Editor; };
        Config.getPlaySurface = [this]
        {
            WindowInputRouterConfig::PlaySurface surface;
            surface.gameplaySink = &Game;
            return surface;
        };
    }

    // One keystroke, as the game polling this key code on its frame sees it.
    void Tap(int key, int mods)
    {
        Editor.ResetState();
        Game.ResetState();
        Game.IsKeyDown(key);
        WindowInputRouter::RouteKey(Config, key, Input::kKeyActionPress, mods);
        WindowInputRouter::RouteKey(Config, key, Input::kKeyActionRelease, mods);
        Editor.Update(0.016f);
        Game.Update(0.016f);
    }
};

const ShortcutCatalogEntry& CatalogEntry(std::string_view displayName)
{
    for (const auto& entry : GetShortcutCatalog())
        if (entry.groupLabel == std::string_view("Global") && entry.displayName == displayName)
            return entry;
    ADD_FAILURE() << "no Global catalog entry named " << displayName;
    return GetShortcutCatalog().front();
}
} // namespace

TEST(EditorReservedShortcuts, EditorChordsReachTheEditorAndBareKeysReachTheGame)
{
    ScopedCatalogDefaultBindings defaults;
    EditorAndGame input;
    struct Chord
    {
        Input::ActionId Action;
        int Key;
        int Mods;
    };
    const Chord reserved[] = {
        {EditorInput::kEditorSaveScene, Input::kKeyCode_S, Input::kModControl},
        {EditorInput::kEditorSaveSceneAs, Input::kKeyCode_S, Input::kModControl | Input::kModShift},
        {EditorInput::kEditorTogglePlayMode, Input::kKeyCode_P, Input::kModControl},
        {EditorInput::kEditorToggleWindowFullscreen, Input::kKeyCode_F, Input::kModControl},
        {EditorInput::kEditorFrameAll, Input::kKeyCode_F, Input::kModAlt},
        {EditorInput::kEditorUniversalSearch, Input::kKeyCode_K, EditorInput::kUniversalSearchDefaultMods},
    };
    for (const Chord& chord : reserved)
    {
        SCOPED_TRACE(testing::Message() << "key=" << chord.Key << " mods=" << chord.Mods);
        input.Tap(chord.Key, chord.Mods);
        EXPECT_TRUE(input.Editor.WasActionTriggered(chord.Action));
        EXPECT_FALSE(input.Game.WasKeyPressed(chord.Key)) << "the game took an editor chord";
    }
    for (const int key : {Input::kKeyCode_G, Input::kKeyCode_F, Input::kKeyCode_Escape})
    {
        SCOPED_TRACE(testing::Message() << "bare key=" << key);
        input.Tap(key, 0);
        EXPECT_TRUE(input.Game.WasKeyPressed(key));
        EXPECT_FALSE(input.Editor.WasKeyPressed(key));
    }
    input.Tap(Input::kKeyCode_A, Input::kModControl);
    EXPECT_TRUE(input.Game.WasKeyPressed(Input::kKeyCode_A)) << "an unreserved chord is the game's";
}

TEST(EditorReservedShortcuts, RebindingOrDisablingAShortcutMovesItsKeyToTheGame)
{
    ScopedCatalogDefaultBindings defaults;
    EditorAndGame input;
    // The preferences' rebind path, which clears and rebinds the action's keys.
    Editor::ApplyShortcutBindings(input.Editor, CatalogEntry("Toggle Play Mode"), {{Input::kKeyCode_F9, 0}});
    input.Tap(Input::kKeyCode_P, Input::kModControl);
    EXPECT_TRUE(input.Game.WasKeyPressed(Input::kKeyCode_P)) << "the old chord stayed reserved";
    input.Tap(Input::kKeyCode_F9, 0);
    EXPECT_TRUE(input.Editor.WasActionTriggered(EditorInput::kEditorTogglePlayMode));
    EXPECT_FALSE(input.Game.WasKeyPressed(Input::kKeyCode_F9));

    input.Editor.SetContextEnabled(EditorInput::kEditorGlobalContext, false);
    input.Tap(Input::kKeyCode_F9, 0);
    EXPECT_TRUE(input.Game.WasKeyPressed(Input::kKeyCode_F9));
    input.Editor.SetContextEnabled(EditorInput::kEditorGlobalContext, true);
    Editor::ApplyShortcutBindings(input.Editor, CatalogEntry("Toggle Play Mode"), {});
    input.Tap(Input::kKeyCode_F9, 0);
    EXPECT_TRUE(input.Game.WasKeyPressed(Input::kKeyCode_F9)) << "an unbound shortcut reserved nothing";
}
