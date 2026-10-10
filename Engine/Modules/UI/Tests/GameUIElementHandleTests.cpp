// Instance-id element addressing — the resolution contract the scripting ABI's
// GE_GameUI_FindElement / GE_UIElement_* exports are built on.
//
// The ABI hands managed code an element's instance id and re-resolves it on every call
// through the published gameplay host's UIManager: an O(1) index probe plus a
// root-reachability filter. Instance ids come from a process-wide monotonic counter and
// are never reused, which is what makes a stale handle fail closed forever instead of
// aliasing whatever took the element's place.
//
// Most of these are driven through GameUI::Detail and UIManager rather than through the C
// exports, for the same reason GameUIHostTests' session-event arms are: what they pin is the
// mechanism underneath, and the C surface's own contract is pinned in AbiNativeSmokeTest.
//
// ONE arm is the exception and has to be — ValueAndScrollEventsReachAScriptThroughTheRealExports.
// The element ABI's type rules are cross-DLL dynamic_casts to class TEMPLATE instantiations, and
// nowhere else has both a real mounted tree and the exports: AbiNativeSmokeTest links them but
// publishes no host, so every call there stops at not-found before a type is examined. That arm
// calls the real exports; they resolve through the published gameplay host and never touch
// EngineCore, which is what lets a process with no engine — this one — call them.
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <gtest/gtest.h>

#include "GameUIHostTestAccess.h"
#include "UIEventHandlerAccess.h"
#include "UIRgTestHarness.h"

// The element ABI's own exports are under test here, not a restatement of them.
#include "Scripting/ScriptingABI.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"

#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/GameUI/SessionEvent.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

// JobSystem types must precede the ECS Query/World templates (Query.h uses
// JobSystem::TaskHandle), mirroring GameUIHost.cpp's own include ordering.
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECS/ECS.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Assets/AssetManager.h"
#include "Components/UI/UIDocument.h"
#include "AssetCore/AssetEvents.h" // AssetEvent/AssetEventType for the hot-reload drive
#include "AssetCore/GUID.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{
// What the real ABI callback delivered. A cdecl reverse callback cannot capture, so the sink
// travels as the `user` pointer the export hands back verbatim — the same shape the C# binding
// uses for its GCHandle.
struct AbiEventSink
{
    int Count = 0;
    float Value = 0.0f;
    float ScrollX = 0.0f;
    float ScrollY = 0.0f;
    std::uint64_t InstanceId = 0;
    // Copied inside the callback — the payload's own contract: text is valid for the call
    // and no longer, so keeping it means copying it, exactly as a binding must.
    std::string Text;
};

void GE_CDECL AbiEventSinkCallback(const GE_UIEventData* ev, void* user)
{
    auto* sink = static_cast<AbiEventSink*>(user);
    if (!ev || !sink)
        return;
    ++sink->Count;
    sink->Value = ev->value;
    sink->ScrollX = ev->scrollX;
    sink->ScrollY = ev->scrollY;
    sink->InstanceId = ev->elementInstanceId;
    sink->Text = ev->text ? std::string(ev->text, ev->textLen) : std::string();
}

std::filesystem::path WriteUxml(const std::filesystem::path& dir, const char* name, const char* xml)
{
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream f(path);
    f << xml;
    return path;
}

// A mounted HUD document on a PUBLISHED gameplay host, laid out and rendered so the pointer
// can actually hit the button. Same shape as GameUIHostTests' session-click arm, which is
// the shape the ABI's live path has.
struct HudFixture
{
    HudFixture(IDevice* dev, const char* dirName, const char* uxmlBody)
        : Root(std::filesystem::temp_directory_path() / dirName)
        , Pool(2)
        , Rg(dev)
    {
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
        if (!Assets.Initialize(Root, &Pool))
            return;

        // Root classes land on the per-entity container (the layout root merges into it),
        // so size the HUD through a class, not the root's id.
        UxmlPath = WriteUxml(Root, "hud.uxml", uxmlBody);
        CssPath = Root / "hud.css";
        {
            std::ofstream f(CssPath);
            f << ".hud-root { position: relative; width: 200px; height: 200px; }\n"
                 ".btn { position: absolute; left: 40px; top: 40px; width: 80px; height: 40px; }\n";
        }
        auto& reg = Assets.GetRegistry();
        if (!reg.RegisterAsset(UxmlPath) || !reg.RegisterAsset(CssPath))
            return;
        LayoutGuid = reg.GetAssetGUID(UxmlPath);
        StyleGuid = reg.GetAssetGUID(CssPath);
        if (LayoutGuid.IsNull() || StyleGuid.IsNull())
            return;
        Assets.LoadAssetAsync(LayoutGuid, AssetLoadPriority::Normal).get();
        Assets.LoadAssetAsync(StyleGuid, AssetLoadPriority::Normal).get();

        Host = std::make_unique<GameUIHost>(dev, &Assets, &Pool);
        GameUI::SetHost(Host.get());

        World = std::make_unique<ECS::World>(&Pool);
        Components::UIDocument doc;
        doc.Layout.Set(LayoutGuid);
        doc.Style.Set(StyleGuid);
        Entity = World->Create<Components::UIDocument>(doc).GetHandle().id;
        World->ProcessCommands();
        Sync();
        Ok = true;
    }

    ~HudFixture() { GameUI::SetHost(nullptr); }
    HudFixture(const HudFixture&) = delete;
    HudFixture& operator=(const HudFixture&) = delete;

    void Sync()
    {
        GameUIHostTestAccess::SyncFromWorld(*Host, *World);
        Pump();
    }

    void Pump()
    {
        GameUIHostTestAccess::Update(*Host, 0.0f, 200, 200);
        DriveUiRender(*Host->GetUIManager(), Rg);
    }

    // Rewrite the .uxml and drive the reload through the asset event dispatcher, exactly as
    // a designer's save does. Host Update pumps UIHotReload; no SyncFromWorld runs.
    void HotReload(const char* uxmlBody)
    {
        {
            std::ofstream f(UxmlPath);
            f << uxmlBody;
        }
        Assets.GetEventDispatcher().DispatchEvent(
            AssetEvent(AssetEventType::AssetModified, LayoutGuid, AssetType::UILayout, UxmlPath.string()));
        Pump();
    }

    void Pointer(float x, float y, bool down)
    {
        GameUIHostTestAccess::SetPointer(*Host, x, y, down);
        GameUIHostTestAccess::Update(*Host, 0.0f, 200, 200);
    }

    // Press and release at one point — a click when that point is inside the button.
    void ClickAt(float x, float y)
    {
        Pointer(x, y, true);
        Pointer(x, y, false);
    }

    UIElement* Find(const char* id) { return Host->GetUIManager()->GetRootElement()->FindById(id); }
    UIElement* Resolve(uint64_t instanceId) { return Host->GetUIManager()->FindElementByInstanceId(instanceId); }

    // Button geometry from the fixture stylesheet: 80x40 at (40,40).
    static constexpr float kInsideX = 80.0f;
    static constexpr float kInsideY = 60.0f;
    static constexpr float kOutsideX = 10.0f;
    static constexpr float kOutsideY = 10.0f;

    bool Ok = false;
    std::filesystem::path Root;
    std::filesystem::path UxmlPath;
    std::filesystem::path CssPath;
    JobSystem::WorkStealingThreadPool Pool;
    AssetManager Assets;
    UiRgHarness Rg;
    GUID LayoutGuid;
    GUID StyleGuid;
    std::unique_ptr<GameUIHost> Host;
    std::unique_ptr<ECS::World> World;
    uint64_t Entity = 0;
};

constexpr const char* kOneButtonHud =
    "<uielement class='hud-root'><button id='take-damage' class='btn' /></uielement>\n";
} // namespace

// ARM 1 — the defect the id-addressed API had, now structurally impossible.
//
// A handler token is unique only WITHIN one element instance: every element's first handler
// gets key 1. The old exports re-resolved by string id at unregister time, so a script that
// registered on element A and then disposed after A was replaced would unregister key 1 on
// the REPLACEMENT — which may be a built-in control's handler, or another script's.
TEST(GameUIElementHandleTests, StaleInstanceIdUnregisterCannotReachAReplacementsHandler)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm1", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* first = hud.Find("take-damage");
    ASSERT_NE(first, nullptr);
    const uint64_t firstInstance = first->GetInstanceId();

    int scriptClicks = 0;
    const uint64_t token = GameUI::Detail::RegisterSessionEvent(firstInstance, kEventButtonClick, [&](const UIEvent&) { ++scriptClicks; });
    ASSERT_EQ(token, 1u) << "fixture precondition: the script's token must be key 1 on the first element, "
                            "or the collision this arm is about cannot occur";

    // Play stops and the HUD is rebuilt: the first button is destroyed and a NEW button
    // carrying the same string id takes its place.
    GameUI::NotifyGameplayStopped();
    hud.Sync();

    UIElement* replacement = hud.Find("take-damage");
    ASSERT_NE(replacement, nullptr);
    ASSERT_NE(replacement->GetInstanceId(), firstInstance)
        << "fixture precondition: the rebuild must have produced a different element";
    EXPECT_EQ(hud.Resolve(firstInstance), nullptr) << "a destroyed element's id must not resolve";

    // A built-in registers FIRST on the replacement, so it owns key 1 there — the same key
    // the script's stale token carries.
    int builtInClicks = 0;
    const auto builtIn = replacement->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++builtInClicks; });
    ASSERT_EQ(builtIn.Key, 1u) << "fixture precondition: the built-in must own key 1 on the replacement";

    // The script disposes: with instance-id addressing this resolves nothing and does nothing.
    GameUI::Detail::UnregisterSessionEvent(firstInstance, kEventButtonClick, token);

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*replacement, kEventButtonClick), 1u)
        << "a stale token reached the replacement element's handler table";
    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(builtInClicks, 1) << "the built-in's click handler was unregistered by a stale script token";
    EXPECT_EQ(scriptClicks, 0) << "the script's handler died with the element it was registered on";
}

// ARM 2 — play-stop destroys the subtree, so a handle fails closed. Regression lock: red if
// anyone later makes resolution skip the manager's reachability filter or cache the pointer.
TEST(GameUIElementHandleTests, InstanceIdFailsClosedAfterPlayStop)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm2", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    const uint64_t instanceId = btn->GetInstanceId();
    ASSERT_EQ(hud.Resolve(instanceId), btn);

    GameUI::NotifyGameplayStopped();

    EXPECT_EQ(hud.Resolve(instanceId), nullptr) << "play-stop dropped the subtree but the id still resolves";
    EXPECT_EQ(GameUI::Detail::RegisterSessionEvent(instanceId, kEventButtonClick, [](const UIEvent&) {}), 0u)
        << "the gate bound to an element that play-stop destroyed";
}

// ARM 3 — owned but detached must be unfindable, so a script write cannot land on something
// the user cannot see. What enforces it on THIS path is index eviction, not the reachability
// filter: TakeChild clears the child's owner manager, and that detach branch erases its
// instance-id index entry, so the lookup misses in the index and never reaches the filter.
// The filter is the second gate, for elements that keep their owner while leaving the root's
// subtree.
TEST(GameUIElementHandleTests, InstanceIdFailsClosedWhenDetachedButAlive)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm3", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    const uint64_t instanceId = btn->GetInstanceId();
    UIElement* parent = btn->GetParent();
    ASSERT_NE(parent, nullptr);

    // Out of the tree WITHOUT destroying it — the unique_ptr keeps it alive.
    std::unique_ptr<UIElement> detached = parent->TakeChild(btn);
    ASSERT_NE(detached, nullptr);
    ASSERT_EQ(detached->GetInstanceId(), instanceId) << "detaching must not change identity";

    EXPECT_EQ(hud.Resolve(instanceId), nullptr)
        << "a detached-but-alive element resolved — the detach path left its index entry behind";
    EXPECT_EQ(GameUI::Detail::RegisterSessionEvent(instanceId, kEventButtonClick, [](const UIEvent&) {}), 0u);
}

// ARM 4 — a .uxml reconcile REUSES an id-matched element of unchanged type, handler table
// intact. So the handle stays valid and the subscription must NOT be re-registered: a binder
// that re-subscribes on every bind-generation bump double-fires here. This arm locks the
// corrected GetBindGeneration comment.
TEST(GameUIElementHandleTests, ReconcilePreservesInstanceIdAndItsSubscription)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm4", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    const uint64_t instanceId = btn->GetInstanceId();

    int clicks = 0;
    ASSERT_NE(GameUI::Detail::RegisterSessionEvent(instanceId, kEventButtonClick, [&](const UIEvent&) { ++clicks; }), 0u);
    const uint64_t beforeGeneration = GameUI::GetBindGeneration();

    // The id still declares a button of the same type, so the element survives the reconcile;
    // the added sibling proves the reconcile actually ran.
    hud.HotReload("<uielement class='hud-root'><button id='take-damage' class='btn' />"
                  "<uielement id='added' /></uielement>\n");
    ASSERT_NE(hud.Find("added"), nullptr) << "fixture precondition: the reconcile must have run";
    EXPECT_GT(GameUI::GetBindGeneration(), beforeGeneration)
        << "fixture precondition: the reconcile must bump the generation this arm is about";

    UIElement* after = hud.Find("take-damage");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->GetInstanceId(), instanceId) << "the reconcile replaced an element it should have reused";
    EXPECT_EQ(hud.Resolve(instanceId), after) << "a preserved element must stay resolvable by its id";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*after, kEventButtonClick), 1u)
        << "the preserved element lost (or gained) a subscription across the reconcile";

    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(clicks, 1) << "a preserved subscription must fire exactly once — re-registering on the "
                            "bind-generation bump would double-fire here";
}

// ARM 4, for the convenience wrapper. Button::SetOnClick stores a token naming a table
// entry, so a reconcile that REUSES the element leaves both intact and the callback must
// fire exactly once. The token is not re-minted and must not need to be: an element that
// survives a reconcile keeps its handler table, which is the same property arm 4 above
// pins for a directly-registered subscription.
TEST(GameUIElementHandleTests, ReconcilePreservesASetOnClickSubscription)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm4_setonclick", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    auto* btn = dynamic_cast<Button*>(hud.Find("take-damage"));
    ASSERT_NE(btn, nullptr) << "fixture precondition: the id must name a Button";
    const uint64_t instanceId = btn->GetInstanceId();

    int clicks = 0;
    btn->SetOnClick([&clicks](UIEvent&) { ++clicks; });
    const uint64_t beforeGeneration = GameUI::GetBindGeneration();

    hud.HotReload("<uielement class='hud-root'><button id='take-damage' class='btn' />"
                  "<uielement id='added' /></uielement>\n");
    ASSERT_NE(hud.Find("added"), nullptr) << "fixture precondition: the reconcile must have run";
    EXPECT_GT(GameUI::GetBindGeneration(), beforeGeneration)
        << "fixture precondition: the reconcile must bump the generation this arm is about";

    auto* after = dynamic_cast<Button*>(hud.Find("take-damage"));
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->GetInstanceId(), instanceId)
        << "fixture precondition: the reconcile replaced an element it should have reused";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*after, kEventButtonClick), 1u)
        << "the preserved element lost (or gained) a convenience subscription across the reconcile";

    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(clicks, 1) << "a preserved SetOnClick callback must fire exactly once";

    // The stored token still names the live entry, so the slot is still a SET.
    int replacement = 0;
    after->SetOnClick([&replacement](UIEvent&) { ++replacement; });
    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(clicks, 1)
        << "the token did not survive the reconcile — the replacement stacked instead of replacing";
    EXPECT_EQ(replacement, 1);
    // After the dispatch whose drain erases the replaced entry (see the note in
    // ButtonClickEventTests.SetOnClickReplacesThePreviousCallback).
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*after, kEventButtonClick), 1u);
}

// ARM 6 — a script's instance id can never resolve to editor chrome. Chrome runs on its own
// UIManager with its own index, and resolution goes through the PUBLISHED gameplay host's
// manager, so this holds structurally rather than by a predicate someone could forget.
TEST(GameUIElementHandleTests, InstanceIdFromAnotherManagerNeverResolvesThroughTheGameplayHost)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm6", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    // A second, unpublished host standing in for editor chrome (or a quad-view preview).
    GameUIHost chrome(dev.get(), /*assetManager=*/nullptr, /*jobSystem=*/nullptr);
    std::unique_ptr<UIElement> chromeRoot;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(
        R"(<uielement id='chrome-root'><button id='chrome-btn' /></uielement>)", chromeRoot));
    chrome.GetUIManager()->SetRoot(std::move(chromeRoot));
    UIElement* chromeBtn = chrome.GetUIManager()->GetRootElement()->FindById("chrome-btn");
    ASSERT_NE(chromeBtn, nullptr);
    const uint64_t chromeInstance = chromeBtn->GetInstanceId();
    ASSERT_EQ(chrome.GetUIManager()->FindElementByInstanceId(chromeInstance), chromeBtn)
        << "fixture precondition: the id must be live in its OWN manager";

    EXPECT_EQ(hud.Resolve(chromeInstance), nullptr)
        << "an element owned by another UIManager resolved through the gameplay host";
    EXPECT_EQ(GameUI::Detail::RegisterSessionEvent(chromeInstance, kEventButtonClick, [](const UIEvent&) {}), 0u)
        << "a script could register a handler on editor chrome";
}

// ARM 9 — click semantics. A release over a button whose press began elsewhere is NOT a
// click, and neither is a press on the button released outside it. Red before kEventButtonClick:
// the subscription was on raw kEventMouseUp, which fires on the first case.
TEST(GameUIElementHandleTests, ClickRequiresTheArmAndNotJustAMouseUp)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_arm9", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    ASSERT_GT(btn->GetLayoutWidth(), 0.0f) << "button has no layout — the pointer could never hit it";

    int clicks = 0;
    ASSERT_NE(GameUI::Detail::RegisterSessionEvent(btn->GetInstanceId(), kEventButtonClick, [&](const UIEvent&) { ++clicks; }), 0u);

    // Positive control, and the permanent record of what the old binding did: a raw
    // kEventMouseUp subscription on the SAME element, which is what the pre-slice-1 managed
    // click created. Every divergence below is this counter moving while `clicks` does not.
    int mouseUps = 0;
    btn->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++mouseUps; });

    // Press OUTSIDE, release over the button: not a click.
    hud.Pointer(HudFixture::kOutsideX, HudFixture::kOutsideY, true);
    hud.Pointer(HudFixture::kInsideX, HudFixture::kInsideY, false);
    EXPECT_EQ(clicks, 0) << "a release over an unarmed button was reported as a click";
    ASSERT_EQ(mouseUps, 1) << "control: the old raw-mouse-up binding DID fire here — if this is 0 the "
                              "arm proves nothing, because the event never reached the button at all";

    // Press INSIDE, release outside: also not a click.
    hud.Pointer(HudFixture::kInsideX, HudFixture::kInsideY, true);
    hud.Pointer(HudFixture::kOutsideX, HudFixture::kOutsideY, false);
    EXPECT_EQ(clicks, 0) << "a press that released off the button was reported as a click";

    // Press and release inside: a click, and the two notions finally agree.
    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(clicks, 1) << "an armed-and-inside press/release did not produce a click";
    EXPECT_EQ(mouseUps, 3) << "control: mouse-up fired on all three releases, click on only one — "
                              "that difference is the whole point of binding to kEventButtonClick";
}

// Why GE_UIElement_RegisterEvent refuses UI.ButtonClick on a non-Button, pinned as a fact about
// the tree rather
// than as a restatement of the ABI's own check: a click subscription on an element that does
// not dispatch kEventButtonClick can NEVER fire, however live and resolvable that element is. The
// failure would be silent (Ok plus a valid token) and permanent, so the ABI reports it instead
// — and this arm is what makes that check correct today. It goes red the day something other
// than Button dispatches a click, which is exactly when the ABI's type check must widen.
TEST(GameUIElementHandleTests, APlainElementNeverDispatchesAClickHoweverItIsPressed)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kMixedHud =
        "<uielement class='hud-root'><uielement id='not-a-button' class='btn' /></uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_nonbutton", kMixedHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* plain = hud.Find("not-a-button");
    ASSERT_NE(plain, nullptr);
    ASSERT_EQ(dynamic_cast<Button*>(plain), nullptr) << "fixture precondition: must not be a Button";
    ASSERT_GT(plain->GetLayoutWidth(), 0.0f) << "fixture precondition: it must be hittable";
    EXPECT_EQ(hud.Resolve(plain->GetInstanceId()), plain)
        << "control: the element IS live and resolvable, so what follows is about its TYPE and "
           "not about a failed lookup";

    int clicks = 0;
    ASSERT_NE(GameUI::Detail::RegisterSessionEvent(plain->GetInstanceId(), kEventButtonClick, [&](const UIEvent&) { ++clicks; }), 0u)
        << "the gate itself is type-agnostic — it is the ABI that refuses, and it refuses "
           "precisely because of what this arm asserts next";

    int mouseUps = 0;
    plain->RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++mouseUps; });

    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);

    EXPECT_EQ(mouseUps, 1) << "control: the press/release really did reach this element — without "
                              "this the click assertion below would be vacuous";
    EXPECT_EQ(clicks, 0) << "a plain element dispatched a click; the ABI's non-Button rejection is "
                            "now wrong and must be widened to whatever dispatches kEventButtonClick";
}

// END-TO-END through the REAL exports, on a REAL tree — the only place both exist at once.
//
// The element ABI's type rules are cross-DLL `dynamic_cast`s to class TEMPLATE instantiations
// (`Field<float>`, `Field<bool>`), and that is not a formality: the cast must find type_info the
// exe and GameEngine.Native agree on, for a template the engine instantiated. AbiNativeSmokeTest
// links the exports but publishes no host, so every call there stops at not-found long before a
// type is examined; the managed suite's double answers with tag strings, not casts. So without
// this arm the casts are exercised by nothing, and a build where they silently always fail would
// look exactly like a build where they work.
//
// It drives the control's own mutation path and asserts DELIVERY, not registration: a token is
// not evidence that anything ever arrives.
TEST(GameUIElementHandleTests, ValueAndScrollEventsReachAScriptThroughTheRealExports)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kControlsHud =
        "<uielement class='hud-root'>"
        "<slider id='vol' />"
        "<checkbox id='mute' />"
        "<scrollview id='list' />"
        "<textfield id='name' />"
        "<intfield id='num' />"
        "<uielement id='plain' />"
        "</uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_valueevents", kControlsHud);
    ASSERT_TRUE(hud.Ok);

    auto* slider = dynamic_cast<Slider*>(hud.Find("vol"));
    auto* checkbox = dynamic_cast<ToggleBase*>(hud.Find("mute"));
    auto* scroll = dynamic_cast<ScrollView*>(hud.Find("list"));
    auto* text = dynamic_cast<Field<std::string>*>(hud.Find("name"));
    UIElement* intField = hud.Find("num");
    UIElement* plain = hud.Find("plain");
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(checkbox, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(text, nullptr);
    ASSERT_NE(intField, nullptr);
    ASSERT_NE(plain, nullptr);

    auto subscribe = [](std::uint64_t id, const char* name, AbiEventSink* sink)
    {
        std::uint64_t token = 0;
        return GE_UIElement_RegisterEvent(id, name, &AbiEventSinkCallback, sink, &token);
    };

    AbiEventSink valueSink;
    AbiEventSink boolSink;
    AbiEventSink scrollSink;
    ASSERT_EQ(subscribe(slider->GetInstanceId(), "UI.ValueChanged", &valueSink), GE_Result_Ok)
        << "the cross-DLL dynamic_cast to Field<float> failed — a Slider IS one";
    ASSERT_EQ(subscribe(checkbox->GetInstanceId(), "UI.ValueChanged", &boolSink), GE_Result_Ok);
    ASSERT_EQ(subscribe(scroll->GetInstanceId(), "UI.ScrollOffsetChanged", &scrollSink), GE_Result_Ok);

    // Drive the controls' own paths, not a synthetic dispatch.
    slider->SetMin(0.0f);
    slider->SetMax(10.0f);
    slider->SetValue(4.5f);
    EXPECT_EQ(valueSink.Count, 1) << "a value change on a real Slider did not reach the script";
    EXPECT_FLOAT_EQ(valueSink.Value, 4.5f) << "the widened payload did not survive the crossing";
    EXPECT_EQ(valueSink.InstanceId, slider->GetInstanceId());

    checkbox->SetChecked(true);
    EXPECT_EQ(boolSink.Count, 1);
    EXPECT_FLOAT_EQ(boolSink.Value, 1.0f) << "a bool control reports 1, not a zero payload";

    scroll->SetContentSize(100.0f, 1000.0f);
    scroll->SetViewportSize(100.0f, 100.0f);
    scroll->SetScrollY(60.0f);
    EXPECT_EQ(scrollSink.Count, 0) << "the offset event is coalesced to the flush";
    scroll->FlushPendingScrollChanged();
    EXPECT_EQ(scrollSink.Count, 1);
    EXPECT_FLOAT_EQ(scrollSink.ScrollY, 60.0f);

    // The string payload, through the same export and the same cross-DLL cast — this time to
    // Field<std::string>. The drive is the public pair TextFieldBase's own commit calls.
    AbiEventSink textSink;
    ASSERT_EQ(subscribe(text->GetInstanceId(), "UI.ValueChanged", &textSink), GE_Result_Ok)
        << "the cross-DLL dynamic_cast to Field<std::string> failed — a TextField IS one";
    text->SetValue(std::string("hello from the tree"));
    text->NotifyValueChanged();
    EXPECT_EQ(textSink.Count, 1) << "a text change on a real TextField did not reach the script";
    EXPECT_EQ(textSink.Text, "hello from the tree") << "the UTF-8 payload did not survive the crossing";
    EXPECT_FLOAT_EQ(textSink.Value, 0.0f) << "a string field has no numeric payload";

    // Refusals, through the same export: nothing dispatches these there, so a token would be a
    // permanent silent no-op.
    AbiEventSink unused;
    EXPECT_EQ(subscribe(intField->GetInstanceId(), "UI.ValueChanged", &unused), GE_Result_InvalidArg)
        << "Field<int> still has no payload encoding; the string widening must not over-widen";
    EXPECT_EQ(subscribe(plain->GetInstanceId(), "UI.ValueChanged", &unused), GE_Result_InvalidArg);
    EXPECT_EQ(subscribe(slider->GetInstanceId(), "UI.ScrollOffsetChanged", &unused), GE_Result_InvalidArg)
        << "a Slider is not a ScrollView, however value-shaped it is";

    // Control: the events every element can take stay open to all of them, so the refusals above
    // are about these events and not about a fixture that cannot resolve anything.
    EXPECT_EQ(subscribe(plain->GetInstanceId(), "UI.MouseEnter", &unused), GE_Result_Ok);
    EXPECT_EQ(subscribe(text->GetInstanceId(), "UI.FocusIn", &unused), GE_Result_Ok);
    EXPECT_EQ(unused.Count, 0) << "none of the above should have fired anything";
}

// The typed value accessors, end to end for the same cross-DLL reason as the arm above: their
// type rules are dynamic_casts to Field<T> instantiations, and only here do the exports and a
// real mounted tree meet. Under test: the write routes the CONTROL's own setter (clamping
// included), notify=0 tells nobody, wrong type is InvalidArg on a LIVE element, and the
// string getters honour the copy-what-fits/report-full-length buffer contract.
TEST(GameUIElementHandleTests, TypedValueAccessorsRoundTripThroughTheRealExports)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kControlsHud =
        "<uielement class='hud-root'>"
        "<slider id='vol' />"
        "<checkbox id='mute' />"
        "<scrollview id='list' />"
        "<textfield id='name' />"
        "<dropdown id='mode' />"
        "</uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_typedvalues", kControlsHud);
    ASSERT_TRUE(hud.Ok);

    auto* slider = dynamic_cast<Slider*>(hud.Find("vol"));
    auto* checkbox = dynamic_cast<ToggleBase*>(hud.Find("mute"));
    auto* scroll = dynamic_cast<ScrollView*>(hud.Find("list"));
    UIElement* text = hud.Find("name");
    auto* dropdown = dynamic_cast<Dropdown*>(hud.Find("mode"));
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(checkbox, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(text, nullptr);
    ASSERT_NE(dropdown, nullptr);

    // Float: the export must route Slider::SetValue, which clamps before storing.
    slider->SetMin(0.0f);
    slider->SetMax(10.0f);
    const std::uint64_t sliderId = slider->GetInstanceId();
    EXPECT_EQ(GE_UIElement_SetValueFloat(sliderId, 999.0f, 1), GE_Result_Ok);
    float f = -1.0f;
    EXPECT_EQ(GE_UIElement_GetValueFloat(sliderId, &f), GE_Result_Ok);
    EXPECT_FLOAT_EQ(f, 10.0f) << "the requested 999 must have been clamped by the control itself";

    // The notify flag, observed through a real subscription rather than trusted.
    AbiEventSink sliderSink;
    std::uint64_t token = 0;
    ASSERT_EQ(GE_UIElement_RegisterEvent(sliderId, "UI.ValueChanged", &AbiEventSinkCallback,
                                         &sliderSink, &token), GE_Result_Ok);
    EXPECT_EQ(GE_UIElement_SetValueFloat(sliderId, 3.0f, 0), GE_Result_Ok);
    EXPECT_EQ(sliderSink.Count, 0) << "notify=0 must tell nobody";
    EXPECT_EQ(GE_UIElement_SetValueFloat(sliderId, 4.0f, 1), GE_Result_Ok);
    EXPECT_EQ(sliderSink.Count, 1) << "notify=1 routes Slider::SetValue, which announces";
    EXPECT_FLOAT_EQ(sliderSink.Value, 4.0f);

    // Bool, landing on the control (the checked-class sync is the control's, not the ABI's).
    const std::uint64_t checkboxId = checkbox->GetInstanceId();
    EXPECT_EQ(GE_UIElement_SetValueBool(checkboxId, 1, 1), GE_Result_Ok);
    int32_t b = 0;
    EXPECT_EQ(GE_UIElement_GetValueBool(checkboxId, &b), GE_Result_Ok);
    EXPECT_EQ(b, 1);
    EXPECT_TRUE(checkbox->IsChecked());

    // Text, and the buffer contract: a short buffer still learns the full length.
    const std::uint64_t textId = text->GetInstanceId();
    EXPECT_EQ(GE_UIElement_SetValueText(textId, "hello", 0), GE_Result_Ok);
    char buf[32] = {};
    int32_t len = 0;
    EXPECT_EQ(GE_UIElement_GetValueText(textId, buf, 2, &len), GE_Result_Ok);
    EXPECT_EQ(len, 5) << "truncation must be visible, never silent";
    EXPECT_EQ(GE_UIElement_GetValueText(textId, buf, sizeof(buf), &len), GE_Result_Ok);
    ASSERT_EQ(len, 5);
    EXPECT_EQ(std::string(buf, 5), "hello");

    // Today's TextField contract, pinned so a change to it is a decision: programmatic
    // SetValue is a silent sync — TextFieldBase notifies only from user editing paths — so
    // notify=1 routes it and still announces nothing. If this arm goes red because the
    // control started announcing, update the managed wrapper docs in the same change.
    AbiEventSink textSink;
    ASSERT_EQ(GE_UIElement_RegisterEvent(textId, "UI.ValueChanged", &AbiEventSinkCallback,
                                         &textSink, &token), GE_Result_Ok);
    EXPECT_EQ(GE_UIElement_SetValueText(textId, "world", 1), GE_Result_Ok);
    EXPECT_EQ(textSink.Count, 0)
        << "TextFieldBase::SetValue started notifying; the wrapper docs promise the opposite";

    // Dropdown: index round-trip, the label getter, and the event carrying the option VALUE.
    dropdown->SetOptions({{"val-a", "Label A", ""}, {"val-b", "Label B", ""}}, 0);
    const std::uint64_t dropdownId = dropdown->GetInstanceId();
    AbiEventSink dropSink;
    ASSERT_EQ(GE_UIElement_RegisterEvent(dropdownId, "UI.ValueChanged", &AbiEventSinkCallback,
                                         &dropSink, &token), GE_Result_Ok);
    EXPECT_EQ(GE_UIElement_SetDropdownSelectedIndex(dropdownId, 1, 1), GE_Result_Ok);
    int32_t index = -1;
    EXPECT_EQ(GE_UIElement_GetDropdownSelectedIndex(dropdownId, &index), GE_Result_Ok);
    EXPECT_EQ(index, 1);
    EXPECT_EQ(dropSink.Count, 1);
    EXPECT_EQ(dropSink.Text, "val-b") << "the selection event carries the option VALUE string";
    EXPECT_EQ(GE_UIElement_GetDropdownSelectedLabel(dropdownId, buf, sizeof(buf), &len), GE_Result_Ok);
    EXPECT_EQ(std::string(buf, static_cast<size_t>(len)), "Label B");
    EXPECT_EQ(GE_UIElement_GetValueText(dropdownId, buf, sizeof(buf), &len), GE_Result_Ok)
        << "the dropdown's field value READS through the generic text getter";
    EXPECT_EQ(std::string(buf, static_cast<size_t>(len)), "val-b");
    EXPECT_EQ(GE_UIElement_SetValueText(dropdownId, "val-a", 0), GE_Result_InvalidArg)
        << "raw field writes on a Dropdown would desync index and header; refused by design";
    EXPECT_EQ(GE_UIElement_SetDropdownSelectedIndex(dropdownId, 0, 0), GE_Result_Ok);
    EXPECT_EQ(dropSink.Count, 1) << "the without-notify selection told the table anyway";

    // Scroll offsets: the setter is the control's own clamped, coalescing path.
    scroll->SetContentSize(100.0f, 1000.0f);
    scroll->SetViewportSize(100.0f, 100.0f);
    const std::uint64_t scrollId = scroll->GetInstanceId();
    EXPECT_EQ(GE_UIElement_SetScrollY(scrollId, 70.0f), GE_Result_Ok);
    float sx = -1.0f;
    float sy = -1.0f;
    EXPECT_EQ(GE_UIElement_GetScrollOffset(scrollId, &sx, &sy), GE_Result_Ok);
    EXPECT_FLOAT_EQ(sy, 70.0f);
    EXPECT_FLOAT_EQ(sx, 0.0f);

    // Wrong TYPE on a LIVE element is InvalidArg: inventing a default would hide a wrong-id
    // bug from the script that made it.
    EXPECT_EQ(GE_UIElement_SetValueFloat(checkboxId, 1.0f, 1), GE_Result_InvalidArg);
    EXPECT_EQ(GE_UIElement_GetValueBool(sliderId, &b), GE_Result_InvalidArg);
    EXPECT_EQ(GE_UIElement_GetScrollOffset(sliderId, &sx, &sy), GE_Result_InvalidArg);
    EXPECT_EQ(GE_UIElement_GetDropdownSelectedIndex(textId, &index), GE_Result_InvalidArg);

    // A dead handle is NotFound everywhere in the element ABI.
    EXPECT_EQ(GE_UIElement_GetValueFloat(0xFFFFFFFFFFFFull, &f), GE_Result_NotFound);
}

namespace
{
// A callback that writes the very field it is being told about, through the real export —
// what any script handler can legally do from inside its own event. Captures the payload
// before AND after the write, because the invariant is that the write moves the FIELD and
// not the payload.
struct MutatingAbiSink
{
    std::string Before;
    std::string After;
    const char* Replacement = "";
    int Count = 0;
};

void GE_CDECL MutatingAbiSinkCallback(const GE_UIEventData* ev, void* user)
{
    auto* sink = static_cast<MutatingAbiSink*>(user);
    if (!ev || !sink)
        return;
    ++sink->Count;
    sink->Before = ev->text ? std::string(ev->text, ev->textLen) : std::string();
    GE_UIElement_SetValueText(ev->elementInstanceId, sink->Replacement, 0);
    sink->After = ev->text ? std::string(ev->text, ev->textLen) : std::string();
}
} // namespace

// THE LIFETIME INVARIANT AT THE ABI SEAM: the text payload a callback is handed stays
// valid and byte-identical for the whole dispatch,
// even when a handler writes the field being announced through the real export — the write
// that reallocates the field's own buffer. Without the dispatch-window copy this test is a
// use-after-free reached from ordinary safe C#; the managed ref struct can prevent escape,
// only this native guarantee can prevent staleness.
TEST(GameUIElementHandleTests, TextPayloadSurvivesAMidDispatchWriteThroughTheRealExport)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kHud = "<uielement class='hud-root'><textfield id='name' /></uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_textlifetime", kHud);
    ASSERT_TRUE(hud.Ok);
    auto* text = dynamic_cast<Field<std::string>*>(hud.Find("name"));
    ASSERT_NE(text, nullptr);
    const std::uint64_t id = text->GetInstanceId();

    const std::string original = "original-payload-well-beyond-sso-capacity";
    MutatingAbiSink mutating;
    mutating.Replacement = "a replacement long enough to force a fresh heap allocation";
    AbiEventSink later;
    std::uint64_t token = 0;
    ASSERT_EQ(GE_UIElement_RegisterEvent(id, "UI.ValueChanged", &MutatingAbiSinkCallback,
                                         &mutating, &token), GE_Result_Ok);
    ASSERT_EQ(GE_UIElement_RegisterEvent(id, "UI.ValueChanged", &AbiEventSinkCallback,
                                         &later, &token), GE_Result_Ok);

    text->SetValue(original);
    text->NotifyValueChanged();

    EXPECT_EQ(mutating.Count, 1);
    EXPECT_EQ(mutating.Before, original);
    EXPECT_EQ(mutating.After, original)
        << "the payload moved under the very handler that wrote the field";
    EXPECT_EQ(later.Count, 1);
    EXPECT_EQ(later.Text, original)
        << "the subscriber AFTER the mutating one was handed the buffer the mutation freed";
    EXPECT_EQ(text->GetValue(), mutating.Replacement)
        << "the mid-dispatch write must still APPLY — the guarantee is about the payload, "
           "never about dropping the write";
}

// ARM 5 — what a reconcile DOES clobber. The handle survives and stays writable, but the
// authored `text` attribute is re-applied, so a one-shot text write is lost while a style
// override written through Overrides() survives (a different bag from the inline-style
// attribute). This is the contract the managed API documents: a per-frame driver self-heals
// in one frame, a one-shot writer does not. Red if `text` is ever added to the reconcile's
// preserve list without updating that contract.
TEST(GameUIElementHandleTests, ReconcileRestoresAuthoredTextButNotStyleOverrides)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kLabelHud =
        "<uielement class='hud-root'><label id='hp-label' class='btn' text='-- / --' /></uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_arm5", kLabelHud);
    ASSERT_TRUE(hud.Ok);

    auto* label = dynamic_cast<Label*>(hud.Find("hp-label"));
    ASSERT_NE(label, nullptr);
    const uint64_t instanceId = label->GetInstanceId();
    ASSERT_EQ(label->GetText(), "-- / --") << "fixture precondition: the authored text must have applied";

    // What the ABI's SetLabelText and SetWidthPercent do.
    label->SetText("100 / 100");
    label->Overrides().Set(Style::Width, StyleLength::Percent(42.0f));

    hud.HotReload(kLabelHud);

    UIElement* after = hud.Resolve(instanceId);
    ASSERT_EQ(after, label) << "fixture precondition: the reconcile must have preserved the element";
    EXPECT_EQ(label->GetText(), "-- / --")
        << "the authored text was NOT re-applied — the managed contract says a one-shot text write "
           "is lost across a .uxml reconcile; update the contract if this changed deliberately";
    EXPECT_EQ(after->Overrides().Get(Style::Width), std::optional<StyleLength>(StyleLength::Percent(42.0f)))
        << "a style override written through the override bag must survive a reconcile";
}

// ARM 7 — Overlay suppression is display:none on the per-entity container, never a detach,
// so a handle into a suppressed document stays valid: writes land and become visible when
// the Fullscreen document goes away. Red if suppression is ever reimplemented by removing
// the subtree.
TEST(GameUIElementHandleTests, InstanceIdStaysValidWhileOverlayIsSuppressed)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(2);
    const auto root = std::filesystem::temp_directory_path() / "gameui_handle_arm7";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));

    const auto pathHud = WriteUxml(root, "overlay.uxml", "<uielement id='ov-root'><uielement id='ov-leaf' /></uielement>\n");
    const auto pathMenu = WriteUxml(root, "fullscreen.uxml", "<uielement id='fs-root'><uielement id='fs-leaf' /></uielement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathHud));
    ASSERT_TRUE(reg.RegisterAsset(pathMenu));
    const GUID hudGuid = reg.GetAssetGUID(pathHud);
    const GUID menuGuid = reg.GetAssetGUID(pathMenu);
    ASSERT_FALSE(hudGuid.IsNull());
    ASSERT_FALSE(menuGuid.IsNull());
    assets.LoadAssetAsync(hudGuid, AssetLoadPriority::Normal).get();
    assets.LoadAssetAsync(menuGuid, AssetLoadPriority::Normal).get();

    GameUIHost host(dev.get(), &assets, &pool);
    GameUI::SetHost(&host);
    struct HostScope
    {
        ~HostScope() { GameUI::SetHost(nullptr); }
    } hostScope;

    ECS::World world(&pool);
    Components::UIDocument hudDoc;
    hudDoc.Layout.Set(hudGuid);
    hudDoc.RenderMode = Components::UIRenderMode::Overlay;
    Components::UIDocument menuDoc;
    menuDoc.Layout.Set(menuGuid);
    menuDoc.RenderMode = Components::UIRenderMode::Fullscreen;
    const auto hudE = world.Create<Components::UIDocument>(hudDoc);
    auto menuE = world.Create<Components::UIDocument>(menuDoc);
    world.ProcessCommands();
    GameUIHostTestAccess::SyncFromWorld(host, world);

    UIElement* leaf = host.GetUIManager()->GetRootElement()->FindById("ov-leaf");
    ASSERT_NE(leaf, nullptr);
    const uint64_t instanceId = leaf->GetInstanceId();
    ASSERT_TRUE(GameUIHostTestAccess::AnyReadyFullscreenSubtree(host))
        << "fixture precondition: the Fullscreen document must be ready";
    UIElement* hudRoot = host.FindDocumentRoot(hudE.GetHandle().id);
    ASSERT_NE(hudRoot, nullptr);
    ASSERT_EQ(hudRoot->Overrides().Get(Style::Display), std::optional<DisplayMode>(DisplayMode::None))
        << "fixture precondition: the Overlay must actually be suppressed";

    EXPECT_EQ(host.GetUIManager()->FindElementByInstanceId(instanceId), leaf)
        << "hidden is not dead — a suppressed Overlay's elements must stay resolvable";

    // A write lands while suppressed and is still there when the menu goes away.
    leaf->Overrides().Set(Style::Width, StyleLength::Percent(42.0f));
    menuE.SetEnabled<Components::UIDocument>(false);
    GameUIHostTestAccess::SyncFromWorld(host, world);
    EXPECT_EQ(host.GetUIManager()->FindElementByInstanceId(instanceId), leaf);
    EXPECT_FALSE(hudRoot->Overrides().Get(Style::Display).has_value())
        << "fixture precondition: the Overlay must be restored";
    EXPECT_EQ(leaf->Overrides().Get(Style::Width), std::optional<StyleLength>(StyleLength::Percent(42.0f)))
        << "a write made while suppressed must still be there when the document is shown again";
}

// ---------------------------------------------------------------------------------------
// Slice 2 — the mechanisms the managed object model is built on: tag identity for type
// mapping, and generic event subscription. Driven through the registry and GameUI::Detail
// for the same reason the arms above are: the C exports run EnsureEngineInitialized and
// would bootstrap a whole Engine inside this suite. AbiNativeSmokeTest pins the C surface.
// ---------------------------------------------------------------------------------------

// A binding maps an element's tag id to its own wrapper type, and it learns which id means
// "button" by asking for the id of the NAME. Those are two different lookups —
// GetTagIdForType reads a value cached at registration, GetTagId re-derives it from the
// canonical tag — so the whole type-mapping scheme rests on them agreeing. Red the day one
// of them changes how it derives the id: the binding would then mint Ui.Element for every
// button, silently, with nothing else failing.
TEST(GameUIElementHandleTests, TagIdByNameAgreesWithTagIdByType)
{
    UIRegistration::RegisterBuiltInControls();
    const auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    const StringId buttonByName = reg.GetTagId("button");
    const StringId labelByName = reg.GetTagId("label");
    const StringId elementByName = reg.GetTagId("uielement");
    ASSERT_NE(buttonByName, 0u) << "fixture precondition: built-in controls must be registered";

    EXPECT_EQ(buttonByName, reg.GetTagIdForType(typeid(Button)));
    EXPECT_EQ(labelByName, reg.GetTagIdForType(typeid(Label)));
    EXPECT_EQ(elementByName, reg.GetTagIdForType(typeid(UIElement)));

    // Distinctness is the other half: ids that collided would map two native types onto one
    // wrapper without any lookup ever failing.
    EXPECT_NE(buttonByName, labelByName);
    EXPECT_NE(buttonByName, elementByName);
    EXPECT_NE(labelByName, elementByName);

    // An unregistered type answers 0 — a valid "no mapping", which is what makes the
    // binding's fall back to the base wrapper correct rather than a guess.
    struct UnregisteredElement : UIElement
    {
    };
    EXPECT_EQ(reg.GetTagIdForType(typeid(UnregisteredElement)), 0u);
    EXPECT_EQ(reg.GetTagId("not-a-registered-tag"), 0u);
}

// The tag id a live element reports is its OWN type's, not its parent's or its children's.
// Button builds a Label child in its constructor, so an implementation that walked the
// subtree — or that read the authored tag string rather than the type — would pass a
// button-only fixture and mint the wrong wrapper here.
TEST(GameUIElementHandleTests, LiveElementsReportTheirOwnTagId)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kMixedHud = "<uielement class='hud-root'><button id='btn' class='btn' />"
                                      "<label id='lbl' text='x' /><uielement id='plain' /></uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_tagid", kMixedHud);
    ASSERT_TRUE(hud.Ok);

    const auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    UIElement* btn = hud.Find("btn");
    UIElement* lbl = hud.Find("lbl");
    UIElement* plain = hud.Find("plain");
    ASSERT_NE(btn, nullptr);
    ASSERT_NE(lbl, nullptr);
    ASSERT_NE(plain, nullptr);

    EXPECT_EQ(reg.GetTagIdForType(typeid(*btn)), reg.GetTagId("button"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(*lbl)), reg.GetTagId("label"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(*plain)), reg.GetTagId("uielement"));
}

// ARM 15 (native half) — subscriptions are per-registration, not per-element: two
// registrations on one event id are TWO entries. That is what makes the managed layer's
// promise meaningful — one native entry however many C# listeners a multicast event has —
// because the naive one-entry-per-listener implementation is visible right here.
// Unregistering one must leave the other's entry AND its callable intact.
TEST(GameUIElementHandleTests, EachSessionRegistrationIsItsOwnHandlerEntry)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_refcount", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    const uint64_t instanceId = btn->GetInstanceId();
    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(*btn, kEventButtonClick), 0u)
        << "fixture precondition: nothing else is subscribed to this button's click";

    int first = 0;
    int second = 0;
    const uint64_t tokenA =
        GameUI::Detail::RegisterSessionEvent(instanceId, kEventButtonClick, [&](const UIEvent&) { ++first; });
    const uint64_t tokenB =
        GameUI::Detail::RegisterSessionEvent(instanceId, kEventButtonClick, [&](const UIEvent&) { ++second; });
    ASSERT_NE(tokenA, 0u);
    ASSERT_NE(tokenB, 0u);
    ASSERT_NE(tokenA, tokenB) << "two registrations must mint distinct tokens";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*btn, kEventButtonClick), 2u);

    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 1);

    GameUI::Detail::UnregisterSessionEvent(instanceId, kEventButtonClick, tokenA);
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(*btn, kEventButtonClick, tokenA))
        << "unregister must RELEASE the callable, not just deactivate the entry — a retained "
           "one keeps a managed GCHandle rooted and pins the scripts load context";
    EXPECT_TRUE(UIEventHandlerAccess::HandlerHasCallable(*btn, kEventButtonClick, tokenB))
        << "unregistering one subscription reached another's callable";

    hud.ClickAt(HudFixture::kInsideX, HudFixture::kInsideY);
    EXPECT_EQ(first, 1) << "an unregistered subscription still fired";
    EXPECT_EQ(second, 2);
}

// A token is unique within an ELEMENT, not within an element+event. Presenting one with the
// wrong event id must therefore be inert — which is exactly why the ABI's unregister carries
// the event NAME as well as the token. Red under an unregister that assumes one event id.
TEST(GameUIElementHandleTests, UnregisterWithTheWrongEventIdLeavesTheSubscription)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    HudFixture hud(dev.get(), "gameui_handle_evtscope", kOneButtonHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* btn = hud.Find("take-damage");
    ASSERT_NE(btn, nullptr);
    const uint64_t instanceId = btn->GetInstanceId();

    int moves = 0;
    const uint64_t token =
        GameUI::Detail::RegisterSessionEvent(instanceId, kEventMouseMove, [&](const UIEvent&) { ++moves; });
    ASSERT_NE(token, 0u);

    GameUI::Detail::UnregisterSessionEvent(instanceId, kEventButtonClick, token);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*btn, kEventMouseMove), 1u)
        << "a token presented with the wrong event id removed the subscription it names on "
           "another id — handler keys are not unique across event ids";
    EXPECT_TRUE(UIEventHandlerAccess::HandlerHasCallable(*btn, kEventMouseMove, token));

    GameUI::Detail::UnregisterSessionEvent(instanceId, kEventMouseMove, token);
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(*btn, kEventMouseMove, token));
}

// ARM 17 — the element-level events the managed surface exposes really are dispatched to a
// PLAIN element (unlike the click, which only Button raises), and they carry the fields the
// ABI's event struct forwards. Also the pay-per-use half: an event nobody subscribed has no
// handler entry at all, so pointer motion over the element costs nothing. Red the day the
// event surface auto-wires anything.
TEST(GameUIElementHandleTests, PlainElementReceivesPointerEventsOnlyWhenSubscribed)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    constexpr const char* kPlainHud =
        "<uielement class='hud-root'><uielement id='panel' class='btn' /></uielement>\n";
    HudFixture hud(dev.get(), "gameui_handle_pointerevents", kPlainHud);
    ASSERT_TRUE(hud.Ok);

    UIElement* panel = hud.Find("panel");
    ASSERT_NE(panel, nullptr);
    ASSERT_GT(panel->GetLayoutWidth(), 0.0f) << "fixture precondition: it must be hittable";
    const uint64_t instanceId = panel->GetInstanceId();

    // Nobody has subscribed: no entries, and driving the pointer over it changes that.
    hud.Pointer(HudFixture::kOutsideX, HudFixture::kOutsideY, false);
    hud.Pointer(HudFixture::kInsideX, HudFixture::kInsideY, false);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*panel, kEventMouseMove), 0u)
        << "an unsubscribed event has a native handler entry — the surface is auto-wiring";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(*panel, kEventMouseEnter), 0u);

    int enters = 0;
    int moves = 0;
    UIEvent lastMove{};
    ASSERT_NE(GameUI::Detail::RegisterSessionEvent(instanceId, kEventMouseEnter,
                                                  [&](const UIEvent&) { ++enters; }),
              0u);
    ASSERT_NE(GameUI::Detail::RegisterSessionEvent(instanceId, kEventMouseMove,
                                                  [&](const UIEvent& e)
                                                  {
                                                      ++moves;
                                                      lastMove = e;
                                                  }),
              0u);

    // Off, then on: one hover transition and at least one move over the element.
    hud.Pointer(HudFixture::kOutsideX, HudFixture::kOutsideY, false);
    hud.Pointer(HudFixture::kInsideX, HudFixture::kInsideY, false);

    EXPECT_EQ(enters, 1) << "a plain element never saw UI.MouseEnter — Ui.Element.PointerEntered "
                            "would be a subscription that can never fire";
    ASSERT_GT(moves, 0) << "a plain element never saw UI.MouseMove";
    // The three fields the ABI's event struct forwards, at their source.
    EXPECT_EQ(lastMove.CurrentTarget, panel)
        << "CurrentTarget is what the ABI reports as the event's element instance id";
    EXPECT_FLOAT_EQ(lastMove.X, HudFixture::kInsideX);
    EXPECT_FLOAT_EQ(lastMove.Y, HudFixture::kInsideY);
}
