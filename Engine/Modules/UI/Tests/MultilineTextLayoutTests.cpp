// Multi-line text through the real UIManager pipeline — style resolution,
// Yoga measurement, primitive generation — at several content scales.
//
// Companion to TextGeometryChromeParityTests, which pins the shaping layer
// against Chrome. These pin what the layer above it does with those numbers:
// where each line box lands horizontally, whether Yoga sizes a box for the
// lines the renderer will actually draw into it, and whether either of those
// answers changes shape at a fractional DPI scale.
//
// Wrapped text is measured by two independent implementations — the Yoga
// measure callback walks a caret map, the renderer shapes glyphs — so their
// agreement is itself an invariant worth pinning rather than assuming.

#include "FixedScalePlatform.h"
#include "RobotoTestFont.h"
#include "UIRgTestHarness.h"

#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;
using GameEngine::UITesting::FixedScalePlatform;
using GameEngine::UITesting::kUiAtlasPixelSize;
using GameEngine::UITesting::LoadRobotoAtlas;
using GameEngine::UITesting::LoadStagedFontAtlas;

namespace
{

// 1.0, the two fractional Windows steps, and Retina. A scale applied twice or
// truncated inconsistently only shows up at the fractional ones.
constexpr float kContentScales[] = {1.0f, 1.25f, 1.5f, 2.0f};

constexpr char kParagraph[] = "The quick brown fox jumps over the lazy dog";

// A wrapping Label inside a fixed-size root, styled from CSS so the whole
// resolve/layout/emit path runs as it does in the editor.
struct WrappedLabelFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    std::unique_ptr<FixedScalePlatform> Platform;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    Label* Lbl = nullptr;
    float ContentScale = 1.0f;

    // Sizes are CSS-logical px, as authored.
    bool Init(float contentScale, float labelWidth, const char* textAlign,
              const std::string& text, const char* extraLabelCss = "")
    {
        ContentScale = contentScale;
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto label = std::make_unique<Label>();
        Lbl = label.get();
        Lbl->SetId("lbl");
        root->AddChild(std::move(label));

        Platform = std::make_unique<FixedScalePlatform>(contentScale);
        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetPlatform(Platform.get());
        Ui->SetRoot(std::move(root));

        const auto css =
            std::filesystem::temp_directory_path() /
            ("ui_multiline_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 600px; height: 400px; }\n"
              << "#lbl { width: " << labelWidth << "px; font-family: Roboto; font-size: 16px; "
              << "white-space: normal; text-align: " << textAlign << "; " << extraLabelCss
              << " }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Lbl->SetText(text);
        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 4; ++i)
            Pump();
        return true;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    // The face the label's own `font-family` resolved to, not the manager's
    // default. Without the staged Roboto the resolver falls through to a
    // system face and every Chrome-derived number below would be compared
    // against the wrong font, so callers check this is really Roboto.
    FontAtlas* Font() const { return Ui->ResolveFontForStyle(Lbl->GetResolvedStyle()); }

    bool ResolvedRoboto() const
    {
        FontAtlas* font = Font();
        if (!font)
            return false;
        const auto info = font->GetFaceDebugInfo();
        return info && info->family.find("Roboto") != std::string::npos;
    }

    float PhysicalPixelSize() const
    {
        const float fontSize = Lbl->GetResolvedStyle().Visual.FontSize;
        return std::max(1.0f, fontSize * ContentScale);
    }

    // Content box left edge / width in physical px.
    float ContentLeft() const
    {
        const ResolvedStyle& rs = Lbl->GetResolvedStyle();
        return Lbl->GetLayoutX() * ContentScale +
               (rs.Layout.Padding.Left + rs.Layout.BorderWidth.Left) * ContentScale;
    }
    float ContentWidth() const
    {
        const ResolvedStyle& rs = Lbl->GetResolvedStyle();
        const float inset = rs.Layout.Padding.Left + rs.Layout.Padding.Right +
                            rs.Layout.BorderWidth.Left + rs.Layout.BorderWidth.Right;
        return std::max(0.0f, Lbl->GetLayoutWidth() * ContentScale - inset * ContentScale);
    }

    std::vector<UI::UIPrimitive> GlyphPrims() const
    {
        std::vector<UI::UIPrimitive> out;
        for (uint16_t i = 0;; ++i)
        {
            const UI::UIPrimitive* p = Ui->PeekPrimitiveForTesting(*Lbl, i);
            if (!p)
                break;
            if (UI::GetMode(p->ModeAndFlags) == UI::PrimitiveMode::Slug)
                out.push_back(*p);
        }
        return out;
    }
};

// Groups emitted glyph quads into visual lines by their Y, and reports each
// line's left edge and the span its quads cover.
struct EmittedLine
{
    float Top = 0.0f;
    float Left = 0.0f;
    float Right = 0.0f;
    size_t Count = 0;
};

// Glyph quad tops vary within a line — an ascender starts higher than an
// x-height letter — so lines are separated by gap size rather than by matching
// a single Y. Any gap larger than the intra-line ink spread but smaller than
// the baseline-to-baseline distance separates two lines; `maxGapWithinLine`
// is that threshold.
std::vector<EmittedLine> GroupGlyphsIntoLines(const std::vector<UI::UIPrimitive>& glyphs,
                                              float maxGapWithinLine)
{
    std::vector<const UI::UIPrimitive*> sorted;
    sorted.reserve(glyphs.size());
    for (const auto& g : glyphs)
        sorted.push_back(&g);
    std::sort(sorted.begin(), sorted.end(),
              [](const UI::UIPrimitive* a, const UI::UIPrimitive* b) { return a->Y < b->Y; });

    std::vector<EmittedLine> lines;
    float previousY = 0.0f;
    for (const UI::UIPrimitive* g : sorted)
    {
        if (lines.empty() || (g->Y - previousY) > maxGapWithinLine)
            lines.push_back({g->Y, g->X, g->X + g->W, 0});
        EmittedLine& line = lines.back();
        line.Top = std::min(line.Top, g->Y);
        line.Left = std::min(line.Left, g->X);
        line.Right = std::max(line.Right, g->X + g->W);
        line.Count += 1;
        previousY = g->Y;
    }
    return lines;
}

// Glyph quads are dilated for anti-aliasing, so a line's ink box is wider than
// its advance width by roughly a pixel on each side. Line-position assertions
// allow for that rather than pretending the quad edge is the pen position.
constexpr float kInkDilationTolerancePx = 2.5f;

// At font-size 16 the tallest and shortest glyph tops on one line differ by
// about 4px, while consecutive lines are 25.9px apart. 10px separates the two
// at scale 1 and is scaled with the content scale at the call sites.
constexpr float kLineGapThresholdPx = 10.0f;

} // namespace

// --- Per-line alignment (issue #711 item 4) ----------------------------------

// Chrome centres each line box on its own width: for this paragraph at a
// 150px width it puts the three lines at x = 3.17, 6.48 and 61.44. A
// block-level offset would put all three at the same x, leaving the short
// final line hard against the left edge.
TEST(MultilineTextLayout, CenterAlignmentCentresEachLineIndependently)
{
    WrappedLabelFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, "center", kParagraph));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto glyphs = fx.GlyphPrims();
    ASSERT_FALSE(glyphs.empty());
    const auto lines = GroupGlyphsIntoLines(glyphs, kLineGapThresholdPx);
    ASSERT_EQ(lines.size(), 3u);

    const float kChromeLeft[] = {3.171875f, 6.484375f, 61.4375f};
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_NEAR(lines[i].Left - fx.ContentLeft(), kChromeLeft[i], kInkDilationTolerancePx)
            << "line " << i;

    // The three offsets must genuinely differ — equal offsets are exactly the
    // block-level-alignment defect this pins against.
    EXPECT_GT(lines[2].Left, lines[1].Left + 40.0f);
    EXPECT_GT(lines[1].Left, lines[0].Left + 1.0f);
}

TEST(MultilineTextLayout, RightAlignmentAlignsEachLineToTheContentEdge)
{
    WrappedLabelFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, "right", kParagraph));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto lines = GroupGlyphsIntoLines(fx.GlyphPrims(), kLineGapThresholdPx);
    ASSERT_EQ(lines.size(), 3u);

    const float contentRight = fx.ContentLeft() + fx.ContentWidth();
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_NEAR(lines[i].Right, contentRight, kInkDilationTolerancePx) << "line " << i;

    // Chrome's per-line left edges for the same block.
    const float kChromeLeft[] = {6.34375f, 12.96875f, 122.875f};
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_NEAR(lines[i].Left - fx.ContentLeft(), kChromeLeft[i], kInkDilationTolerancePx)
            << "line " << i;
}

TEST(MultilineTextLayout, LeftAlignmentPutsEveryLineAtTheContentEdge)
{
    WrappedLabelFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, "left", kParagraph));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto lines = GroupGlyphsIntoLines(fx.GlyphPrims(), kLineGapThresholdPx);
    ASSERT_EQ(lines.size(), 3u);
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_NEAR(lines[i].Left, fx.ContentLeft(), kInkDilationTolerancePx) << "line " << i;
}

// Per-line alignment has to survive the logical -> physical conversion: the
// offsets scale with the content scale rather than being computed once in the
// wrong space.
TEST(MultilineTextLayout, PerLineAlignmentHoldsAcrossContentScales)
{
    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, "center", kParagraph)) << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto lines = GroupGlyphsIntoLines(fx.GlyphPrims(), kLineGapThresholdPx * scale);
        ASSERT_EQ(lines.size(), 3u) << "scale=" << scale;

        // Each line's ink is centred in the content box to within the AA
        // dilation, independent of scale.
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const float leftGap = lines[i].Left - fx.ContentLeft();
            const float rightGap = (fx.ContentLeft() + fx.ContentWidth()) - lines[i].Right;
            EXPECT_NEAR(leftGap, rightGap, 2.0f * kInkDilationTolerancePx)
                << "scale=" << scale << " line " << i;
        }
    }
}

// --- Yoga measurement versus the shaped result (issue #711 item 3) -----------

// The measure callback and the renderer are separate wrap implementations. If
// they disagree about where lines break, Yoga sizes a box for one layout and
// the renderer draws another into it — clipped or short-measured text with no
// single obvious culprit.
TEST(MultilineTextLayout, MeasuredBlockAgreesWithTheShapedBlock)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const char* corpus[] = {
        kParagraph,
        "a b c",
        "one two three four five six seven eight nine ten",
        "e-mail well-known co-op",
        "supercalifragilistic word",
        "first line\nsecond much longer line here\nthird",
    };

    for (const char* text : corpus)
    {
        for (float wrap : {40.0f, 60.0f, 100.0f, 150.0f, 200.0f})
        {
            WrappedLabelFixture fx;
            ASSERT_TRUE(fx.Init(1.0f, wrap, "left", text));
            if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

            StyledRun run{};
            run.Font = fx.Font();
            run.PixelSize = fx.PhysicalPixelSize();
            run.Text = text;
            const float lineBox = 0.0f; // unset line-height: "normal"
            const auto shaped =
                TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), wrap, lineBox,
                                           TextLayout::WordBreak::Normal);

            // The renderer emitted exactly the glyphs the shaper produced, in
            // order — so line membership can be read off the shaped result
            // instead of re-derived from quad positions.
            const auto glyphs = fx.GlyphPrims();
            ASSERT_EQ(glyphs.size(), shaped.Glyphs.size())
                << "\"" << text << "\" @" << wrap << " emitted vs shaped glyph count";

            // And Yoga measured a box for exactly those lines — single-line
            // included, both paths share the CSS line box. Yoga snaps the
            // measured size to the logical pixel grid, so agreement is to
            // within one pixel, not exactly.
            EXPECT_NEAR(fx.Lbl->GetLayoutHeight(), shaped.Metrics.height, 1.0f)
                << "\"" << text << "\" @" << wrap;
        }
    }
}

// A wrapped block's height is the line count times one baseline-to-baseline
// distance, at every scale. Layout works in logical px and rendering in
// physical px; a height computed in the wrong space shows up here as a
// non-integer multiple.
TEST(MultilineTextLayout, WrappedBlockHeightIsAWholeNumberOfLinesAtEveryScale)
{
    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, "left", kParagraph)) << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        StyledRun run{};
        run.Font = fx.Font();
        run.PixelSize = fx.PhysicalPixelSize();
        run.Text = kParagraph;
        const auto shaped = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
                                                       150.0f * scale, 0.0f,
                                                       TextLayout::WordBreak::Normal);
        ASSERT_EQ(shaped.LineCount, 3u) << "scale=" << scale;
        ASSERT_EQ(shaped.LineBreaks.size(), 3u) << "scale=" << scale;

        // The laid-out logical height converts to exactly three line boxes,
        // to within Yoga's pixel-grid snapping.
        const float lineBox = shaped.Metrics.height / 3.0f;
        EXPECT_NEAR(fx.Lbl->GetLayoutHeight() * scale, lineBox * 3.0f, 1.0f * scale)
            << "scale=" << scale;

        // And every emitted quad sits where the shaper put it, offset by the
        // content box — the logical-to-physical conversion introduced no drift.
        const auto glyphs = fx.GlyphPrims();
        ASSERT_EQ(glyphs.size(), shaped.Glyphs.size()) << "scale=" << scale;
        const ResolvedStyle& style = fx.Lbl->GetResolvedStyle();
        const float contentTop =
            fx.Lbl->GetLayoutY() * scale +
            (style.Layout.Padding.Top + style.Layout.BorderWidth.Top) * scale;
        for (size_t i = 0; i < glyphs.size(); ++i)
            EXPECT_NEAR(glyphs[i].Y - contentTop, shaped.Glyphs[i].y, 1.5f)
                << "scale=" << scale << " glyph " << i;
    }
}

// UIManager::MeasureTextLineCount answers for any text what the label's own measure answers:
// at the label's width it counts the three lines the block above is laid out with, and a
// shorter text, or the same text at a wider width, one. The label's own text stays as it was.
TEST(MultilineTextLayout, MeasureTextLineCountAgreesWithTheLabelsOwnMeasure)
{
    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, "left", kParagraph)) << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        EXPECT_EQ(fx.Ui->MeasureTextLineCount(*fx.Lbl, kParagraph, 150.0f), 3) << "scale=" << scale;
        EXPECT_EQ(fx.Ui->MeasureTextLineCount(*fx.Lbl, "The quick", 150.0f), 1) << "scale=" << scale;
        EXPECT_EQ(fx.Ui->MeasureTextLineCount(*fx.Lbl, kParagraph, 600.0f), 1) << "scale=" << scale;
        EXPECT_EQ(fx.Lbl->GetText(), kParagraph);
        const float height = fx.Lbl->GetLayoutHeight();
        fx.Pump();
        EXPECT_FLOAT_EQ(fx.Lbl->GetLayoutHeight(), height) << "measuring leaves the label's layout alone";
    }
}

// A block taller than its text is top-aligned, matching a CSS block box: the
// container centres the box, the text does not centre itself inside it. The
// single-line centring TextInput relies on is a deliberate field convention,
// not the block rule, so this pins the boundary between them.
TEST(MultilineTextLayout, MultiLineBlocksAreTopAlignedInATallerBox)
{
    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, "left", kParagraph, "height: 200px;"))
            << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto lines = GroupGlyphsIntoLines(fx.GlyphPrims(), kLineGapThresholdPx * scale);
        ASSERT_EQ(lines.size(), 3u) << "scale=" << scale;

        const ResolvedStyle& rs = fx.Lbl->GetResolvedStyle();
        const float contentTop =
            fx.Lbl->GetLayoutY() * scale +
            (rs.Layout.Padding.Top + rs.Layout.BorderWidth.Top) * scale;
        const float lineStep = lines[1].Top - lines[0].Top;

        // The first line's ink starts within one line box of the content top —
        // it is not pushed halfway down a 200px box.
        EXPECT_GE(lines[0].Top, contentTop - 1.0f) << "scale=" << scale;
        EXPECT_LT(lines[0].Top - contentTop, lineStep) << "scale=" << scale;
    }
}

// A bordered box is measured against the same width it is painted into.
//
// Wrap width is decided twice: the Yoga measure callback wraps at the content
// width Yoga solved, the renderer wraps at ComputeContentBox's. The renderer
// has always subtracted the border; Yoga only does once it is told the border
// exists. While it was not, the label was measured against a box 40px wider
// than the one it was drawn into, and Yoga sized it for fewer lines than the
// renderer then drew — so the last lines fell outside the box.
//
// The specimen is chosen so the border genuinely changes the answer. This
// paragraph takes 3 lines anywhere in 120..160px and 4 at 115px and below, so
// a 160px box with a 25px border — 110px of content — is measured as 4 lines
// with the border in the solve and 3 without it. A narrower border would land
// inside that 120..160 plateau, both arms would wrap identically, and the
// specimen could not have failed however wrong the box model was.
TEST(MultilineTextLayout, BorderedBoxIsSizedForTheLinesItActuallyDraws)
{
    constexpr float kLabelWidth = 160.0f;
    constexpr float kBorder = 25.0f;
    constexpr size_t kLinesAtContentWidth = 4; // at the 110px content width

    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, kLabelWidth, "left", kParagraph,
                            "padding: 0px; border: 25px solid #ff0000;"))
            << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto lines = GroupGlyphsIntoLines(fx.GlyphPrims(), kLineGapThresholdPx * scale);
        ASSERT_GE(lines.size(), 2u) << "scale=" << scale;

        const ResolvedStyle& rs = fx.Lbl->GetResolvedStyle();
        EXPECT_FLOAT_EQ(rs.Layout.BorderWidth.Left, kBorder) << "scale=" << scale;

        // Specimen guards, not the discriminator: these two hold in both arms.
        // The renderer's wrap width was never the broken half — it subtracted
        // the border all along — so it draws 4 lines either way. They are here
        // to prove the 110px content width really is a 4-line width, which is
        // what makes the height arithmetic below mean anything.
        EXPECT_NEAR(fx.ContentWidth(), (kLabelWidth - 2.0f * kBorder) * scale, 0.01f)
            << "scale=" << scale;
        EXPECT_EQ(lines.size(), kLinesAtContentWidth) << "scale=" << scale;

        // Every line was drawn inside the content box horizontally — the
        // renderer's half of the agreement.
        const float contentRight = fx.ContentLeft() + fx.ContentWidth();
        for (size_t i = 0; i < lines.size(); ++i)
            EXPECT_LE(lines[i].Right, contentRight + kInkDilationTolerancePx)
                << "scale=" << scale << " line " << i;

        // Yoga's half, and the assertion that actually separates the two arms.
        // With the border out of the solve Yoga measured this label at 160px,
        // sized it for 3 lines and added no border to the height, leaving a
        // content height of 13px for the 4 lines the renderer went on to draw.
        const float lineStep = lines[1].Top - lines[0].Top;
        ASSERT_GT(lineStep, 0.0f) << "scale=" << scale;
        const float contentHeight =
            fx.Lbl->GetLayoutHeight() * scale -
            (rs.Layout.Padding.Top + rs.Layout.Padding.Bottom + rs.Layout.BorderWidth.Top +
             rs.Layout.BorderWidth.Bottom) *
                scale;
        EXPECT_EQ(static_cast<size_t>(std::lround(contentHeight / lineStep)), lines.size())
            << "scale=" << scale << " contentHeight=" << contentHeight
            << " lineStep=" << lineStep;
    }
}

// --- Line-height mapping (issue #711 item 2) ---------------------------------

// CSS resolves `line-height` against the font *size*: at font-size 16,
// `line-height: 1` is a 16px line box and `line-height: 20px` is a 20px one.
// Chrome measures this paragraph at 48px, 60px and 72px tall for line-height
// 1, 1.25 and 1.5 (three lines of 16, 20 and 24) — and so does the engine:
// ResolveLineBoxPx maps the parser's sign encoding to a line box in px, and
// ShapeMultiline steps by exactly that box.
TEST(MultilineTextLayout, LineHeightResolvesAgainstFontSizePerCss)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    constexpr float kPixelSize = 16.0f;
    // Chrome reports 21px for `line-height: normal` on this face at 16px, and
    // so do we: Blink rounds ascent, descent and lineGap to whole device
    // pixels independently and sums them. (The old path reported 21.6, and
    // moved with the atlas ppem.)
    const float metricHeight = atlas->GetFontLineMetrics(kPixelSize).height;
    ASSERT_NEAR(metricHeight, 21.0f, 0.001f);

    StyledRun run{};
    run.Font = atlas.get();
    run.PixelSize = kPixelSize;
    run.Text = kParagraph;

    struct Case
    {
        float CssLineHeight; // parser encoding: negative = unitless multiplier
        float ChromeHeight;  // three line boxes, from Chrome
    };
    const Case cases[] = {
        {-1.0f, 48.0f},
        {-1.25f, 60.0f},
        {-1.5f, 72.0f},
    };

    for (const auto& c : cases)
    {
        const float box = TextLayout::ResolveLineBoxPx(c.CssLineHeight, (float)kPixelSize, 1.0f);
        const auto shaped = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 150.0f,
                                                       box, TextLayout::WordBreak::Normal);
        ASSERT_EQ(shaped.LineCount, 3u);
        EXPECT_NEAR(shaped.Metrics.height, c.ChromeHeight, 0.001f);
    }

    // A length line-height means exactly that many pixels per line.
    const float pxBox = TextLayout::ResolveLineBoxPx(20.0f, (float)kPixelSize, 1.0f);
    const auto shapedPx = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 150.0f,
                                                     pxBox, TextLayout::WordBreak::Normal);
    ASSERT_EQ(shapedPx.LineCount, 3u);
    EXPECT_NEAR(shapedPx.Metrics.height, 60.0f, 0.001f);

    // "normal" (unset) uses the font's own line height per line — three of
    // them is Chrome's 63px for this paragraph.
    const auto shapedNormal = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
                                                         150.0f, 0.0f, TextLayout::WordBreak::Normal);
    ASSERT_EQ(shapedNormal.LineCount, 3u);
    EXPECT_NEAR(shapedNormal.Metrics.height, metricHeight * 3.0f, 0.001f);
}

// A Script Editor line-height preference expressed in "normal lines" is turned
// into a unitless CSS line-height by the resolved face's ratio (normal line box
// / font size), probed once at 64px. Because a `normal` line box is rounded to
// whole pixels per size, that probe is an approximation, and the error it can
// carry is bounded only over the sizes the Script Editor actually renders at:
// its stylesheet is font-size 14px, so the probe is applied at 14 logical px
// times the content scales the editor supports. Over the whole 6-256px range
// the same ratio drifts past 2px, which is why this pins the real sizes rather
// than an arbitrary sweep.
//
// A slider at exactly 1.0 emits the literal keyword `normal` and skips the
// ratio entirely, so this error only ever applies to non-default settings.
TEST(MultilineTextLayout, ProbedNormalRatioIsSubPixelAtTheScriptEditorsOwnSizes)
{
    // The monospace faces the Script Editor's font list offers. Roboto Mono is
    // the default and ships with the editor; the rest are system installs.
    struct ScriptFace { const char* File; bool Staged; };
    const ScriptFace faces[] = {
        {"RobotoMono-Regular.ttf", true},
        {"consola.ttf", false},
        {"CascadiaCode.ttf", false},
        {"cour.ttf", false},
    };

    constexpr float kProbePx = 64.0f;
    constexpr float kScriptFontSizePx = 14.0f;
    // 100%, the two fractional Windows steps, and Retina.
    constexpr float kScriptContentScales[] = {1.0f, 1.25f, 1.5f, 2.0f};
    // Measured worst case across these faces and sizes is 0.6875px, at Roboto
    // Mono under a 125% scale.
    constexpr float kMaxDriftPx = 0.75f;

    int facesChecked = 0;
    for (const auto& face : faces)
    {
        std::unique_ptr<FontAtlas> atlas;
        if (face.Staged)
        {
            atlas = LoadStagedFontAtlas(face.File);
        }
        else if (const char* windir = std::getenv("WINDIR"))
        {
            const std::string path = std::string(windir) + "\\Fonts\\" + face.File;
            if (Rendering::Utils::FileExists(path.c_str()))
            {
                const std::vector<uint8_t> bytes = Rendering::Utils::ReadFile(path.c_str());
                auto candidate = std::make_unique<FontAtlas>();
                if (!bytes.empty() &&
                    candidate->LoadFontBytes(bytes.data(), bytes.size(), kUiAtlasPixelSize))
                    atlas = std::move(candidate);
            }
        }
        if (!atlas)
            continue;
        ++facesChecked;

        const float probedRatio = atlas->GetFontLineMetrics(kProbePx).height / (float)kProbePx;
        ASSERT_GT(probedRatio, 0.0f) << face.File;

        for (float scale : kScriptContentScales)
        {
            // The atlas ppem the editor would rasterise this at.
            const float px = std::max(1.0f, kScriptFontSizePx * scale);
            const float actual = atlas->GetFontLineMetrics(px).height;
            EXPECT_NEAR(probedRatio * px, actual, kMaxDriftPx)
                << face.File << " at content scale " << scale << " (" << px << "px)";
        }
    }
    ASSERT_GT(facesChecked, 0) << "no Script Editor face available to probe";
}

// CSS resolves a unitless line-height against the font SIZE, so a multiplier
// tuned for one face reproduces "one normal line" only for faces that happen
// to share its metric ratio — every other face gets that first face's line.
// The Script Editor font is user-selectable, so its line-height preference
// (1.0 = one normal line) cannot be a fixed multiplier; it has to come from
// the resolved face, which is what `normal` and a per-face ratio both give.
TEST(MultilineTextLayout, UnitlessLineHeightIsPerFaceButNormalIsPerFont)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Second face with a different metric ratio comes from the Windows font directory";
#else
    auto reference = LoadRobotoAtlas();
    if (!reference)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    constexpr float kPixelSize = 14.0f;
    const float referenceRatio = reference->GetFontLineMetrics(kPixelSize).height / (float)kPixelSize;

    // Any system face whose normal line differs from the reference's will do;
    // these three are the monospace stacks the Script Editor font list offers.
    std::unique_ptr<FontAtlas> other;
    float otherRatio = 0.0f;
    if (const char* windir = std::getenv("WINDIR"))
    {
        for (const char* name : {"consola.ttf", "cour.ttf", "lucon.ttf"})
        {
            const std::string path = std::string(windir) + "\\Fonts\\" + name;
            if (!Rendering::Utils::FileExists(path.c_str()))
                continue;
            const std::vector<uint8_t> bytes = Rendering::Utils::ReadFile(path.c_str());
            auto candidate = std::make_unique<FontAtlas>();
            if (bytes.empty() || !candidate->LoadFontBytes(bytes.data(), bytes.size(), kUiAtlasPixelSize))
                continue;
            const float ratio = candidate->GetFontLineMetrics(kPixelSize).height / (float)kPixelSize;
            if (std::abs(ratio - referenceRatio) > 0.01f)
            {
                other = std::move(candidate);
                otherRatio = ratio;
                break;
            }
        }
    }
    if (!other)
        GTEST_SKIP() << "No system face with a metric ratio different from the reference face";

    auto lineBoxFor = [](FontAtlas* font, float cssLineHeight)
    {
        StyledRun run{};
        run.Font = font;
        run.PixelSize = kPixelSize;
        run.Text = "line";
        const float box = TextLayout::ResolveLineBoxPx(cssLineHeight, (float)kPixelSize, 1.0f);
        const auto shaped = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 0.0f, box,
                                                       TextLayout::WordBreak::Normal);
        return shaped.Metrics.height;
    };

    // `normal` (the parser's 0) is each face's own line.
    const float referenceNormal = lineBoxFor(reference.get(), 0.0f);
    const float otherNormal = lineBoxFor(other.get(), 0.0f);
    EXPECT_NEAR(referenceNormal, referenceRatio * kPixelSize, 0.001f);
    EXPECT_NEAR(otherNormal, otherRatio * kPixelSize, 0.001f);
    EXPECT_GT(std::abs(referenceNormal - otherNormal), 0.1f);

    // The regression: the reference face's ratio baked as a unitless multiplier
    // hands the other face the reference's line, not its own.
    EXPECT_NEAR(lineBoxFor(other.get(), -referenceRatio), referenceNormal, 0.001f);

    // The fix: the multiplier derived from the face in use reproduces exactly
    // that face's normal line.
    EXPECT_NEAR(lineBoxFor(other.get(), -otherRatio), otherNormal, 0.001f);
    EXPECT_NEAR(lineBoxFor(reference.get(), -referenceRatio), referenceNormal, 0.001f);
#endif
}

// Single-line measurement, multi-line measurement and the rendered block all
// share ONE definition: the CSS line box. A label that wraps to two lines is
// exactly twice as tall as the same label on one line (this used to jump from
// a 21px MeasureUtf8-based box to 25.92px metric-times-default lines the
// moment text wrapped).
TEST(MultilineTextLayout, SingleAndMultiLineShareTheCssLineBox)
{
    WrappedLabelFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 200.0f, "left", "a b c"));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const float metricHeight = fx.Font()->GetFontLineMetrics(fx.PhysicalPixelSize()).height;

    StyledRun run{};
    run.Font = fx.Font();
    run.PixelSize = fx.PhysicalPixelSize();
    run.Text = "a b c";
    const auto shaped = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 200.0f,
                                                   0.0f, TextLayout::WordBreak::Normal);
    ASSERT_EQ(shaped.LineCount, 1u);

    // Yoga snaps the measured box to the pixel grid; the shaper does not.
    EXPECT_NEAR(shaped.Metrics.height, metricHeight, 0.001f);
    EXPECT_NEAR(fx.Lbl->GetLayoutHeight(), metricHeight, 1.0f);

    WrappedLabelFixture wrapped;
    ASSERT_TRUE(wrapped.Init(1.0f, 25.0f, "left", "a b c"));
    EXPECT_NEAR(wrapped.Lbl->GetLayoutHeight(), 2.0f * metricHeight, 1.0f);
}

// --- Multi-line caret and hit-testing (issue #711 item 5) --------------------

namespace
{

// TextAreaMetrics is protected — the numbers the hit-test math is expressed in
// are implementation detail. Subclassing to read them keeps them that way
// while letting the tests aim clicks at real line boxes instead of guessing.
// ScriptTextArea extends TextArea the same way.
class ProbeTextArea final : public TextArea
{
  public:
    float LineAdvance() const
    {
        TextAreaMetrics met{};
        return ComputeMetrics(met) ? met.LineAdvance : 0.0f;
    }
    float MetricsPixelSize() const
    {
        TextAreaMetrics met{};
        return ComputeMetrics(met) ? met.Px : 0.0f;
    }
    FontAtlas* MetricsFont() const
    {
        TextAreaMetrics met{};
        return ComputeMetrics(met) ? met.Font : nullptr;
    }
};

// A TextArea in a fixed-size root. TextArea resolves its own font and metrics
// through its owner manager, so it needs the full pipeline rather than a bare
// control instance.
struct TextAreaFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    std::unique_ptr<FixedScalePlatform> Platform;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    ProbeTextArea* Area = nullptr;
    float ContentScale = 1.0f;

    bool Init(float contentScale, float width, const std::string& text)
    {
        ContentScale = contentScale;
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto area = std::make_unique<ProbeTextArea>();
        Area = area.get();
        Area->SetId("area");
        root->AddChild(std::move(area));

        Platform = std::make_unique<FixedScalePlatform>(contentScale);
        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetPlatform(Platform.get());
        Ui->SetRoot(std::move(root));

        const auto css =
            std::filesystem::temp_directory_path() /
            ("ui_textarea_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 600px; height: 400px; }\n"
              << "#area { width: " << width << "px; height: 200px; font-family: Roboto; "
              << "font-size: 16px; white-space: normal; padding: 0px; border-width: 0px; }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Area->SetValue(text);
        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 4; ++i)
        {
            Ui->Update(0.016f, /*interactive=*/true);
            DriveUiRender(*Ui, *Rg);
        }
        return true;
    }

    bool ResolvedRoboto() const
    {
        FontAtlas* font = Ui->ResolveFontForStyle(Area->GetResolvedStyle());
        if (!font)
            return false;
        const auto info = font->GetFaceDebugInfo();
        return info && info->family.find("Roboto") != std::string::npos;
    }

    // Click at a point relative to the control's own top-left, in the logical
    // units the pointer path works in.
    int ClickAt(float localX, float localY)
    {
        const ResolvedStyle& style = Area->GetResolvedStyle();
        FontAtlas* font = Ui->ResolveFontForStyle(style);
        const float x = Area->GetLayoutX();
        const float y = Area->GetLayoutY();
        Area->OnPointerDown(x + localX, y + localY, x, y, Area->GetLayoutWidth(),
                            Area->GetLayoutHeight(), style, font);
        return Area->GetCaretIndex();
    }

    void PressKey(int key)
    {
        Area->OnKey(key, /*mods=*/0, Platform.get());
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }
};

// Wrapped at 150px this is the three-line block Chrome was measured against:
// "The quick brown fox " / "jumps over the lazy " / "dog".
constexpr char kThreeLineText[] = "The quick brown fox jumps over the lazy dog";
constexpr int kSecondLineStart = 20;
constexpr int kThirdLineStart = 40;

} // namespace

// Chrome's caretRangeFromPoint maps a click at the left edge of each line to
// that line's first index: 0, 20 and 40 for this block.
TEST(MultilineTextLayout, ClickingEachLineLandsOnThatLinesFirstIndex)
{
    TextAreaFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, kThreeLineText));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const float lineAdvance = fx.Area->LineAdvance();
    ASSERT_GT(lineAdvance, 0.0f);

    EXPECT_EQ(fx.ClickAt(0.0f, lineAdvance * 0.5f), 0);
    EXPECT_EQ(fx.ClickAt(0.0f, lineAdvance * 1.5f), kSecondLineStart);
    EXPECT_EQ(fx.ClickAt(0.0f, lineAdvance * 2.5f), kThirdLineStart);
}

// The boundary between two line boxes sits exactly one line advance down: a
// click just above it belongs to the upper line, just below to the lower.
TEST(MultilineTextLayout, ClicksResolveToTheLineBoxTheyFallIn)
{
    TextAreaFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, kThreeLineText));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const float lineAdvance = fx.Area->LineAdvance();
    EXPECT_LT(fx.ClickAt(0.0f, lineAdvance - 0.5f), kSecondLineStart);
    EXPECT_EQ(fx.ClickAt(0.0f, lineAdvance + 0.5f), kSecondLineStart);

    // Below the last line the index clamps to that line instead of running off
    // the end of the line table.
    EXPECT_GE(fx.ClickAt(0.0f, lineAdvance * 10.0f), kThirdLineStart);
}

// Index to position and back on every line: the x each caret index sits at
// must hit-test to that same index, including across the wrap boundaries and
// at fractional DPI scales.
TEST(MultilineTextLayout, CaretIndexRoundTripsThroughHitTestingOnEveryLine)
{
    for (float scale : kContentScales)
    {
        TextAreaFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, kThreeLineText)) << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        FontAtlas* font = fx.Area->MetricsFont();
        const float pixelSize = fx.Area->MetricsPixelSize();
        const float lineAdvance = fx.Area->LineAdvance();
        ASSERT_TRUE(font) << "scale=" << scale;

        struct LineSpan
        {
            int Start;
            int End;
            int Index;
        };
        const LineSpan lines[] = {{0, 19, 0}, {kSecondLineStart, 39, 1}, {kThirdLineStart, 43, 2}};

        for (const auto& line : lines)
        {
            const std::string lineText(kThreeLineText + line.Start, line.End - line.Start);
            std::vector<float> caretX;
            ASSERT_TRUE(font->BuildCaretMapUtf8(lineText, pixelSize, caretX))
                << "scale=" << scale;

            const float y = lineAdvance * (static_cast<float>(line.Index) + 0.5f);
            for (size_t i = 0; i + 1 < caretX.size(); ++i)
            {
                // Aim a quarter of the way into the character the index
                // precedes: unambiguously nearer this caret than the next, so
                // the expected answer does not depend on tie-breaking.
                const float aim = caretX[i] + (caretX[i + 1] - caretX[i]) * 0.25f;
                EXPECT_EQ(fx.ClickAt(aim, y), line.Start + static_cast<int>(i))
                    << "scale=" << scale << " line " << line.Index << " index " << i;
            }
        }
    }
}

// Vertical movement carries a preferred column: stepping down onto a short
// line and back up returns to the original column rather than to where the
// short line ended.
TEST(MultilineTextLayout, VerticalCaretMovementKeepsColumnAffinity)
{
    TextAreaFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, kThreeLineText));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    // Near the end of line 1, well past the end of line 3 ("dog").
    fx.Area->SetSelection(18, 18);
    ASSERT_EQ(fx.Area->GetCaretIndex(), 18);

    fx.PressKey(Input::kKeyCode_Down);
    const int onSecondLine = fx.Area->GetCaretIndex();
    EXPECT_GE(onSecondLine, kSecondLineStart);
    EXPECT_LE(onSecondLine, 39);

    fx.PressKey(Input::kKeyCode_Down);
    const int onThirdLine = fx.Area->GetCaretIndex();
    EXPECT_GE(onThirdLine, kThirdLineStart);
    EXPECT_LE(onThirdLine, 43);

    fx.PressKey(Input::kKeyCode_Up);
    fx.PressKey(Input::kKeyCode_Up);
    EXPECT_EQ(fx.Area->GetCaretIndex(), 18);
}

// A horizontal step re-anchors the column, so a later vertical move tracks the
// new position rather than the stale preference.
TEST(MultilineTextLayout, HorizontalMovementClearsColumnAffinity)
{
    TextAreaFixture fx;
    ASSERT_TRUE(fx.Init(1.0f, 150.0f, kThreeLineText));
    if (!fx.ResolvedRoboto())
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    fx.Area->SetSelection(18, 18);
    fx.PressKey(Input::kKeyCode_Down);
    fx.PressKey(Input::kKeyCode_Down);
    fx.PressKey(Input::kKeyCode_Left);
    fx.PressKey(Input::kKeyCode_Up);
    fx.PressKey(Input::kKeyCode_Up);
    EXPECT_NE(fx.Area->GetCaretIndex(), 18);
}


// UIManager::MeasureTextWidth answers for any text what the label's own measure answers: the
// width of a label sized to its text, at every scale, whatever the label shows; a longer text
// is measured on one line, unwrapped. The label's own text stays as it was.
TEST(MultilineTextLayout, MeasureTextWidthAgreesWithTheLabelsOwnMeasure)
{
    for (float scale : kContentScales)
    {
        WrappedLabelFixture fx;
        ASSERT_TRUE(fx.Init(scale, 150.0f, "left", "The quick", "width: auto; align-self: flex-start;"))
            << "scale=" << scale;
        if (!fx.ResolvedRoboto())
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const float laidOut = fx.Lbl->GetLayoutWidth();
        ASSERT_GT(laidOut, 0.0f) << "scale=" << scale;
        EXPECT_NEAR(fx.Ui->MeasureTextWidth(*fx.Lbl, "The quick"), laidOut, 0.5f) << "scale=" << scale;
        EXPECT_GT(fx.Ui->MeasureTextWidth(*fx.Lbl, kParagraph), 2.0f * laidOut) << "scale=" << scale;
        EXPECT_EQ(fx.Lbl->GetText(), "The quick");
    }
}

// Button::WidthForText and Dropdown::WidthForLabel answer for the text a control shows the width
// it is laid out at with `width: auto`, its padding, border and chevron included, at every scale;
// a control not yet laid out answers 0.
TEST(MultilineTextLayout, ControlsReportTheWidthTheyAreLaidOutAtForTheirText)
{
    Button detached;
    EXPECT_EQ(detached.WidthForText("Run the turn"), 0.0f) << "no layout yet";

    for (float scale : kContentScales)
    {
        // First: destroyed last, after the manager released its buffers.
        SharedDeviceReleaseRetirement retirement;
        IDevice* device = SharedHeadlessDevice();
        if (!device)
            GTEST_SKIP() << "No headless device";
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto ownedButton = std::make_unique<Button>();
        Button* button = ownedButton.get();
        button->SetText("Run the turn");
        root->AddChild(std::move(ownedButton));
        auto ownedDropdown = std::make_unique<Dropdown>();
        Dropdown* dropdown = ownedDropdown.get();
        dropdown->SetOptionsFromLabels({"Auto", "Claude (local session)"}, 1);
        root->AddChild(std::move(ownedDropdown));

        FixedScalePlatform platform(scale);
        UIManager ui(device);
        ui.SetPlatform(&platform);
        ui.SetRoot(std::move(root));
        const auto css = std::filesystem::temp_directory_path() / "ui_control_widths.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; align-items: flex-start; width: 600px; "
                 "height: 400px; }\n"
              << ".button { display: flex; flex-direction: row; padding: 4px 11px; border-width: 1px; }\n"
              << ".dropdown { display: flex; flex-direction: column; }\n"
              << ".dropdown-header { display: flex; flex-direction: row; align-items: center; gap: 4px; "
                 "padding: 4px 6px 4px 10px; border-width: 1px; }\n"
              << ".dropdown-chevron { width: 8px; height: 8px; flex-shrink: 0; }\n"
              << ".dropdown-items { display: none; }\n"
              << ".button-text, .dropdown-header-label { font-family: Roboto; font-size: 16px; "
                 "white-space: nowrap; }\n";
        }
        ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
        UiRgHarness harness(device);
        for (int frame = 0; frame < 4; ++frame)
        {
            ui.Update(0.016f, /*interactive=*/true);
            DriveUiRender(ui, harness);
        }

        const float buttonWidth = button->WidthForText(button->GetText());
        ASSERT_GT(buttonWidth, 0.0f) << "scale=" << scale;
        EXPECT_NEAR(buttonWidth, button->GetLayoutWidth(), 0.5f) << "scale=" << scale;
        const float dropdownWidth = dropdown->WidthForLabel(dropdown->GetSelectedLabel());
        ASSERT_GT(dropdownWidth, 0.0f) << "scale=" << scale;
        EXPECT_NEAR(dropdownWidth, dropdown->GetLayoutWidth(), 0.5f) << "scale=" << scale;
    }
}
