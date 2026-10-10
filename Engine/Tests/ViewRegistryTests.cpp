#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraTypes.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

// Device-free equivalence locks for the camera/view registries (A1.2 L5).
// Written against the CURRENT RenderServices public surface so the S1
// extraction (state + bodies move behind unchanged forwarders) leaves them
// untouched — they never re-point. No device is created: every registry
// operation exercised here is pure CPU bookkeeping (ReleaseView's GPU
// teardown is a no-op with a null device).

// L5(a): GetViews() iterates in allocation order, and that order survives
// interleaved allocate/release (the vector storage + append-on-allocate +
// stable erase drive per-view pass declaration order downstream).
TEST(ViewRegistryTests, GetViewsIterationOrderIsAllocationOrder)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId v1 = rs.Views().AllocateView("v1", cam);
    const ViewId v2 = rs.Views().AllocateView("v2", cam);
    const ViewId v3 = rs.Views().AllocateView("v3", cam);

    ASSERT_TRUE(rs.Views().ReleaseView(v2));
    const ViewId v4 = rs.Views().AllocateView("v4", cam);

    const auto& views = rs.Views().GetViews();
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[0].id, v1);
    EXPECT_EQ(views[1].id, v3);
    EXPECT_EQ(views[2].id, v4);
}

// L5(b): ReleaseCamera refuses while a view still references the camera, and
// succeeds once the view is released.
TEST(ViewRegistryTests, ReleaseCameraRefusedWhileViewReferencesIt)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    EXPECT_FALSE(rs.Views().ReleaseCamera(cam)) << "camera still referenced by a live view";

    ASSERT_TRUE(rs.Views().ReleaseView(view));
    EXPECT_TRUE(rs.Views().ReleaseCamera(cam)) << "camera releasable once no view references it";
}

// L5(c): ReleaseCamera erases the per-camera side tables (exposure + post-
// process mask). CameraIds are increment-only (no recycling path exists), so
// this erase is defense-in-depth (A3-iii): the test pins the erase-on-release,
// not recycling protection.
TEST(ViewRegistryTests, ReleaseCameraErasesPerCameraSideTables)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");

    ViewRegistry::CameraExposure exposure{};
    exposure.Mode = 1;
    rs.Views().SetCameraExposure(cam, exposure);
    rs.Views().SetCameraPostProcessMask(cam, 0x2u);

    ASSERT_NE(rs.Views().FindCameraExposure(cam), nullptr);
    EXPECT_TRUE(rs.Views().HasCameraPostProcessMask(cam));

    ASSERT_TRUE(rs.Views().ReleaseCamera(cam));

    EXPECT_EQ(rs.Views().FindCameraExposure(cam), nullptr);
    EXPECT_FALSE(rs.Views().HasCameraPostProcessMask(cam));
}

// L5(d) — S0 subset: ReleaseView succeeds exactly once (double-release is a
// no-op), and after release the view descriptor is gone and per-view knob
// getters fall back to defaults. (The A1 release-erase assertions —
// forcedLOD/pp/fog/warned + callback fire-count — land in S1 when the fix does.)
TEST(ViewRegistryTests, ReleaseViewIsIdempotentAndDropsPerViewState)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    ViewLetterbox lb{};
    lb.active = true;
    lb.width = 640;
    lb.height = 360;
    rs.Views().SetViewLetterbox(view, lb);
    EXPECT_TRUE(rs.Views().GetViewLetterbox(view).active);

    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "first release succeeds";
    EXPECT_FALSE(rs.Views().ReleaseView(view)) << "double-release is a no-op";

    EXPECT_EQ(rs.Views().FindViewDesc(view), nullptr);
    EXPECT_FALSE(rs.Views().GetViewLetterbox(view).active) << "per-view knob defaults after release";
}

// L5(d) — A1 extension (S1): ReleaseView additionally erases the per-view
// override maps that historically leaked (insert-only), and fires the view-
// released callback exactly once per successful release. After release the
// post-process override for the (now-defunct) view is gone, so the effective
// settings fall back to the world value. Double-release is a no-op that does
// not fire the callback.
TEST(ViewRegistryTests, ReleaseViewErasesOverridesAndFiresCallbackOnce)
{
    RenderServices rs;

    int releaseCount = 0;
    rs.Views().SetViewReleasedCallback([&](ViewId) { ++releaseCount; });

    const uint64_t worldId = 7ull;
    PostProcessSettings worldPp{};
    worldPp.Exposure = 2.0f;
    rs.SetWorldPostProcessSettings(worldId, worldPp);

    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("view", cam);

    PostProcessSettings viewPp{};
    viewPp.Exposure = 5.0f;
    rs.Views().SetViewPostProcessOverride(view, viewPp);
    EXPECT_FLOAT_EQ(rs.GetEffectivePostProcessSettings(view, worldId).Exposure, 5.0f)
        << "per-view override wins while set";

    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "first release succeeds";
    EXPECT_EQ(releaseCount, 1) << "released callback fires exactly once on success";

    // A1: the post-process override was erased with the rest of the per-view
    // state, so the effective settings fall back to the world value.
    EXPECT_FLOAT_EQ(rs.GetEffectivePostProcessSettings(view, worldId).Exposure, 2.0f)
        << "per-view post-process override erased on release (A1)";

    EXPECT_FALSE(rs.Views().ReleaseView(view)) << "double-release is a no-op";
    EXPECT_EQ(releaseCount, 1) << "double-release does not fire the callback";
}

// L5(e): an OnDemand view starts expired — ActiveRenderLayerMask() is 0 until
// RequestViewFrame arms it (the mechanism probe capture / hidden panes lean on
// to lapse out of GPU work without a ReleaseView).
TEST(ViewRegistryTests, OnDemandViewStartsExpiredUntilArmed)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, 1u);

    const ViewDesc* dormant = rs.Views().FindViewDesc(view);
    ASSERT_NE(dormant, nullptr);
    EXPECT_EQ(dormant->ActiveRenderLayerMask(), 0u) << "unarmed OnDemand view is dormant";

    rs.Views().RequestViewFrame(view);

    const ViewDesc* armed = rs.Views().FindViewDesc(view);
    ASSERT_NE(armed, nullptr);
    EXPECT_NE(armed->ActiveRenderLayerMask(), 0u) << "armed OnDemand view participates";
}

// A1.6 (D9) hide -> lapse: when a scene/game pane hides, the editor routing
// STOPS re-arming the view but does NOT release it. Modeled device-free: after
// one armed (visible) frame, two BeginFrame decrements with no re-arm drive
// participationFrames to 0. The view stays registered (viewId survives) and
// ActiveRenderLayerMask() gates to 0 within two frames, so extraction / batch-
// key build / culling / shadows cost nothing while hidden — and nothing re-arms
// it on its own.
TEST(ViewRegistryTests, A16_HideLapsesOnDemandViewWithoutReleasing)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, 1u);

    // Visible frame: the pane declares, arming the view for this frame + the next.
    rs.Views().RequestViewFrame(view);
    ASSERT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), 0u)
        << "armed OnDemand view participates while visible";

    // Frame 1 after hide (no re-arm): still fed by the prior request.
    rs.Views().BeginFrame();
    EXPECT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), 0u)
        << "still participates one frame after hide (request covers this frame)";

    // Frame 2 after hide: lapses to dormant. View is NOT released — it is still
    // registered, just contributing zero per-frame work.
    rs.Views().BeginFrame();
    const ViewDesc* lapsed = rs.Views().FindViewDesc(view);
    ASSERT_NE(lapsed, nullptr) << "hidden view stays registered (release is destroy-only)";
    EXPECT_EQ(lapsed->ActiveRenderLayerMask(), 0u) << "lapsed within two frames of hide";

    // A further frame with no re-arm stays dormant — nothing self-arms it.
    rs.Views().BeginFrame();
    const ViewDesc* stillDormant = rs.Views().FindViewDesc(view);
    ASSERT_NE(stillDormant, nullptr);
    EXPECT_EQ(stillDormant->ActiveRenderLayerMask(), 0u)
        << "no re-arm while hidden: stays dormant, no orphan cull loop";
}

// A1.6 (D9) hide -> quick reshow: because a hidden pane lapses instead of
// releasing, the SAME viewId is still registered when the pane reopens. The
// show path reuses it (SceneView/GameView DeclareTargetsRG only Allocate when
// m_ViewId == 0), so a single RequestViewFrame re-arms participation and the
// view renders the next frame — the warm reopen (viewId + HZB history intact).
TEST(ViewRegistryTests, A16_QuickReshowReusesSameViewIdAndRearms)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().SetViewRenderLayerMask(view, 1u);

    // Visible, then hide -> lapse to dormant (two decrements, no re-arm).
    rs.Views().RequestViewFrame(view);
    rs.Views().BeginFrame();
    rs.Views().BeginFrame();
    ASSERT_EQ(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), 0u)
        << "dormant after hide";

    // Reshow: the controller declares again and re-arms. It never re-allocated,
    // so the id is byte-identical to the pre-hide id (warm reopen).
    rs.Views().RequestViewFrame(view);
    const ViewDesc* rearmed = rs.Views().FindViewDesc(view);
    ASSERT_NE(rearmed, nullptr);
    EXPECT_EQ(rearmed->id, view) << "same viewId across hide/reshow (never released)";
    EXPECT_NE(rearmed->ActiveRenderLayerMask(), 0u) << "reshow re-arms participation";

    // Next frame's extraction is fed: still participating after one decrement.
    rs.Views().BeginFrame();
    EXPECT_NE(rs.Views().FindViewDesc(view)->ActiveRenderLayerMask(), 0u)
        << "reshown view renders the next frame";
}

// A1.6 (D9) destroy: release-on-destroy stays mandatory. The controller
// destructor path (DeactivateRenderView -> ReleaseView) actually removes the
// view from the registry, unlike hide which only lapses it.
TEST(ViewRegistryTests, A16_DestroyReleasesViewFromRegistry)
{
    RenderServices rs;
    const CameraId cam = rs.Views().AllocateCamera("cam");
    const ViewId view = rs.Views().AllocateView("onDemand", cam, ViewPurpose::EditorScene,
                                        ViewParticipation::OnDemand);
    rs.Views().RequestViewFrame(view);
    ASSERT_NE(rs.Views().FindViewDesc(view), nullptr) << "view registered while alive";

    // Controller teardown releases the view (and then its camera).
    EXPECT_TRUE(rs.Views().ReleaseView(view)) << "destroy releases the view";
    EXPECT_EQ(rs.Views().FindViewDesc(view), nullptr) << "view gone from registry after destroy";
    EXPECT_TRUE(rs.Views().ReleaseCamera(cam)) << "camera releasable once its view is gone";
}
