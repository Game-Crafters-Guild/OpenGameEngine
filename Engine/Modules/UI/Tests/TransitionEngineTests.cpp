#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "Rendering/Core/Device.h"
#include "UI/Controls/Button.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

class TransitionEngineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dev = SharedHeadlessDevice();
        if (!m_Dev)
            GTEST_SKIP() << "Device init failed";

        UIRegistration::RegisterBuiltInControls();
    }

    void SetupUI(const std::string& xml, const std::string& css)
    {
        std::unique_ptr<UIElement> root;
        ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

        m_Ui = std::make_unique<UIManager>(m_Dev);
        m_Ui->SetRoot(std::move(root));

        m_CssDir = std::filesystem::temp_directory_path();
        m_CssPath = m_CssDir / "transition_engine_test.css";
        {
            std::ofstream f(m_CssPath);
            f << css;
        }
        ASSERT_TRUE(m_Ui->AttachStyleFromFile(m_CssPath.string()));

        Tick(0.0f, false);
    }

    void Tick(float dt, bool interactive = true)
    {
        m_Ui->Update(dt, interactive);
    }

    void HoverAt(float x, float y)
    {
        m_Ui->OnMouseMove(x, y);
    }

    void HoverOff()
    {
        m_Ui->OnMouseMove(-1000.0f, -1000.0f);
    }

    UIElement* FindById(const std::string& id)
    {
        return m_Ui->GetRootElement()->FindById(id);
    }

    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    IDevice* m_Dev = nullptr;
    std::unique_ptr<UIManager> m_Ui;
    std::filesystem::path m_CssDir;
    std::filesystem::path m_CssPath;
};

// ---------------------------------------------------------------------------
// Opacity transition reaches target over multiple frames
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, OpacityTransitionReachesTarget)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 0.2s linear; }
        #box:hover { opacity: 0.4; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    EXPECT_NEAR(box->GetResolvedStyle().Visual.LocalOpacity, 1.0f, 0.01f);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // Advance enough frames beyond the 0.2s duration.
    constexpr float kStep = 0.016f;
    constexpr int kFrames = 30;
    for (int i = 0; i < kFrames; ++i)
        Tick(kStep);

    float finalOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(finalOpacity, 0.4f, 0.02f)
        << "Opacity should reach hover target after transition completes";
}

// ---------------------------------------------------------------------------
// Intermediate values are interpolated (not jumping to target)
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, IntermediateValuesAreInterpolated)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 1.0s linear; }
        #box:hover { opacity: 0.0; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    // Trigger hover.
    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // Advance to ~50% of the 1.0s transition.
    constexpr float kStep = 0.016f;
    constexpr int kFramesToHalf = 31; // ~0.5s
    for (int i = 0; i < kFramesToHalf; ++i)
        Tick(kStep);

    float midOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_GT(midOpacity, 0.1f) << "Should not have jumped to target yet";
    EXPECT_LT(midOpacity, 0.9f) << "Should have made progress from start";
}

// ---------------------------------------------------------------------------
// Layout transitions update committed Yoga geometry, including ancestor hover
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, PositionTopTransitionUpdatesCommittedGeometry)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='card'>
            <uielement id='card-child' />
        </uielement>
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #card { position: relative; top: 0px; width: 50px; height: 50px;
                transition: top 1.0s linear; }
        #card-child { width: 50px; height: 50px; }
        #card:hover { top: -20px; }
    )";
    SetupUI(xml, css);

    UIElement* card = FindById("card");
    ASSERT_NE(card, nullptr);
    EXPECT_NEAR(card->GetLayoutY(), 0.0f, 0.5f);

    // The child is the hit-test leaf; :hover must still match and animate its
    // ancestor card. At t=0 the committed geometry remains at the start value.
    HoverAt(25.0f, 25.0f);
    Tick(0.0f);
    EXPECT_NEAR(card->GetLayoutY(), 0.0f, 0.5f);

    for (int i = 0; i < 31; ++i)
        Tick(0.016f);

    EXPECT_LT(card->GetLayoutY(), -5.0f);
    EXPECT_GT(card->GetLayoutY(), -15.0f);

    for (int i = 0; i < 40; ++i)
        Tick(0.016f);

    EXPECT_NEAR(card->GetLayoutY(), -20.0f, 0.75f);
}

// ---------------------------------------------------------------------------
// Transition is removed after completion (no lingering slots)
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, TransitionCompletesAndStopsUpdating)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 0.1s linear; }
        #box:hover { opacity: 0.5; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // Run well past the 0.1s duration.
    for (int i = 0; i < 20; ++i)
        Tick(0.016f);

    float afterOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(afterOpacity, 0.5f, 0.02f);

    // Additional frames should keep the value stable at the target.
    for (int i = 0; i < 10; ++i)
        Tick(0.016f);

    float stableOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(stableOpacity, 0.5f, 0.02f)
        << "Value should remain at target after transition completes";
}

// ---------------------------------------------------------------------------
// Retargeting: unhover mid-transition reverses smoothly
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, RetargetOnUnhoverMidTransition)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 1.0s linear; }
        #box:hover { opacity: 0.0; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // Advance ~0.3s into the 1.0s transition.
    for (int i = 0; i < 19; ++i)
        Tick(0.016f);

    float midOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_GT(midOpacity, 0.1f);
    EXPECT_LT(midOpacity, 0.95f);

    // Unhover — should retarget back toward 1.0.
    HoverOff();
    Tick(0.0f);

    // Advance well past transition duration to let retarget complete.
    for (int i = 0; i < 80; ++i)
        Tick(0.016f);

    float finalOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(finalOpacity, 1.0f, 0.02f)
        << "Should return to non-hovered value after retarget completes";
}

// ---------------------------------------------------------------------------
// Color transition reaches target
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, ColorTransitionReachesTarget)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px;
                background-color: #000000;
                transition: background-color 0.2s linear; }
        #box:hover { background-color: #ffffff; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    for (int i = 0; i < 30; ++i)
        Tick(0.016f);

    uint32_t finalColor = box->GetResolvedStyle().Visual.BackgroundColor;

    uint8_t r = (finalColor >> 16) & 0xFF;
    uint8_t g = (finalColor >> 8) & 0xFF;
    uint8_t b = (finalColor >> 0) & 0xFF;

    EXPECT_GE(r, 240) << "Red channel should be near 0xFF";
    EXPECT_GE(g, 240) << "Green channel should be near 0xFF";
    EXPECT_GE(b, 240) << "Blue channel should be near 0xFF";
}

// ---------------------------------------------------------------------------
// "transition: all" handles multiple properties simultaneously
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, TransitionAllHandlesMultipleProperties)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                background-color: #000000;
                transition: all 0.2s linear; }
        #box:hover { opacity: 0.5; background-color: #ffffff; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    for (int i = 0; i < 30; ++i)
        Tick(0.016f);

    float finalOpacity = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(finalOpacity, 0.5f, 0.02f);

    uint32_t finalColor = box->GetResolvedStyle().Visual.BackgroundColor;
    uint8_t r = (finalColor >> 16) & 0xFF;
    EXPECT_GE(r, 240) << "Background color should have reached white";
}

// ---------------------------------------------------------------------------
// Delay holds startValue during delay period (not target)
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, DelayHoldsStartValueDuringDelay)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 0.2s linear 0.3s; }
        #box:hover { opacity: 0.0; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // During the 0.3s delay, opacity should stay at the start value (1.0).
    constexpr float kStep = 0.016f;
    for (int i = 0; i < 10; ++i) // ~0.16s, well within delay
        Tick(kStep);

    float duringDelay = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(duringDelay, 1.0f, 0.02f)
        << "Opacity should hold at start value during delay period";

    // After the delay, the transition should run and reach target.
    for (int i = 0; i < 50; ++i) // run well past delay + duration
        Tick(kStep);

    float afterTransition = box->GetResolvedStyle().Visual.LocalOpacity;
    EXPECT_NEAR(afterTransition, 0.0f, 0.02f)
        << "Opacity should reach target after delay + duration";
}

// ---------------------------------------------------------------------------
// Discrete display transition delays display:none until delay elapses
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, DiscreteDisplayTransitionDelaysNone)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { display: flex; width: 50px; height: 50px;
                transition: display 0s ease 0.25s; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    EXPECT_EQ(box->GetResolvedStyle().Layout.DisplayMode, DisplayMode::Flex);

    // Establish snapshots over a couple of frames.
    Tick(0.016f);
    Tick(0.016f);

    // Set display:none via override — the delayed transition should hold flex.
    box->Overrides().Set(Style::Display, DisplayMode::None);
    box->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
    Tick(0.016f);

    EXPECT_EQ(box->GetResolvedStyle().Layout.DisplayMode, DisplayMode::Flex)
        << "Display should be held at flex during the 0.25s delay";

    // Advance partway through the delay.
    constexpr float kStep = 0.016f;
    for (int i = 0; i < 8; ++i) // ~0.13s
        Tick(kStep);

    EXPECT_EQ(box->GetResolvedStyle().Layout.DisplayMode, DisplayMode::Flex)
        << "Display should still be flex during delay";

    // Advance past the delay.
    for (int i = 0; i < 15; ++i) // ~0.24s more, total ~0.4s
        Tick(kStep);

    EXPECT_EQ(box->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None)
        << "Display should snap to none after delay elapses";
}

// ---------------------------------------------------------------------------
// transitionend event fires when transition completes
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The transition engine and the override classifier take their layout/paint
// classification from GetStylePropertyImpact. Two local copies of that list
// had drifted (line-height and border widths animated without marking layout
// dirty; z-index/overflow overrides relayouted needlessly) — pin the
// classifications those consumers rely on so table edits are deliberate.
// The user-visible half (text reflowing while line-height animates) needs
// real font metrics, which headless tests never resolve.
// ---------------------------------------------------------------------------

static_assert(GetStylePropertyImpact(StylePropertyId::LineHeight).Layout,
              "line-height drives the text measure: transitions must relayout per tick");
static_assert(GetStylePropertyImpact(StylePropertyId::BorderTopWidth).Layout,
              "border widths are classified layout: transition slots mark layout dirty");
static_assert(!GetStylePropertyImpact(StylePropertyId::ZIndex).Layout,
              "z-index is paint-only: overrides must not force relayout");
static_assert(!GetStylePropertyImpact(StylePropertyId::Overflow).Layout &&
                  GetStylePropertyImpact(StylePropertyId::OverflowX).Layout,
              "overflow shorthand is paint-only; the per-axis switches are layout");
static_assert(!GetStylePropertyImpact(StylePropertyId::Opacity).Layout,
              "opacity transitions must not relayout per tick");

TEST_F(TransitionEngineTest, LineHeightTransitionAnimatesResolvedValue)
{
    const std::string xml = R"(<uielement id='root'>
        <label id='text' text='measure me'/>
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 400px; }
        #text { font-size: 16px; line-height: 20px;
                transition: line-height 0.2s linear; }
        #text:hover { line-height: 80px; }
    )";
    SetupUI(xml, css);

    UIElement* text = FindById("text");
    ASSERT_NE(text, nullptr);
    EXPECT_NEAR(text->GetResolvedStyle().Visual.LineHeight, 20.0f, 0.5f);

    HoverAt(10.0f, 5.0f);
    Tick(0.0f);

    // Mid-flight the resolved value interpolates rather than jumping.
    constexpr float kStep = 0.016f;
    for (int i = 0; i < 7; ++i)
        Tick(kStep);
    const float midLineHeight = text->GetResolvedStyle().Visual.LineHeight;
    EXPECT_GT(midLineHeight, 25.0f);
    EXPECT_LT(midLineHeight, 75.0f);

    for (int i = 0; i < 30; ++i)
        Tick(kStep);
    EXPECT_NEAR(text->GetResolvedStyle().Visual.LineHeight, 80.0f, 1.0f);
}

TEST_F(TransitionEngineTest, TransitionEndEventFires)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 200px; height: 200px; }
        #box  { width: 50px; height: 50px; opacity: 1.0;
                transition: opacity 0.1s linear; }
        #box:hover { opacity: 0.5; }
    )";
    SetupUI(xml, css);

    UIElement* box = FindById("box");
    ASSERT_NE(box, nullptr);

    int transitionEndCount = 0;
    StylePropertyId endedProperty = StylePropertyId::Unknown;
    box->RegisterEventHandler(kEventTransitionEnd, [&](UIEvent& e) {
        ++transitionEndCount;
        endedProperty = e.TransitionProperty;
    });

    HoverAt(10.0f, 10.0f);
    Tick(0.0f);

    // Run past the 0.1s transition.
    for (int i = 0; i < 20; ++i)
        Tick(0.016f);

    EXPECT_GE(transitionEndCount, 1)
        << "transitionend should fire when transition completes";
    EXPECT_EQ(endedProperty, StylePropertyId::Opacity)
        << "transitionend should report the property that completed";
}

// ---------------------------------------------------------------------------
// Repro: project-picker main-page card. Mirrors the exact structure of the
// recent/new/open cards in ProjectFolderPickerModal: a high-z modal wrapper,
// a ScrollView, and a Button card whose hover styling (top lift + background)
// comes from CSS rules copied from ProjectPicker.css. The template cards (no
// ScrollView) animate; these must too.
// ---------------------------------------------------------------------------

TEST_F(TransitionEngineTest, ProjectCardHoverAnimatesInsideModalScrollView)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='modal'>
            <uielement id='window'>
                <scrollview id='scroll'>
                    <uielement id='grid'>
                        <button id='card' class='project-card project-card-new'>
                            <uielement id='preview' class='project-card-empty-preview' />
                            <uielement id='body' class='project-card-body'>
                                <label id='title' text='New project' />
                            </uielement>
                            <uielement id='outline' class='project-card-outline' />
                        </button>
                    </uielement>
                </scrollview>
            </uielement>
        </uielement>
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 1100px; height: 800px; }
        #modal { position: absolute; left: 0; top: 0; width: 100%; height: 100%; z-index: 10000; display: flex; }
        #window { display: flex; flex-direction: column; width: 1024px; height: 700px; z-index: 10001; overflow: hidden; }
        #scroll { flex-grow: 1; min-height: 0; }
        .scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
        .scroll-viewport { flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
        .scroll-content { min-width: 0; min-height: 0; }
        #grid { display: flex; flex-direction: row; padding: 20px; }
        #card { position: relative; width: 280px; height: 190px; display: flex; flex-direction: column; }
        #preview { width: 100%; height: 132px; }
        .project-card {
            top: 0;
            background-color: #1f2126;
            border: 0 solid transparent;
            box-shadow: 0 7px 18px #8c000000;
            transition: top 0.16s ease-out, background-color 0.16s ease-out, border-color 0.16s ease-out, box-shadow 0.16s ease-out;
        }
        .project-card:hover {
            top: -3px;
            background-color: #272a30;
            border-color: #5b6270;
            box-shadow: 0 11px 26px #b3000000;
        }
        .project-card-new { background-color: #1f2126; }
        .project-card-new:hover { top: -3px; background-color: #18233d; box-shadow: 0 11px 26px #b3000000; }
        .project-card-empty-preview { background-color: #17191d; transition: background-color 0.16s ease-out; }
        .project-card-new:hover .project-card-empty-preview { background-color: #18233d; }
        .project-card-outline {
            position: absolute; left: 0; top: 0; width: 100%; height: 100%;
            background-color: transparent;
            border: 2px solid #454a55;
            border-radius: 8px;
            pointer-events: none;
            transition: border-color 0.16s ease-out;
        }
        .project-card-new:hover .project-card-outline { border-color: #6f98ff; }
    )";
    SetupUI(xml, css);

    UIElement* card = FindById("card");
    UIElement* preview = FindById("preview");
    ASSERT_NE(card, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_GT(card->GetLayoutWidth(), 0.0f);

    EXPECT_EQ(card->GetResolvedStyle().Visual.BackgroundColor, 0xFF1F2126u);

    // Hover the middle of the card (over the preview child, like a real cursor).
    const float cx = card->GetLayoutX() + card->GetLayoutWidth() * 0.5f;
    const float cy = card->GetLayoutY() + 60.0f;
    HoverAt(cx, cy);
    Tick(0.0f);

    // A few frames in, the background must be mid-transition: neither the
    // idle nor the hover color.
    Tick(0.016f);
    Tick(0.016f);
    Tick(0.016f);
    const uint32_t midColor = card->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_NE(midColor, 0xFF1F2126u) << "hover did not restyle the card at all";
    EXPECT_NE(midColor, 0xFF18233Du) << "hover snapped instantly instead of transitioning";

    for (int i = 0; i < 30; ++i)
        Tick(0.016f);

    EXPECT_EQ(card->GetResolvedStyle().Visual.BackgroundColor, 0xFF18233Du)
        << "card background should settle on the hover color";
    EXPECT_EQ(preview->GetResolvedStyle().Visual.BackgroundColor, 0xFF18233Du)
        << "preview should follow the descendant hover rule";
}

// Same repro but with the editor's exact inline overrides applied to the card
// (ApplyProjectCardFrame in ProjectFolderPickerModal.cpp). Inline overrides
// take precedence over CSS, so this variant catches any interaction where an
// override suppresses the :hover restyle or its transition.
TEST_F(TransitionEngineTest, ProjectCardHoverAnimatesWithInlineFrameOverrides)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='modal'>
            <uielement id='window'>
                <scrollview id='scroll'>
                    <uielement id='grid'>
                        <button id='card' class='project-card project-card-new'>
                            <uielement id='preview' class='project-card-empty-preview' />
                            <uielement id='outline' class='project-card-outline' />
                        </button>
                    </uielement>
                </scrollview>
            </uielement>
        </uielement>
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 1100px; height: 800px; }
        #modal { position: absolute; left: 0; top: 0; width: 100%; height: 100%; z-index: 10000; display: flex; }
        #window { display: flex; flex-direction: column; width: 1024px; height: 700px; z-index: 10001; overflow: hidden; }
        #scroll { flex-grow: 1; min-height: 0; }
        .scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
        .scroll-viewport { flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
        .scroll-content { min-width: 0; min-height: 0; }
        #grid { display: flex; flex-direction: row; padding: 20px; }
        #preview { width: 100%; height: 132px; }
        .project-card {
            top: 0;
            background-color: #1f2126;
            border: 0 solid transparent;
            box-shadow: 0 7px 18px #8c000000;
            transition: top 0.16s ease-out, background-color 0.16s ease-out, border-color 0.16s ease-out, box-shadow 0.16s ease-out;
        }
        .project-card:hover {
            top: -3px;
            background-color: #272a30;
            border-color: #5b6270;
            box-shadow: 0 11px 26px #b3000000;
        }
        .project-card-new { background-color: #1f2126; }
        .project-card-new:hover { top: -3px; background-color: #18233d; box-shadow: 0 11px 26px #b3000000; }
        .project-card-empty-preview { background-color: #17191d; transition: background-color 0.16s ease-out; }
        .project-card-new:hover .project-card-empty-preview { background-color: #18233d; }
        .project-card-outline {
            position: absolute; left: 0; top: 0; width: 100%; height: 100%;
            background-color: transparent;
            border: 2px solid #454a55;
            border-radius: 8px;
            pointer-events: none;
            transition: border-color 0.16s ease-out;
        }
        .project-card-new:hover .project-card-outline { border-color: #6f98ff; }
    )";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    // Mirror ApplyProjectCardFrame + addCardOutline inline overrides.
    UIElement* cardEl = root->FindById("card");
    ASSERT_NE(cardEl, nullptr);
    cardEl->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Width, StyleLength::Px(280.0f))
        .Set(Style::MinWidth, StyleLength::Px(280.0f))
        .Set(Style::MaxWidth, StyleLength::Px(280.0f))
        .Set(Style::Height, StyleLength::Px(190.0f))
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::Position, PositionType::Relative)
        .Set(Style::PaddingTop, StyleLength::Px(0.0f))
        .Set(Style::PaddingRight, StyleLength::Px(0.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(0.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(0.0f))
        .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{8.0f, 8.0f, 8.0f, 8.0f})
        .Set(Style::Cursor, CursorStyle::Pointer);
    UIElement* outlineEl = root->FindById("outline");
    ASSERT_NE(outlineEl, nullptr);
    outlineEl->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::BorderWidth, Box4{2.0f, 2.0f, 2.0f, 2.0f})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{8.0f, 8.0f, 8.0f, 8.0f})
        .Set(Style::PointerEvents, false)
        .Set(Style::ZIndex, 100);

    m_Ui = std::make_unique<UIManager>(m_Dev);
    m_Ui->SetRoot(std::move(root));
    m_CssDir = std::filesystem::temp_directory_path();
    m_CssPath = m_CssDir / "transition_engine_card_inline_test.css";
    {
        std::ofstream f(m_CssPath);
        f << css;
    }
    ASSERT_TRUE(m_Ui->AttachStyleFromFile(m_CssPath.string()));
    Tick(0.0f, false);

    UIElement* card = FindById("card");
    ASSERT_NE(card, nullptr);
    ASSERT_GT(card->GetLayoutWidth(), 0.0f);
    EXPECT_EQ(card->GetResolvedStyle().Visual.BackgroundColor, 0xFF1F2126u);

    const float cx = card->GetLayoutX() + card->GetLayoutWidth() * 0.5f;
    const float cy = card->GetLayoutY() + 60.0f;
    HoverAt(cx, cy);
    Tick(0.0f);
    Tick(0.016f);
    Tick(0.016f);
    Tick(0.016f);
    const uint32_t midColor = card->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_NE(midColor, 0xFF1F2126u) << "hover did not restyle the card at all";
    EXPECT_NE(midColor, 0xFF18233Du) << "hover snapped instantly instead of transitioning";

    for (int i = 0; i < 30; ++i)
        Tick(0.016f);
    EXPECT_EQ(card->GetResolvedStyle().Visual.BackgroundColor, 0xFF18233Du);

    UIElement* outline = FindById("outline");
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetResolvedStyle().Visual.BorderColor.Top, 0xFF6F98FFu)
        << "outline should follow the descendant hover rule";
}

// Variant: the card subtree is created AFTER AttachStyleFromFile and after the
// first Update, exactly like RefreshRecentProjectsList rebuilding the recents
// grid at runtime. Hover styling must still match and animate for dynamically
// added elements.
TEST_F(TransitionEngineTest, ProjectCardHoverAnimatesWhenCardAddedAfterAttach)
{
    const std::string xml = R"(<uielement id='root'>
        <uielement id='modal'>
            <uielement id='window'>
                <scrollview id='scroll'>
                    <uielement id='grid' />
                </scrollview>
            </uielement>
        </uielement>
    </uielement>)";

    const std::string css = R"(
        #root { display: flex; width: 1100px; height: 800px; }
        #modal { position: absolute; left: 0; top: 0; width: 100%; height: 100%; z-index: 10000; display: flex; }
        #window { display: flex; flex-direction: column; width: 1024px; height: 700px; z-index: 10001; overflow: hidden; }
        #scroll { flex-grow: 1; min-height: 0; }
        .scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
        .scroll-viewport { flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
        .scroll-content { min-width: 0; min-height: 0; }
        #grid { display: flex; flex-direction: row; padding: 20px; }
        .project-card {
            top: 0;
            background-color: #1f2126;
            box-shadow: 0 7px 18px #8c000000;
            transition: top 0.16s ease-out, background-color 0.16s ease-out, box-shadow 0.16s ease-out;
        }
        .project-card:hover { top: -3px; background-color: #18233d; box-shadow: 0 11px 26px #b3000000; }
        .project-card-empty-preview { background-color: #17191d; transition: background-color 0.16s ease-out; }
        .project-card:hover .project-card-empty-preview { background-color: #18233d; }
    )";
    SetupUI(xml, css);

    // Now add the card dynamically, like RefreshRecentProjectsList does.
    UIElement* grid = FindById("grid");
    ASSERT_NE(grid, nullptr);
    auto card = std::make_unique<Button>();
    card->SetId("card");
    card->AddClass("project-card");
    card->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Width, StyleLength::Px(280.0f))
        .Set(Style::Height, StyleLength::Px(190.0f))
        .Set(Style::Position, PositionType::Relative);
    auto preview = std::make_unique<UIElement>();
    preview->SetId("preview");
    preview->AddClass("project-card-empty-preview");
    preview->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Px(132.0f));
    card->AddChild(std::move(preview));
    grid->AddChild(std::move(card));

    Tick(0.0f);

    UIElement* cardEl = FindById("card");
    ASSERT_NE(cardEl, nullptr);
    ASSERT_GT(cardEl->GetLayoutWidth(), 0.0f);
    EXPECT_EQ(cardEl->GetResolvedStyle().Visual.BackgroundColor, 0xFF1F2126u);

    const float cx = cardEl->GetLayoutX() + cardEl->GetLayoutWidth() * 0.5f;
    const float cy = cardEl->GetLayoutY() + 60.0f;
    HoverAt(cx, cy);
    Tick(0.0f);
    Tick(0.016f);
    Tick(0.016f);
    Tick(0.016f);
    const uint32_t midColor = cardEl->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_NE(midColor, 0xFF1F2126u) << "hover did not restyle the dynamically added card";
    EXPECT_NE(midColor, 0xFF18233Du) << "hover snapped instantly instead of transitioning";

    for (int i = 0; i < 30; ++i)
        Tick(0.016f);
    EXPECT_EQ(cardEl->GetResolvedStyle().Visual.BackgroundColor, 0xFF18233Du);
}
