// text-overflow: ellipsis through the real UIManager pipeline — style
// resolution, single-line shaping, primitive generation.
//
// The classic CSS recipe (white-space: nowrap; overflow: hidden;
// text-overflow: ellipsis) must truncate an overflowing single-line run and
// append the "…" mark inside the content box; fitting text, and the default
// clip mode, must emit exactly the glyphs they emitted before the property
// existed. The gates matter as much as the effect: overflow: visible and a
// wrapping block never ellipsize, and the property does not inherit. The mode
// is part of the shape-cache key, so a live style flip has to re-emit rather
// than replay the stale run.

#include "UIRgTestHarness.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/Label.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr char kLongText[] = "The quick brown fox jumps over the lazy dog";
constexpr float kLabelWidthPx = 120.0f;
// Wide enough for kMediumText at 16px, still inside the 400px root.
constexpr float kFittingWidthPx = 360.0f;
constexpr char kMediumText[] = "Fits only in the wide box";
// One word wider than the narrow box: it overflows even after wrapping, so a
// wrapping block is the only thing standing between it and an ellipsis.
constexpr char kUnbreakableText[] = "Supercalifragilisticexpialidocious";

// A nowrap Label inside a fixed-size root, styled from CSS so the whole
// resolve/layout/emit path runs as it does in the editor. Classes flip only
// text-overflow, so tests can compare and live-toggle the modes; ExtraCss
// (appended last, so equal specificity wins) relaxes the gates one at a time.
struct EllipsisLabelFixture
{
    struct Options
    {
        const char* LabelClass = "ell";
        float       WidthPx    = kLabelWidthPx;
        const char* ExtraCss   = "";
        // Wrap the label in an element that can carry an authored tooltip.
        bool        Wrapper    = false;
    };

    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    Label* Lbl = nullptr;
    UIElement* Wrap = nullptr;
    std::filesystem::path CssPath;

    bool Init(const std::string& text, const Options& opt)
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto label = std::make_unique<Label>();
        Lbl = label.get();
        Lbl->SetId("lbl");
        Lbl->AddClass(opt.LabelClass);
        if (opt.Wrapper)
        {
            auto wrapper = std::make_unique<UIElement>();
            wrapper->SetId("wrap");
            Wrap = wrapper.get();
            Wrap->AddChild(std::move(label));
            root->AddChild(std::move(wrapper));
        }
        else
        {
            root->AddChild(std::move(label));
        }

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));

        CssPath = std::filesystem::temp_directory_path() /
                  ("ui_text_overflow_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(CssPath);
            f << "#root { display: flex; flex-direction: column; width: 400px; height: 200px; }\n"
              << "#wrap { display: flex; flex-direction: column; }\n"
              << "#lbl { width: " << opt.WidthPx << "px; height: 24px; font-size: 16px; "
              << "white-space: nowrap; overflow: hidden; }\n"
              << ".clip { text-overflow: clip; }\n"
              << ".ell { text-overflow: ellipsis; }\n"
              << opt.ExtraCss << "\n";
        }
        if (!Ui->AttachStyleFromFile(CssPath.string()))
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

    // Content box edges in physical px (content scale 1 here).
    float ContentLeft() const
    {
        const ResolvedStyle& rs = Lbl->GetResolvedStyle();
        return Lbl->GetLayoutX() + rs.Layout.Padding.Left + rs.Layout.BorderWidth.Left;
    }

    float ContentRight() const
    {
        const ResolvedStyle& rs = Lbl->GetResolvedStyle();
        return Lbl->GetLayoutX() + Lbl->GetLayoutWidth() -
               rs.Layout.Padding.Right - rs.Layout.BorderWidth.Right;
    }

    // Leftmost emitted glyph quad, or +inf when nothing was emitted.
    float FirstGlyphX() const
    {
        float minX = std::numeric_limits<float>::infinity();
        for (const auto& g : GlyphPrims())
            minX = std::min(minX, g.X);
        return minX;
    }

    ~EllipsisLabelFixture()
    {
        Rg.reset();
        Ui.reset();
        if (!CssPath.empty())
        {
            std::error_code ec;
            std::filesystem::remove(CssPath, ec);
        }
    }
};

// Both fixtures alive, or the reason to skip. Glyph counts only compare
// between two managers when both shaped the same text with the same atlas.
bool InitPairOrSkipReason(EllipsisLabelFixture& clip, EllipsisLabelFixture& ell,
                          const std::string& text, std::string& whySkip,
                          float widthPx, const char* extraCss)
{
    if (!clip.Init(text, {"clip", widthPx, extraCss, false}) ||
        !ell.Init(text, {"ell", widthPx, extraCss, false}))
    {
        whySkip = "Device init failed";
        return false;
    }
    if (!clip.Ui->GetDefaultFontAtlas() || !ell.Ui->GetDefaultFontAtlas())
    {
        whySkip = "Font atlas not available";
        return false;
    }
    return true;
}

} // namespace

TEST(TextOverflowStyleTests, ResolvesEllipsisAndDefaultsToClip)
{
    EllipsisLabelFixture fx;
    if (!fx.Init(kLongText, {}))
        GTEST_SKIP() << "Device init failed";
    EXPECT_EQ(fx.Lbl->GetResolvedStyle().Visual.TextOverflow, TextOverflowMode::Ellipsis);

    EllipsisLabelFixture defaulted;
    if (!defaulted.Init(kLongText, {"no-such-class", kLabelWidthPx, "", false}))
        GTEST_SKIP() << "Device init failed";
    EXPECT_EQ(defaulted.Lbl->GetResolvedStyle().Visual.TextOverflow, TextOverflowMode::Clip)
        << "unset text-overflow must resolve to the initial clip value";
}

// text-overflow is not in the inherited set: a child of an ellipsized element
// resolves to the initial clip value, so styling a row never silently
// ellipsizes every label inside it.
TEST(TextOverflowStyleTests, IsNotInherited)
{
    EllipsisLabelFixture fx;
    if (!fx.Init(kLongText, {"no-such-class", kLabelWidthPx,
                             "#root { text-overflow: ellipsis; }", false}))
        GTEST_SKIP() << "Device init failed";
    EXPECT_EQ(fx.Lbl->GetResolvedStyle().Visual.TextOverflow, TextOverflowMode::Clip);
    EXPECT_FALSE(fx.Lbl->WasLastRunEllipsized());
}

TEST(TextOverflowEllipsisTests, OverflowingRunIsTruncatedWithMarkInsideTheBox)
{
    EllipsisLabelFixture clip, ell;
    std::string whySkip;
    if (!InitPairOrSkipReason(clip, ell, kLongText, whySkip, kLabelWidthPx, ""))
        GTEST_SKIP() << whySkip;

    const auto clipGlyphs = clip.GlyphPrims();
    const auto ellGlyphs = ell.GlyphPrims();
    ASSERT_GT(clipGlyphs.size(), 0u);
    ASSERT_GT(ellGlyphs.size(), 1u) << "expected a kept prefix plus the ellipsis mark";

    // Clip mode emits the whole run (the GPU clip hides the spill); ellipsis
    // mode emits fewer quads because it truncates on the CPU.
    EXPECT_LT(ellGlyphs.size(), clipGlyphs.size());

    // Every emitted quad — the mark included — ends inside the content box.
    // Slug quads carry sub-pixel dilation margins; one physical pixel of slop.
    const float right = ell.ContentRight() + 1.0f;
    for (const auto& g : ellGlyphs)
        EXPECT_LE(g.X + g.W, right) << "ellipsized run must fit the content box";
}

// The width is the only variable: the same string is truncated and stamped in
// a narrow box and emitted whole and unstamped once the box fits it. Asserting
// only the fitting half would pass with the feature compiled out.
TEST(TextOverflowEllipsisTests, FittingTextEmitsTheFullRunAndNoMark)
{
    EllipsisLabelFixture narrowClip, narrowEll;
    std::string whySkip;
    if (!InitPairOrSkipReason(narrowClip, narrowEll, kMediumText, whySkip, kLabelWidthPx, ""))
        GTEST_SKIP() << whySkip;
    ASSERT_LT(narrowEll.GlyphPrims().size(), narrowClip.GlyphPrims().size())
        << "control: this text must overflow the narrow box";
    ASSERT_TRUE(narrowEll.Lbl->WasLastRunEllipsized());

    EllipsisLabelFixture wideClip, wideEll;
    if (!InitPairOrSkipReason(wideClip, wideEll, kMediumText, whySkip, kFittingWidthPx, ""))
        GTEST_SKIP() << whySkip;
    // Same glyph count as clip mode: no truncation, no "…" appended.
    EXPECT_EQ(wideEll.GlyphPrims().size(), wideClip.GlyphPrims().size());
    EXPECT_FALSE(wideEll.Lbl->WasLastRunEllipsized());
}

// overflow: visible is the gate CSS puts on text-overflow. An element that
// does not contain its overflow keeps painting the full run past its edges.
TEST(TextOverflowEllipsisTests, VisibleOverflowSuppressesTheEllipsis)
{
    EllipsisLabelFixture clip, ell;
    std::string whySkip;
    if (!InitPairOrSkipReason(clip, ell, kLongText, whySkip, kLabelWidthPx,
                              "#lbl { overflow: visible; }"))
        GTEST_SKIP() << whySkip;
    ASSERT_EQ(ell.Lbl->GetResolvedStyle().Layout.Overflow, Overflow::Visible);
    EXPECT_EQ(ell.GlyphPrims().size(), clip.GlyphPrims().size())
        << "overflow: visible must emit the same run clip mode emits";
    EXPECT_FALSE(ell.Lbl->WasLastRunEllipsized());
}

// A wrapping block has no single line to truncate: multi-line text clips, per
// UIStyle.h. The word is wider than the box, so the block still overflows
// after wrapping — only the multi-line gate keeps the mark away.
TEST(TextOverflowEllipsisTests, WrappingRunIsNeverEllipsized)
{
    EllipsisLabelFixture clip, ell;
    std::string whySkip;
    if (!InitPairOrSkipReason(clip, ell, kUnbreakableText, whySkip, kLabelWidthPx,
                              "#lbl { white-space: normal; }"))
        GTEST_SKIP() << whySkip;
    EXPECT_EQ(ell.GlyphPrims().size(), clip.GlyphPrims().size());
    EXPECT_FALSE(ell.Lbl->WasLastRunEllipsized());
}

// Blink aligns the line from its untruncated width and only then truncates, so
// an ellipsized LTR line reads from the start edge whatever text-align says.
// The control is the same alignment at a width that fits: there the line is
// genuinely centred / right-shifted.
TEST(TextOverflowEllipsisTests, EllipsizedLineIsStartAlignedWhateverTextAlignSays)
{
    for (const char* align : {"center", "right"})
    {
        const std::string css = std::string("#lbl { text-align: ") + align + "; }";

        EllipsisLabelFixture narrow;
        if (!narrow.Init(kMediumText, {"ell", kLabelWidthPx, css.c_str(), false}))
            GTEST_SKIP() << "Device init failed";
        if (!narrow.Ui->GetDefaultFontAtlas())
            GTEST_SKIP() << "Font atlas not available";
        ASSERT_TRUE(narrow.Lbl->WasLastRunEllipsized()) << align;
        EXPECT_NEAR(narrow.FirstGlyphX(), narrow.ContentLeft(), 2.0f)
            << "ellipsized line must start at the content-box left edge (" << align << ")";

        EllipsisLabelFixture wide;
        if (!wide.Init(kMediumText, {"ell", kFittingWidthPx, css.c_str(), false}))
            GTEST_SKIP() << "Device init failed";
        ASSERT_FALSE(wide.Lbl->WasLastRunEllipsized()) << align;
        EXPECT_GT(wide.FirstGlyphX(), wide.ContentLeft() + 8.0f)
            << "control: a fitting line is still aligned (" << align << ")";
    }
}

TEST(TextOverflowEllipsisTests, TruncationStampFeedsTheTooltipFallback)
{
    // The tooltip overlay treats a truncated label as its own tooltip via
    // WasLastRunEllipsized; pin the stamp for all three emission shapes.
    EllipsisLabelFixture clip, ell;
    std::string whySkip;
    if (!InitPairOrSkipReason(clip, ell, kLongText, whySkip, kLabelWidthPx, ""))
        GTEST_SKIP() << whySkip;
    EXPECT_TRUE(ell.Lbl->WasLastRunEllipsized());
    EXPECT_FALSE(clip.Lbl->WasLastRunEllipsized());

    EllipsisLabelFixture fitting;
    if (!fitting.Init("Fits", {}))
        GTEST_SKIP() << "Device init failed";
    EXPECT_FALSE(fitting.Lbl->WasLastRunEllipsized());
}

// The truncated label is a last resort: an authored tooltip anywhere above it
// answers first, so a row that named its command keeps saying so on hover.
// Driven through TooltipOverlay::Update, which is the whole public surface —
// one call resolves the text and writes it into the bubble label.
TEST(TextOverflowEllipsisTests, AuthoredAncestorTooltipBeatsTheTruncatedLabel)
{
    EllipsisLabelFixture fx;
    if (!fx.Init(kLongText, {"ell", kLabelWidthPx, "", /*Wrapper=*/true}))
        GTEST_SKIP() << "Device init failed";
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";
    ASSERT_TRUE(fx.Lbl->WasLastRunEllipsized());

    UIElement* root = fx.Ui->GetRootElement();
    ASSERT_NE(root, nullptr);

    UI::Interaction::TooltipOverlay tip;
    tip.Update(root, fx.Lbl, 10.0f, 10.0f, /*time=*/0.0f);
    auto* bubble = dynamic_cast<Label*>(root->FindById("ui-tooltip-text"));
    ASSERT_NE(bubble, nullptr);
    EXPECT_EQ(bubble->GetText(), kLongText)
        << "with no authored tooltip the truncated label speaks for itself";

    constexpr char kAuthored[] = "Save Scene \u00b7 Editor \u00b7 File \u00b7 Ctrl/Cmd+S";
    fx.Wrap->SetTooltip(kAuthored);
    tip.Update(root, fx.Lbl, 10.0f, 10.0f, /*time=*/0.1f);
    EXPECT_EQ(bubble->GetText(), kAuthored)
        << "an ancestor's authored tooltip outranks the truncated label below it";
}

TEST(TextOverflowEllipsisTests, LiveStyleFlipReshapesTheRun)
{
    EllipsisLabelFixture fx;
    if (!fx.Init(kLongText, {"clip"}))
        GTEST_SKIP() << "Device init failed";
    if (!fx.Ui->GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    const size_t fullCount = fx.GlyphPrims().size();
    ASSERT_GT(fullCount, 0u);

    // Flip clip -> ellipsis on the live element. The mode is folded into the
    // shape-cache key, so this must re-shape and re-emit a truncated run
    // rather than replay the cached full one.
    fx.Lbl->RemoveClass("clip");
    fx.Lbl->AddClass("ell");
    for (int i = 0; i < 4; ++i)
        fx.Pump();

    const size_t ellCount = fx.GlyphPrims().size();
    ASSERT_GT(ellCount, 1u);
    EXPECT_LT(ellCount, fullCount);

    // And back: the clip run returns in full.
    fx.Lbl->RemoveClass("ell");
    fx.Lbl->AddClass("clip");
    for (int i = 0; i < 4; ++i)
        fx.Pump();
    EXPECT_EQ(fx.GlyphPrims().size(), fullCount);
}
