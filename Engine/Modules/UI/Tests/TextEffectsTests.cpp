#include "UI/GlyphRunEmitter.h"
#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"
#include "UI/UITargetSpace.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
constexpr int kWidth = static_cast<int>(UITesting::kReadbackW);
constexpr int kHeight = static_cast<int>(UITesting::kReadbackH);
constexpr int kRed = 0;
constexpr int kGreen = 1;
constexpr int kBlue = 2;

// Channel value in [0, 1]; zero outside the readback.
double Channel(const std::vector<uint8_t>& pixels, int x, int y, int channel)
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
        return 0.0;
    return pixels[(static_cast<size_t>(y) * kWidth + x) * 4 + channel] / 255.0;
}

struct EffectRender
{
    bool DeviceAvailable = false;
    std::string Diagnostic;
    std::vector<uint8_t> Pixels;
};

// One white 48px label at (40, 40) on black with `effectCss` applied. Text
// coverage correction is off, so the fill's green channel is the glyph's
// coverage and an effect painted in pure red reads as its own coverage.
EffectRender RenderEffectSample(const std::string& text, const std::string& effectCss)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        R"(<uielement id="root"><label id="text">)" + text + "</label></uielement>",
        "#root { width: 400px; height: 200px; background-color: #000000; } "
        "#text { margin: 40px; width: 160px; height: 80px; font-size: 48px; color: #ffffff; " +
            effectCss + " }");
    EffectRender render;
    render.DeviceAvailable = fixture.DeviceAvailable();
    render.Diagnostic = fixture.Diagnostic();
    if (!built)
        return render;
    fixture.Manager().SetTextContrast(0);
    fixture.Manager().SetTextBlendGamma(1);
    fixture.Manager().SetTextSmoothingGamma(1);
    fixture.Manager().SetTextSubpixelAA(false);
    render.Pixels = UITesting::RenderUiToBytes(fixture.Manager(), UI::UITargetSpace::EncodedSrgb());
    return render;
}

// Largest glyph coverage within `radius` whole pixels of (x, y).
double MaskWithin(const std::vector<uint8_t>& pixels, int x, int y, int radius)
{
    double result = 0.0;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            if (dx * dx + dy * dy <= radius * radius)
                result = std::max(result, Channel(pixels, x + dx, y + dy, kGreen));
    return result;
}

// A vertical stem's right edge on its middle row: the row, and the edge's x in
// pixel-centre coordinates where the fill coverage crosses one half.
struct StemEdge
{
    int Row = -1;
    double X = 0.0;
};

StemEdge FindStemRightEdge(const std::vector<uint8_t>& pixels)
{
    int top = kHeight, bottom = -1;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth / 2; ++x)
            if (Channel(pixels, x, y, kGreen) > 0.5)
            {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    StemEdge edge;
    if (bottom < top)
        return edge;
    edge.Row = (top + bottom) / 2;
    for (int x = kWidth / 2; x > 0; --x)
    {
        const double inside = Channel(pixels, x - 1, edge.Row, kGreen);
        const double outside = Channel(pixels, x, edge.Row, kGreen);
        if (inside >= 0.5 && outside < 0.5)
        {
            edge.X = (x - 1) + (inside - 0.5) / (inside - outside);
            return edge;
        }
    }
    edge.Row = -1;
    return edge;
}
} // namespace

// A run with effects paints in layers over the whole run: a shadow-and-glow
// instance per glyph (keeping the outline width it starts from, without the
// outline color), then an outline instance per glyph, then the plain fills,
// which carry no effect fields and draw as plain text does. An invisible
// effect leaves its fields zero.
TEST(TextEffects, EffectLayersOfTheWholeRunPrecedeItsPlainFills)
{
    std::array<Rendering::Text::FontAtlas::GlyphPlacement, 2> glyphs{};
    for (auto& glyph : glyphs)
    {
        glyph.width = 12;
        glyph.height = 20;
        glyph.hBandCount = glyph.vBandCount = 1;
        glyph.color = 0xffffffffu;
    }
    glyphs[1].x = 10;
    std::vector<UI::UIPrimitive> primitives;
    UI::GlyphRunTarget target;
    target.Primitives = &primitives;
    target.Effects.ShadowColor = 0xff000000u;
    target.Effects.ShadowOffsetX = 3;
    target.Effects.ShadowBlur = 2;
    target.Effects.GlowColor = 0x00ffffffu; // Transparent: no glow.
    target.Effects.GlowRadius = 5;
    target.Effects.OutlineColor = 0xff00ff00u;
    target.Effects.OutlineWidth = 2;
    UI::EmitGlyphRun(glyphs, 0, 0, 16, target);
    ASSERT_EQ(primitives.size(), 6u);
    for (size_t i = 0; i < primitives.size(); ++i)
    {
        SCOPED_TRACE("primitive " + std::to_string(i));
        const UI::UIPrimitive& glyph = primitives[i];
        const bool shadow = i < 2;
        const bool outline = i >= 2 && i < 4;
        EXPECT_FLOAT_EQ(glyph.X, i % 2 == 0 ? 0.0f : 10.0f);
        EXPECT_EQ(glyph.FillColor, 0xffffffffu);
        EXPECT_EQ(glyph.ShadowColor, shadow ? 0xff000000u : 0u);
        EXPECT_FLOAT_EQ(glyph.ShadowOffsetX, shadow ? 3.0f : 0.0f);
        EXPECT_FLOAT_EQ(glyph.ShadowOffsetY, 0);
        EXPECT_FLOAT_EQ(glyph.ShadowSoftness, shadow ? 2.0f : 0.0f);
        EXPECT_EQ(glyph.GlowColor, 0u);
        EXPECT_FLOAT_EQ(glyph.GlowRadius, 0);
        EXPECT_EQ(glyph.BorderColor, outline ? 0xff00ff00u : 0u);
        EXPECT_FLOAT_EQ(glyph.RadiiY[0], shadow || outline ? 2.0f : 0.0f);
    }
}

// Every effect length widens every glyph quad of the run, so resolution bounds
// them: extents to 64 device pixels, offsets to 256 either way, after the scale.
TEST(TextEffects, ResolutionBoundsEveryLengthInDevicePixels)
{
    UI::TextEffects authored;
    authored.ShadowOffsetX = 1.0e6f;
    authored.ShadowOffsetY = -1.0e6f;
    authored.ShadowBlur = 1.0e6f;
    authored.GlowRadius = 1.0e6f;
    authored.OutlineWidth = 1.0e6f;
    const UI::TextEffects huge = UI::ResolveTextEffects(authored, 2.0f);
    EXPECT_FLOAT_EQ(huge.ShadowOffsetX, 256.0f);
    EXPECT_FLOAT_EQ(huge.ShadowOffsetY, -256.0f);
    EXPECT_FLOAT_EQ(huge.ShadowBlur, 64.0f);
    EXPECT_FLOAT_EQ(huge.GlowRadius, 64.0f);
    EXPECT_FLOAT_EQ(huge.OutlineWidth, 64.0f);

    authored.ShadowOffsetX = 100.0f;
    authored.ShadowOffsetY = -3.0f;
    authored.ShadowBlur = 4.0f;
    authored.GlowRadius = 6.0f;
    authored.OutlineWidth = 2.0f;
    const UI::TextEffects scaled = UI::ResolveTextEffects(authored, 2.0f);
    EXPECT_FLOAT_EQ(scaled.ShadowOffsetX, 200.0f);
    EXPECT_FLOAT_EQ(scaled.ShadowOffsetY, -6.0f);
    EXPECT_FLOAT_EQ(scaled.ShadowBlur, 8.0f);
    EXPECT_FLOAT_EQ(scaled.GlowRadius, 12.0f);
    EXPECT_FLOAT_EQ(scaled.OutlineWidth, 4.0f);
}

// A length or scale that is not a finite number resolves to no effect rather
// than a quad of unbounded size.
TEST(TextEffects, ResolutionDropsNonFiniteLengthsAndScales)
{
    UI::TextEffects authored;
    authored.ShadowOffsetX = std::numeric_limits<float>::infinity();
    authored.ShadowOffsetY = std::numeric_limits<float>::quiet_NaN();
    authored.ShadowBlur = std::numeric_limits<float>::quiet_NaN();
    authored.GlowRadius = std::numeric_limits<float>::infinity();
    authored.OutlineWidth = -std::numeric_limits<float>::infinity();
    const UI::TextEffects resolved = UI::ResolveTextEffects(authored, 1.0f);
    EXPECT_FLOAT_EQ(resolved.ShadowOffsetX, 0.0f);
    EXPECT_FLOAT_EQ(resolved.ShadowOffsetY, 0.0f);
    EXPECT_FLOAT_EQ(resolved.ShadowBlur, 0.0f);
    EXPECT_FLOAT_EQ(resolved.GlowRadius, 0.0f);
    EXPECT_FLOAT_EQ(resolved.OutlineWidth, 0.0f);

    UI::TextEffects plain;
    plain.OutlineWidth = 3.0f;
    for (const float scale : {0.0f, -2.0f, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity()})
        EXPECT_FLOAT_EQ(UI::ResolveTextEffects(plain, scale).OutlineWidth, 3.0f) << "scale " << scale;
}

TEST(TextEffects, AuthoredEffectsInheritAndInitialClearsOneProperty)
{
    UITesting::IsolatedUIFixture fixture;
    bool built = fixture.Build(1.0f,
        R"(<uielement id="root"><label id="inherited">日本語</label><label id="reset">العربية</label></uielement>)",
        R"(#root { width: 400px; height: 200px; text-shadow: 3px 0px 0px #102030;
                   text-outline: 2px #405060; }
            #reset { text-outline-width: initial; })");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    const auto& inherited = fixture.Element("inherited")->GetResolvedStyle().Visual.TextEffects;
    EXPECT_FLOAT_EQ(inherited.ShadowOffsetX, 3);
    EXPECT_EQ(inherited.ShadowColor, 0xff102030u);
    EXPECT_FLOAT_EQ(inherited.OutlineWidth, 2);
    const auto& reset = fixture.Element("reset")->GetResolvedStyle().Visual.TextEffects;
    EXPECT_FLOAT_EQ(reset.OutlineWidth, 0);
    EXPECT_EQ(reset.OutlineColor, 0xff405060u);
}

// The reach is the glyph-quad growth of uiTextEffectReachPx: an outline its
// width plus the one-pixel ramp, a glow the outline width plus twice its
// radius, a shadow its larger offset plus the outline width, 1.5 x its blur
// and the ramp; the largest wins, and an invisible effect reaches nothing.
TEST(TextEffects, ReachIsTheGlyphQuadGrowthOfTheVisibleEffects)
{
    UI::TextEffects effects;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 0.0f);
    effects.GlowRadius = 6;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 0.0f) << "a transparent glow reaches nothing";
    effects.GlowColor = 0xff0000ffu;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 12.0f);
    effects.OutlineWidth = 2;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 12.0f) << "an invisible outline moves nothing";
    effects.OutlineColor = 0xff00ff00u;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 14.0f);
    effects.ShadowColor = 0xff000000u;
    effects.ShadowOffsetX = 3;
    effects.ShadowOffsetY = -9;
    effects.ShadowBlur = 4;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(effects), 9.0f + 2.0f + 6.0f + 1.0f);
    UI::TextEffects outline;
    outline.OutlineColor = 0xff00ff00u;
    outline.OutlineWidth = 2;
    EXPECT_FLOAT_EQ(UI::TextEffectReachPx(outline), 3.0f);
}

namespace
{
// The persistent clip rect an element's own slot holds; null without a slot.
const UI::UIClipRect* OwnClipRect(const UITesting::IsolatedUIFixture& fixture, const std::string& id)
{
    const UIElement* el = fixture.Element(id);
    if (!el || el->m_ClipSlotIdx == UI::kNoClip)
        return nullptr;
    return fixture.Manager().PeekClipRectForTesting(el->m_ClipSlotIdx);
}

// The element's layout box grown by `outset` on every side, physical px at
// content scale 1, against the clip rect its slot holds.
void ExpectClipIsBoxGrownBy(const UITesting::IsolatedUIFixture& fixture, const std::string& id, float outset)
{
    SCOPED_TRACE(id);
    const UIElement* el = fixture.Element(id);
    ASSERT_NE(el, nullptr);
    const UI::UIClipRect* clip = OwnClipRect(fixture, id);
    ASSERT_NE(clip, nullptr) << "the element must own a clip slot";
    EXPECT_FLOAT_EQ(clip->Rect[0], el->GetLayoutX() - outset);
    EXPECT_FLOAT_EQ(clip->Rect[1], el->GetLayoutY() - outset);
    EXPECT_FLOAT_EQ(clip->Rect[2], el->GetLayoutWidth() + 2.0f * outset);
    EXPECT_FLOAT_EQ(clip->Rect[3], el->GetLayoutHeight() + 2.0f * outset);
}

// Labels in a 20px row, as in a list or tree: clipped, single-line, and
// ellipsized unless a rule says otherwise. A 6px glow reaches 12px.
constexpr const char* kClippedRowCss = R"(
    #root { width: 400px; height: 600px; display: flex; flex-direction: column; }
    label { margin: 30px 30px 0px 30px; width: 120px; height: 20px; font-size: 13px;
            overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
    .glow { text-glow: 6px #ff0000; }
)";
constexpr const char* kLongText = "Distant Lighthouse Beacon Keeper Quarters";
constexpr float kGlowReachPx = 12.0f;

// An element that draws its own text through the shared text path and, unlike
// a Label, has no text measure, so it can hold children.
class TextPresentingElement : public UIElement
{
public:
    explicit TextPresentingElement(std::string text) : m_Text(std::move(text)) {}
    const std::string& GetTextContent() const override { return m_Text; }

private:
    std::string m_Text;
};
} // namespace

// The owner's report (#3008): a glowing label in a fixed-height row had its glow
// cut flat by its own overflow clip. The clip grows by the effect's reach, for
// text that fits and for text an ellipsis ends inside the box; plain text keeps
// the padding box exactly, so every element without an effect is unchanged.
TEST(TextEffects, OwnClipGrowsByTheEffectReachWhileTheTextFits)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        std::string(R"(<uielement id="root">)") +
            R"(<label id="fits" class="glow">Sphere</label>)" +
            R"(<label id="ellipsized" class="glow">)" + kLongText + "</label>" +
            R"(<label id="plain">)" + kLongText + "</label>" +
            "</uielement>",
        kClippedRowCss);
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();

    ExpectClipIsBoxGrownBy(fixture, "fits", kGlowReachPx);
    ExpectClipIsBoxGrownBy(fixture, "ellipsized", kGlowReachPx);
    ExpectClipIsBoxGrownBy(fixture, "plain", 0.0f);

    // Every layer of the run wears the grown slot: the glow and the fills.
    const UIElement* fits = fixture.Element("fits");
    const auto glyphs = fixture.Primitives("fits", UI::PrimitiveMode::Slug);
    ASSERT_FALSE(glyphs.empty());
    for (const auto& glyph : glyphs)
        EXPECT_EQ(UI::GetClipIndex(glyph.ModeAndFlags), fits->m_ClipSlotIdx);
}

// Text the box cuts keeps the CSS overflow edge, and its effects are cut with
// it: growing the clip would show the cut glyphs past the box.
TEST(TextEffects, CutTextKeepsItsEffectsInsideThePaddingBox)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        std::string(R"(<uielement id="root"><label id="cut" class="glow">)") + kLongText +
            "</label></uielement>",
        std::string(kClippedRowCss) + "#cut { text-overflow: clip; }");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    ExpectClipIsBoxGrownBy(fixture, "cut", 0.0f);
}

// The grown clip intersects the parent's: a tight clipping parent cuts the
// effect, the label's own box does not. Rounded corners grow with the rect.
TEST(TextEffects, ParentClipBoundsTheGrownClip)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        R"(<uielement id="root"><uielement id="row"><label id="title" class="glow">Sphere</label></uielement></uielement>)",
        std::string(kClippedRowCss) +
            "#row { margin: 40px; width: 300px; height: 20px; overflow: hidden; }"
            "#title { margin: 0px 0px 0px 40px; border-radius: 4px; }");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    const UIElement* row = fixture.Element("row");
    const UIElement* title = fixture.Element("title");
    const UI::UIClipRect* clip = OwnClipRect(fixture, "title");
    ASSERT_NE(clip, nullptr);
    EXPECT_FLOAT_EQ(clip->Rect[0], title->GetLayoutX() - kGlowReachPx) << "the row leaves room on the left";
    EXPECT_FLOAT_EQ(clip->Rect[2], title->GetLayoutWidth() + 2.0f * kGlowReachPx);
    EXPECT_FLOAT_EQ(clip->Rect[1], row->GetLayoutY()) << "the row cuts the glow, at its own edge";
    EXPECT_FLOAT_EQ(clip->Rect[3], row->GetLayoutHeight());
    EXPECT_EQ(clip->ParentIndex, row->m_ClipSlotIdx);
    for (int corner = 0; corner < 4; ++corner)
    {
        EXPECT_FLOAT_EQ(clip->Radii[corner], 4.0f + kGlowReachPx) << "corner " << corner;
        EXPECT_FLOAT_EQ(clip->RadiiY[corner], 4.0f + kGlowReachPx) << "corner " << corner;
    }
}

// A visual-only drain rewrites every owned clip slot in its collect phase, and
// a worker that then emits the element reuses that rect as written. The
// rewrite must reproduce each shape from the last emission's cut verdict:
// the grown rect for an ellipsized glowing label, or the first hover after a
// regen cuts the glow flat again; the padding box for a label whose text the
// box cuts, or the drain shows its cut glyphs past the box. Twelve labels of
// each kind and a two-worker pool fork the emit phase, so every label emits
// off the UI thread.
TEST(TextEffects, OffThreadDrainKeepsEachClipShape)
{
    JobSystem::WorkStealingThreadPool pool(2);
    UITesting::IsolatedUIFixture fixture;
    constexpr int kLabelsPerKind = 12;
    const auto grownId = [](int i) { return "g" + std::to_string(i); };
    const auto cutId = [](int i) { return "c" + std::to_string(i); };
    std::string xml = R"(<uielement id="root">)";
    // 24 rows of 4 + 20 px fill 576 of the fixture's 600 px viewport without
    // shrinking, and every grown rect (12 px past its row) stays inside it.
    std::string css = R"(#root { width: 400px; height: 600px; padding-top: 10px;
                display: flex; flex-direction: column; }
        label { margin: 4px 30px 0px 30px; width: 120px; height: 20px; flex-shrink: 0; font-size: 13px;
                overflow: hidden; white-space: nowrap; text-overflow: ellipsis;
                text-glow: 6px #ff0000; }
        .cut { text-overflow: clip; })";
    for (int i = 0; i < kLabelsPerKind; ++i)
    {
        xml += "<label id=\"" + grownId(i) + "\">" + kLongText + "</label>";
        xml += "<label id=\"" + cutId(i) + "\" class=\"cut\">" + kLongText + "</label>";
    }
    xml += "</uielement>";
    const bool built = fixture.Build(1.0f, xml, css);
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    fixture.Manager().SetJobSystem(&pool);
    fixture.Manager().SetUpdateProfilingEnabled(true);
    for (int i = 0; i < kLabelsPerKind; ++i)
    {
        ExpectClipIsBoxGrownBy(fixture, grownId(i), kGlowReachPx);
        ExpectClipIsBoxGrownBy(fixture, cutId(i), 0.0f);
    }

    for (int i = 0; i < kLabelsPerKind; ++i)
    {
        fixture.Element(grownId(i))->MarkDirty(UIElement::VisualDirty);
        fixture.Element(cutId(i))->MarkDirty(UIElement::VisualDirty);
    }
    fixture.StepFrame();

    const auto& history = fixture.Manager().GetUpdateProfilingHistory();
    const auto drained = std::find_if(history.rbegin(), history.rend(),
        [](const UIManager::UpdateProfileFrame& frame) { return frame.DrainItems >= 2 * kLabelsPerKind; });
    ASSERT_NE(drained, history.rend()) << "the marks must reach the drain, not a full regen";
    ASSERT_GE(drained->DrainParallelTasks, 2u) << "the emit phase must fork onto workers";
    ASSERT_EQ(drained->DrainEscalated, 0u) << "an escalated item re-pushes its clip on the UI thread";
    for (int i = 0; i < kLabelsPerKind; ++i)
    {
        ExpectClipIsBoxGrownBy(fixture, grownId(i), kGlowReachPx);
        ExpectClipIsBoxGrownBy(fixture, cutId(i), 0.0f);
    }
}

// An element whose overflow clip also clips its children keeps the padding
// box for them, its own glowing text included: after the build, where the
// children's push writes the slot last, and after a drain, where the collect
// rewrite and the element's own text push are the only writers.
TEST(TextEffects, ClipThatAlsoClipsChildrenKeepsThePaddingBox)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, R"(<uielement id="root"/>)",
        std::string(kClippedRowCss) +
            "#parent { margin: 30px; width: 120px; height: 20px; font-size: 13px; overflow: hidden;"
            " white-space: nowrap; text-glow: 6px #ff0000; }"
            "#kid { width: 10px; height: 10px; }");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto parent = std::make_unique<TextPresentingElement>("Sphere");
    parent->SetId("parent");
    auto kid = std::make_unique<UIElement>();
    kid->SetId("kid");
    parent->AddChild(std::move(kid));
    fixture.Element("root")->AddChild(std::move(parent));
    fixture.Settle();
    ASSERT_FALSE(fixture.Primitives("parent", UI::PrimitiveMode::Slug).empty()) << "the parent draws its text";
    ExpectClipIsBoxGrownBy(fixture, "parent", 0.0f);

    fixture.Element("parent")->MarkDirty(UIElement::VisualDirty);
    fixture.StepFrame();
    ExpectClipIsBoxGrownBy(fixture, "parent", 0.0f);
}

TEST(TextEffects, ShaderProducesColoredShadowGlowAndOutlinePixels)
{
    const std::array<const char*, 3> effects = {
        "text-shadow: 8px 4px 1px #ff0000;",
        "text-glow: 2px #00ff00;",
        "text-outline: 2px #0000ff;"
    };
    for (size_t channel = 0; channel < effects.size(); ++channel)
    {
        SCOPED_TRACE(effects[channel]);
        const EffectRender render = RenderEffectSample("A", effects[channel]);
        if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
        ASSERT_FALSE(render.Pixels.empty()) << render.Diagnostic;
        size_t colored = 0;
        for (size_t i = 0; i + 3 < render.Pixels.size(); i += 4)
            if (render.Pixels[i + channel] > render.Pixels[i + (channel + 1) % 3] + 12
                && render.Pixels[i + channel] > render.Pixels[i + (channel + 2) % 3] + 12)
                ++colored;
        EXPECT_GT(colored, 10u);
    }
}

namespace
{
// A text's effects paint under all of its glyphs: a fully covered fill pixel
// reads the same with a glow as without, whether the glow is the next glyph's
// (it reaches across the letter gap) or the glyph's own under a translucent
// fill.
void ExpectGlowLeavesFullyCoveredFillUnchanged(const std::string& text, const std::string& css)
{
    const EffectRender plain = RenderEffectSample(text, css);
    if (!plain.DeviceAvailable) GTEST_SKIP() << plain.Diagnostic;
    const EffectRender glowing = RenderEffectSample(text, css + " text-glow: 6px #ff0000;");
    const size_t size = size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4;
    ASSERT_EQ(plain.Pixels.size(), size) << plain.Diagnostic;
    ASSERT_EQ(glowing.Pixels.size(), size) << glowing.Diagnostic;
    uint8_t fullCoverage = 0;
    for (size_t i = 0; i < size; i += 4)
        fullCoverage = std::max(fullCoverage, plain.Pixels[i + kGreen]);
    size_t covered = 0, changed = 0;
    for (size_t i = 0; i < size; i += 4)
    {
        if (plain.Pixels[i + kGreen] + 1 < fullCoverage)
            continue;
        ++covered;
        for (int channel = kRed; channel <= kBlue; ++channel)
            if (std::abs(plain.Pixels[i + channel] - glowing.Pixels[i + channel]) > 1)
            {
                ++changed;
                break;
            }
    }
    ASSERT_GT(covered, 50u);
    EXPECT_EQ(changed, 0u) << "of " << covered << " fully covered fill pixels";
}
} // namespace

TEST(TextEffects, NextGlyphsGlowLeavesTheFillUnchanged)
{
    ExpectGlowLeavesFullyCoveredFillUnchanged("rn", "");
}

TEST(TextEffects, GlowUnderATranslucentFillLeavesTheFillUnchanged)
{
    ExpectGlowLeavesFullyCoveredFillUnchanged("I", "opacity: 0.5;");
}

// Every glow of a run paints under every outline of it: the first stem's red
// outline reads the same on its right, where the next glyph's blue glow
// reaches, as on its left, where no glyph does. Sampled one pixel out from
// each edge, inside the 2 px band; the tolerance (0.1) covers the band's
// subpixel phase, the next glyph's glow over the outline exceeded 0.5.
TEST(TextEffects, NextGlyphsGlowStaysUnderTheOutline)
{
    const EffectRender render = RenderEffectSample("II", "text-outline: 2px #ff0000; text-glow: 12px #0000ff;");
    if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
    ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
        << render.Diagnostic;
    // The middle row of the ink, and the first stem's two edges on it, in
    // pixel-centre coordinates where the fill coverage crosses one half.
    const int row = FindStemRightEdge(render.Pixels).Row;
    ASSERT_GE(row, 0);
    double left = -1.0, right = -1.0;
    for (int x = 1; x < kWidth / 2 && right < 0.0; ++x)
    {
        const double before = Channel(render.Pixels, x - 1, row, kGreen);
        const double at = Channel(render.Pixels, x, row, kGreen);
        if (left < 0.0 && before < 0.5 && at >= 0.5)
            left = x - (at - 0.5) / (at - before);
        else if (left >= 0.0 && before >= 0.5 && at < 0.5)
            right = (x - 1) + (before - 0.5) / (before - at);
    }
    ASSERT_GT(left, 0.0);
    ASSERT_GT(right, left);
    const int leftX = static_cast<int>(std::floor(left - 0.5));
    const int rightX = static_cast<int>(std::ceil(right + 0.5));
    for (const int channel : {kRed, kGreen, kBlue})
        EXPECT_NEAR(Channel(render.Pixels, rightX, row, channel),
                    Channel(render.Pixels, leftX, row, channel), 0.1)
            << "channel " << channel << " left x " << leftX << " right x " << rightX;
    EXPECT_GT(Channel(render.Pixels, leftX, row, kRed), 0.75) << "the left sample is in the outline band";
}

// A shadow is an offset sample: with no blur it is the fill's own coverage,
// moved by the offset, right and down for positive offsets.
TEST(TextEffects, SharpShadowIsTheFillCoverageOffset)
{
    for (const std::array<int, 2> offset : {std::array<int, 2>{70, 0}, std::array<int, 2>{0, 30}})
    {
        const int dx = offset[0], dy = offset[1];
        SCOPED_TRACE("offset " + std::to_string(dx) + ", " + std::to_string(dy));
        const EffectRender render = RenderEffectSample(
            "A", "text-shadow: " + std::to_string(dx) + "px " + std::to_string(dy) + "px #ff0000;");
        if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
        ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
            << render.Diagnostic;
        double maximumError = 0.0, totalError = 0.0, shadowInk = 0.0;
        size_t compared = 0;
        for (int y = 30 + dy; y < 140 + dy; ++y)
            for (int x = 40 + dx; x < 210 + dx; ++x)
            {
                if (Channel(render.Pixels, x, y, kGreen) > 0.0)
                    continue;
                const double expected = Channel(render.Pixels, x - dx, y - dy, kGreen);
                const double error = std::abs(Channel(render.Pixels, x, y, kRed) - expected);
                maximumError = std::max(maximumError, error);
                totalError += error;
                shadowInk += expected;
                ++compared;
            }
        ASSERT_GT(shadowInk, 100.0);
        EXPECT_LT(maximumError, 0.02);
        EXPECT_LT(totalError / compared, 0.002);
    }
}

// The outline band beside a straight stem is the authored width. Along one row
// the outline paint outside the fill, red minus green because the white fill
// adds its own coverage to red, integrates clamp(width + 0.5 - d, 0, 1) over the
// distance d past the edge, which is the width.
TEST(TextEffects, OutlineBandBesideAStemIsTheAuthoredWidth)
{
    for (const int width : {1, 2, 4})
    {
        SCOPED_TRACE("width=" + std::to_string(width));
        const EffectRender render = RenderEffectSample(
            "I", "text-outline: " + std::to_string(width) + "px #ff0000;");
        if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
        ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
            << render.Diagnostic;
        const StemEdge edge = FindStemRightEdge(render.Pixels);
        ASSERT_GE(edge.Row, 0);
        const int first = static_cast<int>(std::floor(edge.X)) - 2;
        double band = 0.0;
        for (int x = first; x <= first + width + 6; ++x)
            band += Channel(render.Pixels, x, edge.Row, kRed) - Channel(render.Pixels, x, edge.Row, kGreen);
        EXPECT_NEAR(band, width, 0.5);
    }
}

TEST(TextEffects, OutlineMatchesDilatedMaskReference)
{
    for (const char* glyph : {"A", "O", "i"})
        for (const int width : {1, 2, 4})
        {
            SCOPED_TRACE(std::string(glyph) + " width=" + std::to_string(width));
            const EffectRender render = RenderEffectSample(
                glyph, "text-outline: " + std::to_string(width) + "px #ff0000;");
            if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
            ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
                << render.Diagnostic;
            // The white fill's green channel is the glyph mask; the red outline
            // is visible where the fill is absent. The mask is antialiased and
            // sampled at pixel centers, so a whole-pixel disk dilation only
            // bounds the true geometric dilation: the outline must agree with it
            // on average, must be present wherever fully lit ink lies within
            // width - 1, and must be absent beyond width + 1 of any ink.
            double totalError = 0.0, worstMissing = 1.0, worstStray = 0.0;
            size_t compared = 0;
            for (int y = 30; y < 140; ++y)
                for (int x = 30; x < 220; ++x)
                {
                    if (Channel(render.Pixels, x, y, kGreen) > 0.0)
                        continue;
                    const double actual = Channel(render.Pixels, x, y, kRed);
                    totalError += std::abs(actual - MaskWithin(render.Pixels, x, y, width));
                    ++compared;
                    if (width > 1 && MaskWithin(render.Pixels, x, y, width - 1) >= 0.99)
                        worstMissing = std::min(worstMissing, actual);
                    if (MaskWithin(render.Pixels, x, y, width + 1) == 0.0)
                        worstStray = std::max(worstStray, actual);
                }
            ASSERT_GT(compared, 10000u);
            EXPECT_LT(totalError / compared, 0.01);
            EXPECT_GT(worstMissing, 0.9);
            EXPECT_LT(worstStray, 0.02);
        }
}

// A glow is a wider falloff of the same distance: beside a straight stem the
// distance to the outline is the horizontal distance to its edge, and the glow
// is exp(-2 (d / R)^2) of it.
TEST(TextEffects, GlowFallsOffWithTheDistanceToTheOutline)
{
    constexpr double kRadius = 6.0;
    const EffectRender render = RenderEffectSample("I", "text-glow: 6px #ff0000;");
    if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
    ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
        << render.Diagnostic;
    const StemEdge edge = FindStemRightEdge(render.Pixels);
    ASSERT_GE(edge.Row, 0);
    for (int step = 2; step <= 12; ++step)
    {
        const int x = static_cast<int>(std::floor(edge.X)) + step;
        const double distance = x - edge.X;
        const double expected = std::exp(-2.0 * (distance / kRadius) * (distance / kRadius));
        EXPECT_NEAR(Channel(render.Pixels, x, edge.Row, kRed), expected, 0.03)
            << "x " << x << " distance " << distance;
        EXPECT_EQ(Channel(render.Pixels, x, edge.Row, kGreen), 0.0);
    }
}

// A blurred shadow is the Gaussian soft edge of the signed distance at the
// offset sample, with the CSS blur radius (sigma = blur / 2) widened by the
// pixel's own half-pixel footprint.
TEST(TextEffects, BlurredShadowIsTheSoftEdgeOfTheOffsetGlyph)
{
    constexpr double kOffset = 60.0;
    constexpr double kSigma = 2.0; // text-shadow blur 4px.
    const EffectRender render = RenderEffectSample("I", "text-shadow: 60px 0px 4px #ff0000;");
    if (!render.DeviceAvailable) GTEST_SKIP() << render.Diagnostic;
    ASSERT_EQ(render.Pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4)
        << render.Diagnostic;
    const StemEdge edge = FindStemRightEdge(render.Pixels);
    ASSERT_GE(edge.Row, 0);
    const double shadowEdge = edge.X + kOffset;
    const double sigma = std::sqrt(kSigma * kSigma + 0.25);
    for (int step = 1; step <= 7; ++step)
    {
        const int x = static_cast<int>(std::floor(shadowEdge)) + step;
        const double distance = x - shadowEdge;
        const double expected = 0.5 - 0.5 * std::erf(distance / (sigma * std::sqrt(2.0)));
        EXPECT_NEAR(Channel(render.Pixels, x, edge.Row, kRed), expected, 0.03)
            << "x " << x << " distance " << distance;
    }
}

namespace
{
// A colour glyph (a green texture whose alpha is one column) with a shadow
// 100 px to the right: its effect instance, then its fill.
class ColorGlyphShadowProbe final : public UIElement
{
public:
    Rendering::TextureHandle Texture;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                              float, float, float, float) override
    {
        const auto slot = ctx.Textures->Register(Texture);
        auto glyph = UI::MakeColorGlyph(40, 40, 64, 64, slot, 0, 0, 1, 1, 0xffffffffu);
        UI::TextEffects effects;
        effects.ShadowColor = UI::PackColorU8(255, 0, 0, 255);
        effects.ShadowOffsetX = 100;
        auto shadow = glyph;
        UI::SetTextEffects(shadow, effects);
        ctx.Emit(shadow);
        ctx.Emit(glyph);
    }
};
} // namespace

TEST(TextEffects, ColorGlyphShadowIsItsOffsetAlphaWithoutItsColor)
{
    using namespace Rendering;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, R"(<uielement id="root"/>)",
        "#root { width: 400px; height: 200px; background-color: #000000; } #probe { width: 400px; height: 200px; }");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* device = fixture.Manager().GetDevice();
    std::array<uint8_t, 8 * 8 * 4> source{};
    // Straight RGB green in transparent texels too: the shadow must take the
    // alpha alone, never the colour.
    for (size_t i = 1; i < source.size(); i += 4) source[i] = 255;
    for (int y = 1; y < 7; ++y)
        source[(y * 8 + 3) * 4 + 3] = 255;
    TextureDesc desc{};
    desc.width = desc.height = 8;
    desc.mipLevels = desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
    const auto texture = device->CreateTexture(desc);
    ASSERT_TRUE(texture.IsValid());
    BufferDesc bufferDesc{};
    bufferDesc.size = source.size();
    bufferDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    bufferDesc.memoryUsage = BufferMemoryUsage::Upload;
    const auto staging = device->CreateBuffer(bufferDesc);
    device->UpdateBuffer(staging, 0, source.size(), source.data());
    auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::CopyDest));
    commands->CopyBufferToTextureSubresource(staging, texture, 0, 0, 8, 8, 0, 8 * 4);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();
    device->DestroyBuffer(staging);
    auto probe = std::make_unique<ColorGlyphShadowProbe>();
    probe->SetId("probe");
    probe->Texture = texture;
    fixture.Element("root")->AddChild(std::move(probe));
    fixture.Settle();
    const auto pixels = UITesting::RenderUiToBytes(fixture.Manager(), UI::UITargetSpace::EncodedSrgb());
    device->WaitForIdle();
    device->DestroyTexture(texture);
    ASSERT_EQ(pixels.size(), size_t{UITesting::kReadbackW} * UITesting::kReadbackH * 4);
    double maximumError = 0.0, shadowInk = 0.0, tint = 0.0;
    for (int y = 30; y < 120; ++y)
        for (int x = 130; x < 215; ++x)
        {
            const double expected = Channel(pixels, x - 100, y, kGreen);
            maximumError = std::max(maximumError, std::abs(Channel(pixels, x, y, kRed) - expected));
            shadowInk += expected;
            tint = std::max({tint, Channel(pixels, x, y, kGreen), Channel(pixels, x, y, kBlue)});
        }
    ASSERT_GT(shadowInk, 50.0);
    EXPECT_LT(maximumError, 0.02);
    EXPECT_EQ(tint, 0.0);
}
