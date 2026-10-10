// Debug-server input injection must take the route real input takes.
//
// Injection used to call UIManager directly, which reaches the UI and nothing
// else: the editor InputSystem never saw the pointer, and the runtime input sink
// — what gameplay reads, behind the play-pointer remap — received nothing at
// all. No tool could drive or observe play-mode mouse input, and the remap had
// no automated coverage because nothing could reach it.
//
// These drive the real seam (InjectedInput.cpp) over a real UIManager, real
// InputSystems and the real remap transform. The window is null, so the scale
// conversion is identity and the router's own client->logical step is skipped;
// the scale itself is WindowInputRouter's contract and is covered there.
#include <gtest/gtest.h>

#include "Core/WindowInputRouter.h"
#include "DebugServer/InjectedInput.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Mathematics/Vector2.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <limits>
#include <memory>

using namespace GameEngine;

namespace
{
// A Game View parked away from the window origin, so a remapped coordinate is
// distinguishable from a passthrough one.
constexpr float kViewportLogicalX = 120.0f;
constexpr float kViewportLogicalY = 60.0f;

struct InjectionFixture
{
    std::unique_ptr<Rendering::IDevice> device;
    std::unique_ptr<UIManager> ui;
    Input::InputSystem editorInput;
    Input::InputSystem runtimeInput;
    WindowInputRouterConfig config;

    bool Init()
    {
        device = MakeHeadlessDevice();
        if (!device)
            return false;
        ui = std::make_unique<UIManager>(device.get());
        config.getUi = [this]() { return ui.get(); };
        config.getInput = [this]() { return &editorInput; };
        config.getPlaySurface = [this]() {
            WindowInputRouterConfig::PlaySurface surface;
            surface.gameplaySink = &runtimeInput;
            // The transform the editor's play-surface map runs once it has
            // resolved the Game View. Only the viewport origin is supplied,
            // because resolving the panel needs a docked editor.
            surface.mapGameplayPointer = [this](float clientX, float clientY, float& playX, float& playY) {
                WindowInputRouter::ClientToSurfaceLocal(/*window=*/nullptr, ui.get(), kViewportLogicalX,
                                                        kViewportLogicalY, clientX, clientY, playX, playY);
            };
            return surface;
        };
        return true;
    }
};
} // namespace

TEST(InjectedInputRoutingTests, InjectedMoveReachesEditorInputAndTheRemappedRuntimeSink)
{
    InjectionFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Editor::InjectMouseMove(f.config, /*window=*/nullptr, f.ui.get(), 200.0f, 150.0f);

    // The UI still receives the UI-logical coordinate it always did: the
    // coordinate contract every existing MCP caller depends on is unchanged.
    const Mathematics::Vector2 uiPosition = f.ui->GetMousePosition();
    EXPECT_FLOAT_EQ(uiPosition.x, 200.0f);
    EXPECT_FLOAT_EQ(uiPosition.y, 150.0f);

    // The two sinks injection never used to reach.
    ASSERT_TRUE(f.editorInput.IsPointerInWindow());
    const Mathematics::Vector2 editorPosition = f.editorInput.GetMousePosition();
    EXPECT_FLOAT_EQ(editorPosition.x, 200.0f);
    EXPECT_FLOAT_EQ(editorPosition.y, 150.0f);

    ASSERT_TRUE(f.runtimeInput.IsPointerInWindow());
    const Mathematics::Vector2 playPosition = f.runtimeInput.GetMousePosition();
    EXPECT_FLOAT_EQ(playPosition.x, 200.0f - kViewportLogicalX);
    EXPECT_FLOAT_EQ(playPosition.y, 150.0f - kViewportLogicalY);
}

TEST(InjectedInputRoutingTests, InjectedButtonReachesTheRuntimeSink)
{
    InjectionFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Editor::InjectMouseButton(f.config, f.ui.get(), Input::kMouseButton_Left, /*pressed=*/true);
    f.runtimeInput.Update(0.016f);

    EXPECT_TRUE(f.runtimeInput.IsMouseButtonDown(Input::kMouseButton_Left));
}

TEST(InjectedInputRoutingTests, InjectedClickKeepsAModifierOnlyThePointerMaskReported)
{
    InjectionFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    // A modifier held from before the window took focus is reported by pointer
    // event masks and by no key event, so it has no physical key state backing
    // it. An injected click carries no platform mask; passing zero would derive
    // the state away.
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/true, Input::kModShift);
    WindowInputRouter::RouteMouseButton(f.config, Input::kMouseButton_Left, /*pressed=*/false, Input::kModShift);
    ASSERT_EQ(f.ui->GetModifierKeys(), Input::kModShift);

    Editor::InjectMouseButton(f.config, f.ui.get(), Input::kMouseButton_Left, /*pressed=*/true);

    EXPECT_EQ(f.ui->GetModifierKeys(), Input::kModShift);
}

// Characterization, not a red-green guard: with a null window the direct feed
// and the routed path both deliver to the UI, so removing the unbound-config
// branch would not fail this. It pins the contract that a context wiring its own
// window callbacks (the color picker) stays clickable through injection.
TEST(InjectedInputRoutingTests, UnboundConfigStillFeedsTheUi)
{
    InjectionFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    WindowInputRouterConfig unbound;
    Editor::InjectMouseMove(unbound, /*window=*/nullptr, f.ui.get(), 42.0f, 24.0f);

    const Mathematics::Vector2 uiPosition = f.ui->GetMousePosition();
    EXPECT_FLOAT_EQ(uiPosition.x, 42.0f);
    EXPECT_FLOAT_EQ(uiPosition.y, 24.0f);
}

// The narrowing that makes this reachable from a JSON request: 1e300 is an
// ordinary double, so it parses, and only the cast to float produces infinity.
TEST(InjectedInputRoutingTests, JsonRangeDoubleNarrowsToANonInjectableCoordinate)
{
    EXPECT_TRUE(Editor::IsInjectablePointerPosition(0.0f, 0.0f));
    EXPECT_TRUE(Editor::IsInjectablePointerPosition(-1.0e6f, 1.0e6f));

    EXPECT_FALSE(Editor::IsInjectablePointerPosition(static_cast<float>(1.0e300), 0.0f));
    EXPECT_FALSE(Editor::IsInjectablePointerPosition(0.0f, static_cast<float>(-1.0e300)));
    EXPECT_FALSE(Editor::IsInjectablePointerPosition(std::numeric_limits<float>::quiet_NaN(), 0.0f));
}

TEST(InjectedInputRoutingTests, NonFinitePositionReachesNoSink)
{
    InjectionFixture f;
    if (!f.Init())
    {
        GTEST_SKIP() << "Device init failed";
    }

    Editor::InjectMouseMove(f.config, /*window=*/nullptr, f.ui.get(), 200.0f, 150.0f);

    Editor::InjectMouseMove(f.config, /*window=*/nullptr, f.ui.get(), static_cast<float>(1.0e300), 150.0f);
    Editor::InjectMouseMove(f.config, /*window=*/nullptr, f.ui.get(), 200.0f,
                            std::numeric_limits<float>::quiet_NaN());

    // Every sink still holds the last position that was injectable — the
    // rejected pair reached neither the UI, the editor InputSystem, nor the
    // remapped runtime sink gameplay reads.
    const Mathematics::Vector2 uiPosition = f.ui->GetMousePosition();
    EXPECT_FLOAT_EQ(uiPosition.x, 200.0f);
    EXPECT_FLOAT_EQ(uiPosition.y, 150.0f);

    ASSERT_TRUE(f.editorInput.IsPointerInWindow());
    const Mathematics::Vector2 editorPosition = f.editorInput.GetMousePosition();
    EXPECT_FLOAT_EQ(editorPosition.x, 200.0f);
    EXPECT_FLOAT_EQ(editorPosition.y, 150.0f);

    ASSERT_TRUE(f.runtimeInput.IsPointerInWindow());
    const Mathematics::Vector2 playPosition = f.runtimeInput.GetMousePosition();
    EXPECT_FLOAT_EQ(playPosition.x, 200.0f - kViewportLogicalX);
    EXPECT_FLOAT_EQ(playPosition.y, 150.0f - kViewportLogicalY);
}
