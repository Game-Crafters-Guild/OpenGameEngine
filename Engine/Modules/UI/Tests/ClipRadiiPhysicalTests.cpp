// A clip slot's rect and its corner radii are consumed in ONE space.
//
// ui_sdf.frag's computeClipAlpha evaluates `roundedRectSDF(p, clip.rect,
// clip.radii, clip.radiiY)` with `p` the primitive's physical-px local position,
// radii carrying the per-corner semi-axes, so the two
// halves of a UIClipRect have to agree: a rect in physical px carrying radii in
// CSS-logical px describes a corner the size CSS asked for divided by the
// content scale. At 200% the mask's rounding is half what the element's own
// painted background shows, and the two disagree by more the higher the DPI.
//
// The numbers below are not read back from the engine. They are `border-radius`
// as authored, times the fixture's content scale, per the same logical->physical
// mapping the element's background rect already uses (UIManager_PrimitiveGen
// scales vs.BorderRadius by ctx.ContentScale when it emits the Rect primitive).
// Every case keeps the expected radius well under half the clip rect's smaller
// side, because roundedRectSDF clamps `r` to that half-extent internally — a
// radius chosen at or above it would be clamped to the same value in both
// spaces and prove nothing.

#include "FixedScalePlatform.h"
#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Rendering/Core/Device.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::UITesting::FixedScalePlatform;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// The authored radius, shared by every case here.
constexpr float kAuthoredRadiusPx = 8.0f;
constexpr float kAuthoredBorderPx = 2.0f;
constexpr float kRadiusTolerance = 0.01f;

// A parent with children owns a clip slot masking at its padding box with
// inner radii (InsetToPaddingBox + InnerClipRadii), pushed eagerly from
// GeneratePrimitivesForElement (ClipSlotPushedForChildren).
constexpr char kParentXml[] = R"(<uielement id="root">
  <uielement id="clipper">
    <uielement id="child"/>
  </uielement>
</uielement>)";

constexpr char kParentCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#clipper { width: 100px; height: 60px; overflow: hidden; border-radius: 8px;
           background-color: #202020; flex-shrink: 0; }
#child { width: 200px; height: 200px; background-color: #808080; flex-shrink: 0; }
)";

constexpr char kLargeClipXml[] = R"(<uielement id="root">
  <uielement id="clipper">
    <uielement id="child"/>
  </uielement>
</uielement>)";

constexpr char kLargeClipCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#clipper { width: 700px; height: 500px; overflow: hidden; border-radius: 8px;
           background-color: #202020; flex-shrink: 0; }
#child { width: 700px; height: 500px; background-color: #808080; flex-shrink: 0; }
)";

// A childless label masks at its PADDING box instead (InsetToPaddingBox +
// InnerClipRadii), and only once its own glyphs actually spill the box.
constexpr char kLeafXml[] = R"(<uielement id="root">
  <label id="leaf">overflowing label text that cannot possibly fit</label>
</uielement>)";

constexpr char kLeafCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#leaf { width: 60px; height: 20px; overflow: hidden; border-radius: 8px;
        border-width: 2px; border-color: #808080; border-style: solid;
        font-size: 14px; flex-shrink: 0; }
)";

// The persistent clip slot `id` owns, or nullptr when it owns none.
const UI::UIClipRect* ClipSlotOf(const IsolatedUIFixture& fx, const std::string& id)
{
    UIElement* root = fx.Manager().GetRootElement();
    UIElement* el = root ? root->FindById(id) : nullptr;
    if (!el || el->m_ClipSlotIdx == UI::kNoClip)
        return nullptr;
    return fx.Manager().PeekClipRectForTesting(el->m_ClipSlotIdx);
}

void ExpectAllRadii(const UI::UIClipRect& cr, float expected, const char* what)
{
    EXPECT_NEAR(cr.Radii[0], expected, kRadiusTolerance) << what << " (top-left)";
    EXPECT_NEAR(cr.Radii[1], expected, kRadiusTolerance) << what << " (top-right)";
    EXPECT_NEAR(cr.Radii[2], expected, kRadiusTolerance) << what << " (bottom-right)";
    EXPECT_NEAR(cr.Radii[3], expected, kRadiusTolerance) << what << " (bottom-left)";
}

// Scroll translation is the one path that moves a clip owner's rect on a pure
// drain frame (no Yoga solve, no full regen), so it is the only way to reach the
// drain's in-place clip-slot rewrite — a second writer of the same four floats,
// which can therefore disagree with the push path independently. Run at a
// content scale of 2 so a logical-px radius is distinguishable from a physical
// one; DrainClipCoherenceTests covers the rect at scale 1.
struct ScaledDrainFixture
{
    // SetPlatform stores a non-owning pointer: the stub must outlive the manager.
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    std::unique_ptr<FixedScalePlatform> Platform;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    ScrollView* Sv = nullptr;
    UIElement* Clip = nullptr;

    bool Init(float contentScale)
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

        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("spacer");
        Sv->GetViewport()->AddChild(std::move(spacer));

        auto clip = std::make_unique<UIElement>();
        Clip = clip.get();
        clip->SetId("clip");
        clip->AddClass("clipbox");
        auto inner = std::make_unique<UIElement>();
        inner->SetId("inner");
        inner->AddClass("inner");
        clip->AddChild(std::move(inner));
        Sv->GetViewport()->AddChild(std::move(clip));

        auto filler = std::make_unique<UIElement>();
        filler->AddClass("filler");
        Sv->GetViewport()->AddChild(std::move(filler));

        Platform = std::make_unique<FixedScalePlatform>(contentScale);
        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetPlatform(Platform.get());
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_clip_radii_physical.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 220px; height: 220px; }\n"
                 "#sv { width: 220px; height: 120px; }\n"
                 ".spacer { width: 100px; height: 60px; flex-shrink: 0; }\n"
                 ".clipbox { display: flex; flex-direction: column; width: 100px; height: 40px; "
                 "overflow: hidden; border-radius: 8px; background-color: #202020; flex-shrink: 0; }\n"
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

    ~ScaledDrainFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

constexpr float kDrainContentScale = 2.0f;
constexpr float kScrollPx = 30.0f;

} // namespace

// The children-overflow push (GeneratePrimitivesForElement). Its rect is the
// element rect already converted to physical px, so its radii must be too.
TEST(ClipRadiiPhysical, ChildClipRadiiScaleWithContentScale)
{
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kParentXml, kParentCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();

        const UI::UIClipRect* cr = ClipSlotOf(fx, "clipper");
        ASSERT_NE(cr, nullptr) << "overflow:hidden parent must own a clip slot";
        ExpectAllRadii(*cr, kAuthoredRadiusPx, "clip radii at content scale 1");
    }

    IsolatedUIFixture fx;
    const bool built = fx.Build(2.0f, kParentXml, kParentCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const UI::UIClipRect* cr = ClipSlotOf(fx, "clipper");
    ASSERT_NE(cr, nullptr) << "overflow:hidden parent must own a clip slot";

    // The rect is unambiguously physical, so it fixes the space the radii have
    // to be read in: a 100x60 CSS box measures 200x120 here.
    EXPECT_NEAR(cr->Rect[2], 200.0f, kRadiusTolerance);
    EXPECT_NEAR(cr->Rect[3], 120.0f, kRadiusTolerance);
    ExpectAllRadii(*cr, kAuthoredRadiusPx * 2.0f, "clip radii at content scale 2");
}

TEST(ClipRadiiPhysical, LargePanelKeepsAuthoredPixelRadius)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kLargeClipXml, kLargeClipCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const UI::UIClipRect* cr = ClipSlotOf(fx, "clipper");
    ASSERT_NE(cr, nullptr) << "overflow:hidden parent must own a clip slot";
    EXPECT_NEAR(cr->Rect[2], 700.0f, 0.5f);
    EXPECT_NEAR(cr->Rect[3], 500.0f, 0.5f);
    ExpectAllRadii(*cr, kAuthoredRadiusPx,
                   "8px clip radius must not grow with a 700x500 panel");
}

// The element's own painted corner and its clip's corner are the same CSS
// corner. Whatever space each is expressed in, one radius cannot be twice the
// other on the same element.
TEST(ClipRadiiPhysical, ChildClipRadiiMatchTheElementsOwnPaintedCorner)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(2.0f, kParentXml, kParentCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const auto rects = fx.Primitives("clipper", UI::PrimitiveMode::Rect);
    ASSERT_FALSE(rects.empty()) << "the clipper paints a background rect";

    const UI::UIClipRect* cr = ClipSlotOf(fx, "clipper");
    ASSERT_NE(cr, nullptr);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_NEAR(cr->Radii[i], rects.front().Radii[i], kRadiusTolerance)
            << "clip corner " << i << " must match the background corner it masks";
    }
}

// The own-text overflow push (EmitTextPrimitives). Its rect went through
// InsetToPaddingBox and is physical; the radii come from InnerClipRadii, which
// subtracts the CSS-logical border from the CSS-logical radius, so the result
// still needs scaling.
TEST(ClipRadiiPhysical, LeafOwnTextClipRadiiAreInnerAndPhysical)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(2.0f, kLeafXml, kLeafCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    if (!fx.Manager().GetDefaultFontAtlas())
        GTEST_SKIP() << "Font atlas not available";

    const UI::UIClipRect* cr = ClipSlotOf(fx, "leaf");
    ASSERT_NE(cr, nullptr) << "overflowing label text must allocate the element's clip slot";

    // Padding box of a 60x20 CSS box with a 2px border, in physical px.
    EXPECT_NEAR(cr->Rect[2], (60.0f - 2.0f * kAuthoredBorderPx) * 2.0f, kRadiusTolerance);
    EXPECT_NEAR(cr->Rect[3], (20.0f - 2.0f * kAuthoredBorderPx) * 2.0f, kRadiusTolerance);

    ExpectAllRadii(*cr, (kAuthoredRadiusPx - kAuthoredBorderPx) * 2.0f,
                   "padding-box clip radii at content scale 2");
}

// The drain's in-place slot rewrite is the second writer of the same four
// floats. The scroll assertion below is the instrument check: it proves the
// rewrite actually ran on this frame, so a passing radius assertion cannot be
// the push path's value surviving untouched.
TEST(ClipRadiiPhysical, DrainSlotRewriteKeepsClipRadiiPhysical)
{
    ScaledDrainFixture fx;
    const bool inited = fx.Init(kDrainContentScale);
    if (!fx.Dev)
        GTEST_SKIP() << "no headless GPU device";
    ASSERT_TRUE(inited);

    ASSERT_NE(fx.Clip->m_ClipSlotIdx, UI::kNoClip) << "overflow:hidden owner must own a clip slot";
    const uint16_t slot = fx.Clip->m_ClipSlotIdx;
    const UI::UIClipRect* baseClip = fx.Ui->PeekClipRectForTesting(slot);
    ASSERT_NE(baseClip, nullptr);
    const float baseClipY = baseClip->Rect[1];

    fx.Sv->SetScrollY(kScrollPx);
    fx.Pump();
    ASSERT_NEAR(fx.Sv->GetScrollY(), kScrollPx, 0.5f) << "content must have scrolled";

    const UI::UIClipRect* scrolled = fx.Ui->PeekClipRectForTesting(fx.Clip->m_ClipSlotIdx);
    ASSERT_NE(scrolled, nullptr);
    ASSERT_NEAR(scrolled->Rect[1], baseClipY - kScrollPx * kDrainContentScale, 1.0f)
        << "instrument check: the drain must have rewritten this slot's rect";

    ExpectAllRadii(*scrolled, kAuthoredRadiusPx * kDrainContentScale,
                   "clip radii after the drain's slot rewrite");
}
