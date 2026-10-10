// Mode-0 idle gate: a frame with no input and nothing pending skips straight
// to the tooltip/cursor tail. These tests pin the gate's take/decline behavior
// and that the work it skips (style resolve, layout, transitions) still lands
// correctly the moment anything changes.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{

std::filesystem::path MakeTempCssPath(const char* prefix)
{
    const auto tmpDir = std::filesystem::temp_directory_path();
    const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const uint64_t tid = (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
    return tmpDir / (std::string(prefix) + "_" + std::to_string(t) + "_" + std::to_string(tid) + ".css");
}

bool LastFrameWasIdle(const UIManager& ui)
{
    UIManager::UpdateProfileFrame f;
    if (!ui.GetLastUpdateProfileFrame(f))
        return false;
    return f.IdleFrame != 0u;
}

// Font streaming, structure-generation snapshots, and stylesheet application
// all settle over the first few heavy frames; run enough Updates that the
// only remaining reason to stay heavy would be a real gate bug.
void ConvergeToIdle(UIManager& ui, int frames = 6)
{
    for (int i = 0; i < frames; ++i)
        ui.Update(0.016f, /*interactive=*/true);
}

// Same convergence, driven through the passive (non-interactive) Update path a
// Player/Game-View HUD uses. The idle gate must engage on a clean passive frame
// exactly as it does interactively — a passive host feeds no input, so nothing
// here should keep it heavy once style/layout have settled.
void ConvergeToIdlePassive(UIManager& ui, int frames = 6)
{
    for (int i = 0; i < frames; ++i)
        ui.Update(0.016f, /*interactive=*/false);
}

} // namespace

TEST(UIIdleGateTests, TakesCleanFramesAndSkipsLayoutWork)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_take");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdle(ui);

    // A converged, untouched tree must be serviced by the idle gate with no
    // build/solve work at all.
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(LastFrameWasIdle(ui));
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateBuildYogaMs(), 0.0);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0);

    // And it stays idle frame after frame.
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(LastFrameWasIdle(ui));
}

TEST(UIIdleGateTests, AKeystrokeDeclinesTheIdleGateSoTheFrameIsStillServiced)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_key");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a { width: 100px; height: 40px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdle(ui);
    ui.Update(0.016f, /*interactive=*/true);
    ASSERT_TRUE(LastFrameWasIdle(ui)) << "precondition: converged to idle";

    // Keys are dispatched at the platform callback, so by the time Update runs
    // there is no queue left to notice. The frame after a keystroke must still
    // be serviced anyway — the gate reads m_KeyInputSinceLastUpdate for exactly
    // this. Lose that wiring and the editor stops repainting while typing,
    // which no other test in the suite would catch.
    ui.OnKey(Input::kKeyCode_A, Input::kKeyActionPress, 0);
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_FALSE(LastFrameWasIdle(ui)) << "a keystroke must decline the idle gate";

    // The flag is retired by the pass that acted on it, so the tree settles.
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(LastFrameWasIdle(ui)) << "and the gate re-engages once serviced";
}

TEST(UIIdleGateTests, DirtyMarkDeclinesGateAndChangeApplies)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_dirty");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a.wide { width: 200px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdle(ui);
    ui.Update(0.016f, /*interactive=*/true);
    ASSERT_TRUE(LastFrameWasIdle(ui));

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NEAR(b->GetLayoutX(), 100.0f, 0.5f);

    // A style-affecting mutation must decline the gate and take effect on
    // that same Update — idling past it would freeze the stale layout.
    a->AddClass("wide");
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_FALSE(LastFrameWasIdle(ui));
    EXPECT_NEAR(b->GetLayoutX(), 200.0f, 0.5f);

    // Once serviced, the tree is clean again and the gate re-engages.
    ConvergeToIdle(ui, 3);
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(LastFrameWasIdle(ui));
    EXPECT_NEAR(b->GetLayoutX(), 200.0f, 0.5f);
}

TEST(UIIdleGateTests, ActiveTransitionsDeclineGateUntilComplete)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_transition");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a { opacity: 1.0; transition: opacity 0.2s linear; }
#a.faded { opacity: 0.2; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdle(ui);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);
    EXPECT_NEAR(a->GetResolvedStyle().Visual.LocalOpacity, 1.0f, 0.01f);

    // Kick off the transition and tick through it. Every frame the
    // transition is active must be heavy — the gate skipping
    // TransitionEngine::Advance would freeze the animation mid-flight.
    a->AddClass("faded");
    bool sawActiveTransitionFrame = false;
    for (int i = 0; i < 30; ++i)
    {
        ui.Update(0.016f, /*interactive=*/true);
        const bool idle = LastFrameWasIdle(ui);
        const float op = a->GetResolvedStyle().Visual.LocalOpacity;
        if (op > 0.22f && op < 0.98f)
        {
            sawActiveTransitionFrame = true;
            EXPECT_FALSE(idle) << "idle-gated frame during an active transition (opacity=" << op << ")";
        }
    }
    EXPECT_TRUE(sawActiveTransitionFrame) << "transition never observed mid-flight";

    // Transition finished: opacity settled at the target and the gate
    // re-engages.
    EXPECT_NEAR(a->GetResolvedStyle().Visual.LocalOpacity, 0.2f, 0.02f);
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(LastFrameWasIdle(ui));
}

TEST(UIIdleGateTests, InheritedColorPropagatesAfterIdleStretch)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='child' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_inherit");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; color: rgb(10, 20, 30); }
#child { width: 100px; height: 40px; }
#root.recolored { color: rgb(200, 50, 25); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdle(ui);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* child = r->FindById("child");
    ASSERT_NE(child, nullptr);

    // ResolveStyles (the inheritance walk) is gated on the resolve-dirty
    // flag; a stretch of idle frames must not leave the child's inherited
    // color stale once the parent changes.
    const uint32_t before = child->GetResolvedStyle().Visual.Color;
    ui.Update(0.016f, /*interactive=*/true);
    ASSERT_TRUE(LastFrameWasIdle(ui));
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_EQ(child->GetResolvedStyle().Visual.Color, before);

    r->AddClass("recolored");
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_FALSE(LastFrameWasIdle(ui));
    const uint32_t after = child->GetResolvedStyle().Visual.Color;
    EXPECT_NE(after, before);
    // Inherited from the parent's new resolved color.
    EXPECT_EQ(after, r->GetResolvedStyle().Visual.Color);
}

// A passive host (Player/Game-View HUD, editor background tool window) drives
// Update with interactive=false. Before the gate was made host-agnostic these
// frames NEVER idled — they paid the full build/cascade/solve every frame even
// when the tree was completely clean. This pins the passive path onto the same
// idle early-out and proves a dirty change still declines + applies passively.
TEST(UIIdleGateTests, PassiveHostTakesCleanFramesAndAppliesDirtyChanges)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_passive");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a.wide { width: 200px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdlePassive(ui);

    // A converged, untouched tree serviced through the passive path must be
    // idle-gated with no build/solve work — the whole point of the fix.
    ui.Update(0.016f, /*interactive=*/false);
    EXPECT_TRUE(LastFrameWasIdle(ui));
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateBuildYogaMs(), 0.0);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NEAR(b->GetLayoutX(), 100.0f, 0.5f);

    // A style-affecting mutation must decline the passive gate and take effect
    // on that same passive Update — layout still solves without interactivity.
    a->AddClass("wide");
    ui.Update(0.016f, /*interactive=*/false);
    EXPECT_FALSE(LastFrameWasIdle(ui));
    EXPECT_NEAR(b->GetLayoutX(), 200.0f, 0.5f);

    // Serviced → clean again → the passive gate re-engages.
    ConvergeToIdlePassive(ui, 3);
    ui.Update(0.016f, /*interactive=*/false);
    EXPECT_TRUE(LastFrameWasIdle(ui));
    EXPECT_NEAR(b->GetLayoutX(), 200.0f, 0.5f);
}

// A passive HUD with a CSS transition must keep animating: the idle gate has to
// decline every frame the transition is active (TransitionEngine::Advance runs
// in the passive heavy pass), then re-engage once it settles. Guards against a
// regression where idle-gating a passive frame would freeze the animation.
TEST(UIIdleGateTests, PassiveHostActiveTransitionsDeclineGateUntilComplete)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev.get());
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_idle_passive_transition");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a { opacity: 1.0; transition: opacity 0.2s linear; }
#a.faded { opacity: 0.2; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ConvergeToIdlePassive(ui);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);
    EXPECT_NEAR(a->GetResolvedStyle().Visual.LocalOpacity, 1.0f, 0.01f);

    a->AddClass("faded");
    bool sawActiveTransitionFrame = false;
    for (int i = 0; i < 30; ++i)
    {
        ui.Update(0.016f, /*interactive=*/false);
        const bool idle = LastFrameWasIdle(ui);
        const float op = a->GetResolvedStyle().Visual.LocalOpacity;
        if (op > 0.22f && op < 0.98f)
        {
            sawActiveTransitionFrame = true;
            EXPECT_FALSE(idle) << "passive idle-gated frame during an active transition (opacity=" << op << ")";
        }
    }
    EXPECT_TRUE(sawActiveTransitionFrame) << "transition never observed mid-flight on the passive path";

    EXPECT_NEAR(a->GetResolvedStyle().Visual.LocalOpacity, 0.2f, 0.02f);
    ui.Update(0.016f, /*interactive=*/false);
    EXPECT_TRUE(LastFrameWasIdle(ui));
}
