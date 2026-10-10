// Per-view-class LOD error budgets: the override layered over the global
// SSE budget, resolved by view class (Rendering::ViewPurpose).
//
// These drive the REAL production function, RenderServices::ComputeViewSseScales
// — the one ShadowMapRenderFeature folds into the cascade static-cache key and
// the one MakeViewLODParams feeds to the scatter — rather than the pure helper
// alone, because the thing that can actually break is the class lookup, not the
// arithmetic.
//
// Expected values are computed from first principles (2*budgetPx/viewportH, the
// definition in MeshLODThresholds.h's header comment) instead of by calling
// LodSseThresholdToCoverage. Deriving the expectation from the function under
// test would pass no matter what that function did.

#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraTypes.h"

#include <gtest/gtest.h>

namespace gr = GameEngine::Rendering;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{
// No view publishes a world-pass height in these tests, so every view falls back
// to kLodSseFallbackViewportH inside the conversion — fixed and known, which is
// what lets the expectations below be written out longhand.
constexpr float kFallbackH = static_cast<float>(gr::kLodSseFallbackViewportH);

float ExpectedScale(float budgetPx)
{
    return 2.0f * budgetPx / kFallbackH;
}

gr::ViewId MakeView(RenderServices& rs, const char* name, gr::ViewPurpose purpose)
{
    return rs.Views().AllocateView(name, rs.Views().AllocateCamera(name), purpose);
}
} // namespace

// --- The pure resolver -------------------------------------------------------

TEST(LodViewBudgetResolve, DisabledReturnsTheGlobalBudgetUnchanged)
{
    gr::LodViewBudgetOverride budget;
    budget.Enabled = false;
    // A stale percent must not leak through a disabled override: this is the
    // difference between "disabled" and "100%", and only this asserts it.
    budget.BudgetPercent = 50.0f;
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget), 10.0f);
}

TEST(LodViewBudgetResolve, EnabledScalesByPercent)
{
    gr::LodViewBudgetOverride budget;
    budget.Enabled = true;
    budget.BudgetPercent = 50.0f;
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget), 5.0f);
    budget.BudgetPercent = 200.0f;
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget), 20.0f);
    budget.BudgetPercent = 100.0f;
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget), 10.0f);
}

TEST(LodViewBudgetResolve, PercentClampsToTheSupportedRange)
{
    gr::LodViewBudgetOverride budget;
    budget.Enabled = true;
    budget.BudgetPercent = 1.0f; // below kMinLodBudgetPercent
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget),
                    10.0f * gr::kMinLodBudgetPercent / 100.0f);
    budget.BudgetPercent = 100000.0f; // above kMaxLodBudgetPercent
    EXPECT_FLOAT_EQ(gr::ResolveLodBudgetPx(10.0f, budget),
                    10.0f * gr::kMaxLodBudgetPercent / 100.0f);
}

// --- Through ComputeViewSseScales -------------------------------------------

TEST(LodViewBudgetScales, NoOverrideSpendsTheGlobalBudgetInEveryClass)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);

    const gr::ViewId game = MakeView(rs, "Game", gr::ViewPurpose::Game);
    const gr::ViewId scene = MakeView(rs, "Scene", gr::ViewPurpose::EditorScene);

    // A fresh RenderServices must be byte-identical to the pre-override path:
    // all-disabled is the same renderer as no overrides at all.
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(game).Default, ExpectedScale(10.0f));
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(scene).Default, ExpectedScale(10.0f));
}

TEST(LodViewBudgetScales, DisabledOverrideFallsThroughToTheGlobalBudget)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);
    const gr::ViewId game = MakeView(rs, "Game", gr::ViewPurpose::Game);

    // Stored but off, with a percent that WOULD change the result if honoured.
    gr::LodViewBudgetOverride budget;
    budget.Enabled = false;
    budget.BudgetPercent = 200.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, budget);

    // MUTATION THAT MUST FAIL THIS: force-enable the override (drop the
    // `if (!classOverride.Enabled)` early-out in ResolveLodBudgetPx) and this
    // becomes ExpectedScale(20.0f).
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(game).Default, ExpectedScale(10.0f));
}

TEST(LodViewBudgetScales, PercentScalesExactlyTheTargetedClass)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);

    const gr::ViewId game = MakeView(rs, "Game", gr::ViewPurpose::Game);
    const gr::ViewId scene = MakeView(rs, "Scene", gr::ViewPurpose::EditorScene);
    const gr::ViewId preview = MakeView(rs, "Preview", gr::ViewPurpose::EditorPreview);

    gr::LodViewBudgetOverride budget;
    budget.Enabled = true;
    budget.BudgetPercent = 50.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, budget);

    // MUTATION THAT MUST FAIL THIS: apply the override to every class (index
    // the table with a constant instead of desc->purpose) and the Scene and
    // Preview expectations below break while the Game one still passes.
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(game).Default, ExpectedScale(5.0f));
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(scene).Default, ExpectedScale(10.0f));
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(preview).Default, ExpectedScale(10.0f));
}

TEST(LodViewBudgetScales, TwoClassesCarryDifferentBudgetsAtOnce)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);

    const gr::ViewId game = MakeView(rs, "Game", gr::ViewPurpose::Game);
    const gr::ViewId scene = MakeView(rs, "Scene", gr::ViewPurpose::EditorScene);

    gr::LodViewBudgetOverride coarse;
    coarse.Enabled = true;
    coarse.BudgetPercent = 200.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, coarse);

    gr::LodViewBudgetOverride fine;
    fine.Enabled = true;
    fine.BudgetPercent = 50.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::EditorScene, fine);

    // The shipped case: Game View coarser than Scene View in one session.
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(game).Default, ExpectedScale(20.0f));
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(scene).Default, ExpectedScale(5.0f));
}

TEST(LodViewBudgetScales, TheTightSkinnedScaleTracksTheOverrideToo)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);
    rs.SetLODSkinnedBudgetScale(0.5f);
    const gr::ViewId game = MakeView(rs, "Game", gr::ViewPurpose::Game);

    gr::LodViewBudgetOverride budget;
    budget.Enabled = true;
    budget.BudgetPercent = 50.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, budget);

    // The override scales the budget the skinned multiplier then tightens, so
    // characters keep their relationship to props inside the overridden class.
    const RenderServices::ViewSseScales scales = rs.ComputeViewSseScales(game);
    EXPECT_FLOAT_EQ(scales.Default, ExpectedScale(5.0f));
    EXPECT_FLOAT_EQ(scales.Tight, ExpectedScale(2.5f));
}

TEST(LodViewBudgetScales, AnUnregisteredViewSpendsTheGlobalBudget)
{
    RenderServices rs;
    rs.SetLODErrorBudgetPx(10.0f);

    gr::LodViewBudgetOverride budget;
    budget.Enabled = true;
    budget.BudgetPercent = 400.0f;
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, budget);

    // No view with this id was ever allocated. ViewPurpose::Game is the ViewDesc
    // default, so a lookup that silently defaulted instead of reporting "absent"
    // would apply the Game override to a view that has no class at all.
    constexpr gr::ViewId kNeverAllocated = 4242u;
    EXPECT_FLOAT_EQ(rs.ComputeViewSseScales(kNeverAllocated).Default, ExpectedScale(10.0f));
}
