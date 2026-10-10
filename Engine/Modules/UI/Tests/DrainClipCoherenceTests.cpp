#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

// A position element with overflow:hidden owns a clip slot that masks its
// children. Scrolling a ScrollView moves that owner without re-solving Yoga:
// UIManager translates the scrolled subtree's committed rects and queues it for
// the primitive drain (drain-only, no full regen — see ApplyScrollTransforms /
// TranslateLayoutSubtree). On that drain frame, DrainPrimitiveDataDirty must
// rewrite the owner's clip slot in place from the translated rect (the drain
// clip-slot refresh in UIManager_PrimitiveGen.cpp). Without it, the owner clips
// its children against the pre-scroll rect — the same "clip lags the moved
// content on drain frames" failure the picker hover-lift shave belonged to.
//
// This is the frame type a headless layout change cannot reach (a `top`/class
// change re-solves and full-regens, re-pushing every clip via PushClip); scroll
// translation is the one path that mutates a clip owner's rect on a pure drain.
namespace
{
struct ScrollClipFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    ScrollView* Sv = nullptr;
    UIElement* Clip = nullptr;
    UIElement* Inner = nullptr;

    bool Init()
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto sv = std::make_unique<ScrollView>();
        Sv = sv.get();
        sv->SetId("sv");
        root->AddChild(std::move(sv));

        // Scroll content: a spacer keeps the clip owner off the viewport top
        // (so a modest scroll never clamps it against the viewport clip), the
        // overflow:hidden owner with an overflowing child, then a tall filler so
        // the content exceeds the viewport and can actually scroll.
        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("spacer");
        Sv->GetViewport()->AddChild(std::move(spacer));

        auto clip = std::make_unique<UIElement>();
        Clip = clip.get();
        clip->SetId("clip");
        clip->AddClass("clipbox");
        auto inner = std::make_unique<UIElement>();
        Inner = inner.get();
        inner->SetId("inner");
        inner->AddClass("inner");
        clip->AddChild(std::move(inner));
        Sv->GetViewport()->AddChild(std::move(clip));

        auto filler = std::make_unique<UIElement>();
        filler->AddClass("filler");
        Sv->GetViewport()->AddChild(std::move(filler));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_drain_clip_scroll.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 220px; height: 220px; }\n"
                 "#sv { width: 220px; height: 120px; }\n"
                 ".spacer { width: 100px; height: 60px; flex-shrink: 0; }\n"
                 // Owner: overflow:hidden + a child => owns a persistent clip slot;
                 // a background gives it a primitive range.
                 ".clipbox { display: flex; flex-direction: column; width: 100px; height: 40px; "
                 "overflow: hidden; background-color: #202020; flex-shrink: 0; }\n"
                 ".inner { width: 100px; height: 100px; background-color: #808080; flex-shrink: 0; }\n"
                 ".filler { width: 100px; height: 400px; flex-shrink: 0; }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 3; ++i)
            Pump();
        return true;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    ~ScrollClipFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

constexpr float kScrollPx = 30.0f;
} // namespace

TEST(DrainClipCoherenceTests, OverflowClipTracksScrollTranslationOnDrainFrame)
{
    ScrollClipFixture fx;
    if (!fx.Init())
        GTEST_SKIP() << "no headless GPU device";

    // Settled baseline: the owner owns a clip slot at its resting rect.
    ASSERT_NE(fx.Clip->m_ClipSlotIdx, UI::kNoClip) << "overflow:hidden owner must own a clip slot";
    const uint16_t slot = fx.Clip->m_ClipSlotIdx;
    const UI::UIClipRect* baseClip = fx.Ui->PeekClipRectForTesting(slot);
    ASSERT_NE(baseClip, nullptr);
    const float baseClipY = baseClip->Rect[1];
    const float baseLayoutY = fx.Clip->GetLayoutY();
    EXPECT_NEAR(baseClipY, baseLayoutY, 0.5f) << "resting clip must sit at the owner's rect";

    // Scroll down: the scroll-content subtree (owner included) translates up by
    // kScrollPx and drains — no Yoga solve, no full regen.
    fx.Sv->SetScrollY(kScrollPx);
    fx.Pump();
    ASSERT_NEAR(fx.Sv->GetScrollY(), kScrollPx, 0.5f) << "content must have scrolled";

    // The translation moved the owner's committed rect up by kScrollPx...
    EXPECT_NEAR(fx.Clip->GetLayoutY(), baseLayoutY - kScrollPx, 0.5f)
        << "scroll translation must move the owner's committed rect";

    // ...and the drain must have moved its clip slot with it. A stale clip
    // (drain skipped the slot rewrite) would still read baseClipY here and shave
    // the top kScrollPx of the owner's children.
    ASSERT_EQ(fx.Clip->m_ClipSlotIdx, slot) << "clip slot is persistent across the drain";
    const UI::UIClipRect* scrolledClip = fx.Ui->PeekClipRectForTesting(slot);
    ASSERT_NE(scrolledClip, nullptr);
    EXPECT_NEAR(scrolledClip->Rect[1], baseClipY - kScrollPx, 0.5f)
        << "clip slot must track the scrolled owner rect, not lag at the pre-scroll position";

    // Sanity: the clipped child's baked primitive moved with the owner, so the
    // failure mode under test is a lagging clip, not stationary content.
    const UI::UIPrimitive* innerPrim = fx.Ui->PeekPrimitiveForTesting(*fx.Inner, 0);
    ASSERT_NE(innerPrim, nullptr) << "the clipped child emits a background primitive";
    EXPECT_NEAR(innerPrim->Y, baseLayoutY - kScrollPx, 0.5f) << "child content scrolled with the owner";
}
