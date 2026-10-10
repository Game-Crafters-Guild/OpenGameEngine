// GitHub #788 — the caret's "hold solid until" deadline is re-derived from the
// CURRENT frame time every time the caret is emitted, so it can never be
// overtaken by the clock it is compared against.
//
// The shader gate is `if (pc.timeSeconds >= prim.CaretTime) { ...blink... }`
// (ui_sdf.frag). Both multi-line controls stamped
// `prim.CaretTime = manager->GetTimeSeconds() + <window>` from inside their
// primitive emit, which evaluates the gate as `t >= t + window` — false on every
// frame, for ever. The blink branch is unreachable and the caret is permanently
// solid.
//
// WHY THIS NEEDS A REPAINT-DRIVEN TEST AND NOT A UNIT ONE. CaretBlinkTests.cpp
// pins the timing arithmetic — the rate, the window, the phase boundary — and
// every one of those tests passes with this defect live, because none of them
// drives a control. The defect is not in the numbers; it is in WHEN the number
// is written. So the assertion here has to be about a deadline observed across
// repaints, and the fixture has to actually produce repaints: a caret whose
// deadline is re-armed by the repaint loop looks identical to a correct one in
// any test that does not pump frames.
//
// THE INSTRUMENT IS THE SECOND TEST, and it is load-bearing. The specimen below
// asserts that the deadline does NOT move while the caret is only being
// redrawn. That assertion also passes if the caret simply stopped being emitted
// — which would be a worse bug wearing a green tick. `AKeystrokeStampsANewDeadline`
// proves emission is live by showing an input-driven change reach the primitive,
// so the pair cannot both pass on a dead emit path.
//
// SCOPE. TextArea is covered here. ScriptTextArea carries the identical defect
// and the identical fix, but it lives in Apps/Editor, which has no UI test
// target reachable from this suite (TextAreaContentScaleTests.cpp:71 states the
// same limit for #733). Its half of the fix is verified by inspection only, and
// that is a stated gap rather than a silent one.

#include "IsolatedUIFixture.h"

#include "Platform/SystemMetrics.h"
#include "UI/CaretBlink.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <textarea id="area" value="ab"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 200px; }
#area {
  width: 300px;
  height: 80px;
  font-family: Roboto;
  font-size: 16px;
  padding: 0px;
  border-width: 0px;
  background-color: #272727;
  color: #ffffff;
}
)";

// The caret is flagged in the primitive itself (kPrimCaretBit), so it is found
// by what it IS rather than by guessing at its geometry.
std::optional<UIPrimitive> FindCaret(const IsolatedUIFixture& fx, const std::string& id)
{
    for (const UIPrimitive& p : fx.Primitives(id))
    {
        if ((p.ModeAndFlags & GameEngine::UI::kPrimCaretBit) != 0u)
            return p;
    }
    return std::nullopt;
}

// The window the control is entitled to hold the caret solid for, read from the
// same OS setting the production path reads. On a machine set to "never blink"
// this is 0, and the deadline should never sit ahead of the clock at all.
float ForceVisibleWindow()
{
    return GameEngine::UI::CaretForceVisibleSeconds(GameEngine::Platform::GetCaretBlinkHalfPeriod());
}

#define REQUIRE_FOCUSED_AREA(fx)                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(fx).Build(1.0f, kXml, kCss))                                                         \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
        ASSERT_TRUE((fx).FocusViaClick("area"));                                                   \
        ASSERT_EQ((fx).Manager().GetFocusedElementId(), "area");                                   \
    } while (false)

} // namespace

// The specimen. No input after the click that focused the control; only
// repaints. The deadline stamped by that click must stay where it was put, and
// the clock must overtake it.
TEST(CaretForceVisibleDeadline, RepaintsDoNotPushTheDeadlineOut)
{
    IsolatedUIFixture fx;
    REQUIRE_FOCUSED_AREA(fx);

    // The baseline is taken AFTER one forced repaint, not straight out of the
    // click. Focusing is two inputs, not one — the pointer press, and the
    // focus-gain dispatched a frame or two later — and the second stamps the
    // deadline without marking the control dirty, so the primitive still in the
    // snapshot carries the FIRST input's value. Sampling there would measure
    // that catch-up rather than the property under test. It does not soften the
    // test: the defect re-derived the deadline on every emit, so the 30 repaints
    // below still move it by their whole elapsed span.
    fx.Element("area")->MarkDirty(GameEngine::UIElement::VisualDirty);
    fx.PumpSeconds(0.0f);

    const auto first = FindCaret(fx, "area");
    ASSERT_TRUE(first.has_value()) << "a focused text area must emit a caret primitive";
    const float stamped = first->CaretTime;

    // Repaint past the whole force-visible window with no input at all. The
    // element is marked visually dirty each frame so a repaint genuinely
    // arrives — that is the editor's steady state, and the condition under
    // which the defect manifests.
    const float window = ForceVisibleWindow();
    const float total = window + 1.0f;
    constexpr int kFrames = 30;
    for (int i = 0; i < kFrames; ++i)
    {
        fx.Element("area")->MarkDirty(GameEngine::UIElement::VisualDirty);
        fx.PumpSeconds(total / static_cast<float>(kFrames));
    }

    const auto later = FindCaret(fx, "area");
    ASSERT_TRUE(later.has_value()) << "the caret must still be emitted after the repaints";

    EXPECT_FLOAT_EQ(later->CaretTime, stamped)
        << "the force-visible deadline is stamped by the input that moves the caret and must "
           "not be re-derived while the caret is merely being drawn";

    const float now = fx.Manager().GetTimeSeconds();
    EXPECT_LE(later->CaretTime, now)
        << "with the window elapsed the clock must have overtaken the deadline, or the shader's "
           "blink branch is unreachable and the caret is solid for ever";
}

// The instrument for the specimen, and the behaviour the deadline exists for: an
// input that moves the caret puts it back in front of the clock. This also
// proves the emit path is live — a dead one could not carry the new value.
TEST(CaretForceVisibleDeadline, AKeystrokeStampsANewDeadline)
{
    IsolatedUIFixture fx;
    REQUIRE_FOCUSED_AREA(fx);

    const float window = ForceVisibleWindow();
    constexpr int kFrames = 30;
    for (int i = 0; i < kFrames; ++i)
    {
        fx.Element("area")->MarkDirty(GameEngine::UIElement::VisualDirty);
        fx.PumpSeconds((window + 1.0f) / static_cast<float>(kFrames));
    }

    // By here the click's window has long elapsed, so the deadline sits behind
    // the clock and any forward movement below can only have come from the
    // keystroke.
    const auto beforeTyping = FindCaret(fx, "area");
    ASSERT_TRUE(beforeTyping.has_value());

    fx.TypeChar('c');

    const auto afterTyping = FindCaret(fx, "area");
    ASSERT_TRUE(afterTyping.has_value()) << "the caret must survive typing";
    EXPECT_GT(afterTyping->CaretTime, beforeTyping->CaretTime)
        << "typing must push the force-visible deadline forward, or the caret can blink out "
           "mid-keystroke";

    // On a machine configured to blink, the fresh deadline is ahead of the
    // clock; on one set to "never blink" the window is zero and there is
    // nothing to hold. Both are correct, and the assertion says which is which
    // rather than assuming this machine's setting.
    const float now = fx.Manager().GetTimeSeconds();
    if (window > 0.0f)
        EXPECT_GT(afterTyping->CaretTime, now) << "a keystroke must hold the caret solid";
    else
        EXPECT_LE(afterTyping->CaretTime, now) << "with no blink there is no window to hold";
}
