// GameUIHost keyboard pins: the host answers keys and characters for a play
// surface, and its answer is what the input chain uses to stop a keystroke
// before gameplay. The sibling GameUIHostTests covers the pointer half.
//
// Headless: these drive OnKey/OnChar directly with no render tick. The rows that
// turn on where the cursor is, or on a document mounting and unmounting, build
// the layout or the world they need and still never render.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Engine/GameUI/GameUIHost.h"
#include "Input/InputSystem.h" // kKeyAction* / kMod* live here, not in KeyCodes.h
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

// ECS + AssetManager for the row that mounts and unmounts a real UIDocument, so
// the focus-lifetime rule is pinned on the production reconcile path. JobSystem
// types must precede the ECS Query/World templates (Query.h uses
// JobSystem::TaskHandle), mirroring GameUIHost.cpp's own include ordering.
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECS/ECS.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Components/UI/UIDocument.h"
#include "UI/Registration/ElementRegistration.h"

using namespace GameEngine;

namespace
{
// The variable is read once per UIManager, at construction, so a test that
// needs it set must set it before the host is built and restore it after.
class ScopedEnvVar
{
  public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* previous = std::getenv(name))
        {
            m_HadPrevious = true;
            m_Previous = previous;
        }
        Apply(value);
    }
    ~ScopedEnvVar() { Apply(m_HadPrevious ? m_Previous.c_str() : nullptr); }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    void Apply(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(m_Name, value ? value : "");
#else
        if (value)
            setenv(m_Name, value, 1);
        else
            unsetenv(m_Name);
#endif
    }
    const char* m_Name;
    bool m_HadPrevious = false;
    std::string m_Previous;
};

// A HUD with one text field mounted the way a bound UIDocument subtree mounts:
// a direct child of the host's root. Returns the field, unfocused.
TextField* MountHudField(GameUIHost& host)
{
    UIManager* ui = host.GetUIManager();
    if (!ui)
        return nullptr;
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto field = std::make_unique<TextField>();
    field->SetId("chat");
    TextField* raw = field.get();
    root->AddChild(std::move(field));
    ui->SetRoot(std::move(root));
    return raw;
}

// One button, laid out at a known rect with the cursor parked over it and hover
// resolved. Layout comes from a stylesheet because hit-testing needs a real rect,
// and each caller needs its own file so two managers in one test do not share one.
Button* MountHoveredButton(UIManager& ui, const char* cssName)
{
    auto root = std::make_unique<UIElement>();
    root->SetId("hud-root");
    auto btnOwned = std::make_unique<Button>();
    Button* btn = btnOwned.get();
    btn->SetId("damage-btn");
    root->AddChild(std::move(btnOwned));
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / cssName;
    {
        std::ofstream out(css);
        out << R"(
#hud-root { display: flex; width: 200px; height: 40px; }
#damage-btn { width: 100px; height: 20px; }
)";
    }
    if (!ui.AttachStyleFromFile(css.string()))
        return nullptr;

    ui.Update(0.0f, /*interactive=*/false);
    ui.OnMouseMove(btn->GetLayoutX() + 5.0f, btn->GetLayoutY() + 5.0f);
    ui.Update(0.0f, /*interactive=*/true);
    return ui.GetHoveredElement() == btn ? btn : nullptr;
}

// A HUD document the way a game ships one: a .uxml with a chat field, registered
// and pre-warmed so the host's asset poll resolves on the first sync.
GUID RegisterHudLayout(AssetManager& assets, const std::filesystem::path& dir)
{
    const auto path = dir / "hud_chat.uxml";
    {
        std::ofstream f(path);
        f << "<UIElement id=\"docroot\"><TextField id=\"chat\" /></UIElement>\n";
    }
    auto& registry = assets.GetRegistry();
    registry.RegisterAsset(path);
    const GUID guid = registry.GetAssetGUID(path);
    assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    return guid;
}

} // namespace

// The slice's reason for existing: a HUD chat box takes the character, and says
// so, which is what stops the same keystroke from also walking the character.
TEST(GameUIHostKeyboardTests, AFocusedHudFieldTakesTypingAndSaysSo)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);
    host.GetUIManager()->SetFocusById("chat");

    EXPECT_TRUE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "a character-producing key is the focused field's";
    EXPECT_TRUE(host.OnChar('w')) << "and so is the character it produces";
}

// Keys follow focus, never the cursor. A chrome manager offers a key with nothing
// focused to the element the pointer rests on — a Button acts on Space — so on a
// game surface a HUD button the player's cursor happens to sit over would eat the
// jump. The host declines it instead, and the game keeps Space.
TEST(GameUIHostKeyboardTests, AHoveredHudButtonWithNothingFocusedDeclinesTheKey)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // The control: the same tree on a plain manager does fire, so the decline
    // below is the host's rule and not an inert button or an unresolved hover.
    UIManager chrome(dev.get());
    Button* chromeButton = MountHoveredButton(chrome, "gameui_host_hovered_chrome.css");
    ASSERT_NE(chromeButton, nullptr) << "control: hover did not resolve onto the button";
    int chromeClicks = 0;
    chromeButton->RegisterEventHandler(kEventButtonClick, [&chromeClicks](UIEvent&) { ++chromeClicks; });
    ASSERT_TRUE(chrome.OnKey(Input::kKeyCode_Space, Input::kKeyActionPress, 0));
    ASSERT_EQ(chromeClicks, 1) << "control: a hovered unfocused button fires on a chrome manager";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    Button* hudButton = MountHoveredButton(*host.GetUIManager(), "gameui_host_hovered_hud.css");
    ASSERT_NE(hudButton, nullptr) << "the cursor must rest on the HUD button";
    int hudClicks = 0;
    hudButton->RegisterEventHandler(kEventButtonClick, [&hudClicks](UIEvent&) { ++hudClicks; });

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_Space, Input::kKeyActionPress, 0));
    EXPECT_EQ(hudClicks, 0) << "hover is not focus on a game surface";
}

// The other half of the same rule: with nothing focused the HUD is transparent,
// which is what lets W keep walking the character while the HUD is on screen.
TEST(GameUIHostKeyboardTests, AnUnfocusedHudDeclinesTheSameKeystroke)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0));
    EXPECT_FALSE(host.OnChar('w'));
}

TEST(GameUIHostKeyboardTests, AHostWithNothingMountedDeclinesEverything)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0));
    EXPECT_FALSE(host.OnChar('w'));
}

// A release is never anyone's to consume: the game that saw the press must see
// the release that ends it, or the key is stuck down for the rest of the session.
TEST(GameUIHostKeyboardTests, AReleaseIsNeverTheHudsEvenWithAFieldFocused)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);
    host.GetUIManager()->SetFocusById("chat");

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionRelease, 0));
}

// A game binding a bare modifier (Ctrl to crouch) must still receive it while a
// HUD field has focus: a modifier is not text, so no field acts on it.
TEST(GameUIHostKeyboardTests, ABareModifierIsNeverTheHuds)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);
    host.GetUIManager()->SetFocusById("chat");

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_LeftControl, Input::kKeyActionPress, Input::kModControl));
}

// C7: the UI module's F4-F12 diagnostic toggles are opted into per developer by
// the environment, but they are the editor's own chrome keys. A game surface
// declines them however the environment is set, or a developer debugging UI
// silently takes the playing game's function keys.
TEST(GameUIHostKeyboardTests, TheUiDiagnosticFunctionKeysAreNeverAGameSurfaces)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    ScopedEnvVar on("GE_UI_DEBUG_KEYS", "1");

    // A plain manager, built under the same environment, is the control: it does
    // take them, so a false below is the host's decision and not the variable
    // failing to reach the manager.
    UIManager chrome(dev.get());
    chrome.SetRoot(std::make_unique<UIElement>());
    ASSERT_TRUE(chrome.OnKey(Input::kKeyCode_F4, Input::kKeyActionPress, 0))
        << "control: the environment does reach a chrome manager";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_F4, Input::kKeyActionPress, 0));
    EXPECT_FALSE(host.OnKey(Input::kKeyCode_F12, Input::kKeyActionPress, 0));
    EXPECT_FALSE(host.GetUIManager()->IsTextDebugOverlayEnabled())
        << "F4 must not have toggled the UI module's overlay from a game surface";
}

// Escape is the way out of a HUD text box. The control reverts its edit and keeps
// focus — correct for a chrome field, which the user can click away from — so on a
// game surface the host ends the focus itself; otherwise the field goes on
// claiming every movement key with no way back to the game but clicking the world.
TEST(GameUIHostKeyboardTests, EscapeHandsTheKeyboardBackToTheGame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // The control: chrome semantics are unchanged. The same field on a plain
    // manager reverts on Escape and keeps focus, so the difference below is the
    // host's rule for game surfaces alone.
    UIManager chrome(dev.get());
    auto chromeRoot = std::make_unique<UIElement>();
    auto chromeField = std::make_unique<TextField>();
    chromeField->SetId("chat");
    chromeRoot->AddChild(std::move(chromeField));
    chrome.SetRoot(std::move(chromeRoot));
    chrome.SetFocusById("chat");
    ASSERT_TRUE(chrome.OnChar('h')) << "control: the focused field takes the character";
    ASSERT_TRUE(chrome.OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0))
        << "control: Escape cancels the edit session";
    ASSERT_EQ(chrome.GetFocusedElementId(), "chat") << "control: a chrome field keeps its focus";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);
    UIManager* ui = host.GetUIManager();
    ui->SetFocusById("chat");
    ASSERT_TRUE(host.OnChar('h')) << "precondition: the HUD field is taking typing";

    EXPECT_TRUE(host.OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0))
        << "the field acted on Escape, so the game does not also see it";
    EXPECT_TRUE(ui->GetFocusedElementId().empty()) << "Escape ends the game surface's focus";
    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "and the movement key is the game's again";
}

// An Escape no HUD control acted on is nobody's: it must reach the game, which is
// what lets Escape open a pause menu while a HUD is on screen.
TEST(GameUIHostKeyboardTests, AnUnclaimedEscapeStillTravelsToTheGame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);

    EXPECT_FALSE(host.OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0));
}

// Play stop unmounts the HUD's documents and the next session remounts them with
// the ids they had. Focus is an id, so a chat box focused when play stopped would
// be focused again before the player has touched anything, and the new session's
// movement keys would go to a field nobody clicked.
TEST(GameUIHostKeyboardTests, FocusDoesNotSurviveTheDocumentThatHeldIt)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto dir = std::filesystem::temp_directory_path() / "gameui_host_focus_lifetime";
    std::filesystem::create_directories(dir);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(dir, &pool));
    const GUID layout = RegisterHudLayout(assets, dir);
    ASSERT_FALSE(layout.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(layout);
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    ASSERT_TRUE(host.SyncDocuments(&world));
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui->GetRootElement()->FindById("chat"), nullptr) << "the HUD field did not mount";
    ui->SetFocusById("chat");
    ASSERT_TRUE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "precondition: the focused field takes the movement key";

    // Play stop drops every bound subtree; the next session remounts the same
    // document, with a new TextField carrying the same id.
    host.ResetBoundDocuments();
    ASSERT_TRUE(host.SyncDocuments(&world));
    ASSERT_NE(ui->GetRootElement()->FindById("chat"), nullptr) << "the HUD field did not remount";

    EXPECT_TRUE(ui->GetFocusedElementId().empty()) << "focus outlived the element it named";
    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "the new session's first movement key belongs to the game";
}

// The other way a HUD document ends: a script destroys the entity carrying it —
// a chat panel or dialog closed by destroying it mid-play. The sweep that notices
// the entity is gone leaves the same tree behind as a play stop does, so the focus
// its field held must die there too, or the next dialog to mount with the same id
// is focused before anything is clicked.
TEST(GameUIHostKeyboardTests, FocusDoesNotSurviveTheEntityThatCarriedTheDocument)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto dir = std::filesystem::temp_directory_path() / "gameui_host_focus_entity_destroyed";
    std::filesystem::create_directories(dir);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(dir, &pool));
    const GUID layout = RegisterHudLayout(assets, dir);
    ASSERT_FALSE(layout.IsNull());

    GameUIHost host(dev.get(), &assets, &pool);
    ECS::World world(&pool);
    Components::UIDocument doc;
    doc.Layout.Set(layout);
    ECS::Entity document = world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();

    ASSERT_TRUE(host.SyncDocuments(&world));
    UIManager* ui = host.GetUIManager();
    ASSERT_NE(ui->GetRootElement()->FindById("chat"), nullptr) << "the HUD field did not mount";
    ui->SetFocusById("chat");
    ASSERT_TRUE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "precondition: the focused field takes the movement key";

    document.Destroy();
    world.ProcessCommands();
    ASSERT_FALSE(host.SyncDocuments(&world)) << "the sweep did not drop the destroyed entity's subtree";
    ASSERT_EQ(ui->GetRootElement()->FindById("chat"), nullptr) << "the HUD field is still in the tree";
    EXPECT_TRUE(ui->GetFocusedElementId().empty()) << "focus outlived the entity that carried it";

    // The symptom the rule exists for: the dialog re-opens as a new entity carrying
    // the same layout, so its field mounts with the same id a stale focus would name.
    world.Create<Components::UIDocument>(doc);
    world.ProcessCommands();
    ASSERT_TRUE(host.SyncDocuments(&world));
    ASSERT_NE(ui->GetRootElement()->FindById("chat"), nullptr) << "the replacement HUD field did not mount";

    EXPECT_TRUE(ui->GetFocusedElementId().empty()) << "the replacement field is focused before any click";
    EXPECT_FALSE(host.OnKey(Input::kKeyCode_W, Input::kKeyActionPress, 0))
        << "the movement key belongs to the game again";
}

// The keyboard counterpart of CancelPointer. A surface that stops receiving keys
// never sees the release of a modifier held across the gap, so the state it kept
// would turn the next plain key into a chord.
TEST(GameUIHostKeyboardTests, ResetKeyboardStateDropsAModifierHeldAcrossTheGap)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    GameUIHost host(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    ASSERT_NE(MountHudField(host), nullptr);

    host.OnKey(Input::kKeyCode_LeftControl, Input::kKeyActionPress, Input::kModControl);
    ASSERT_NE(host.GetUIManager()->GetModifierKeys() & Input::kModControl, 0)
        << "precondition: the press is held";

    host.ResetKeyboardState();
    EXPECT_EQ(host.GetUIManager()->GetModifierKeys() & Input::kModControl, 0);
}
