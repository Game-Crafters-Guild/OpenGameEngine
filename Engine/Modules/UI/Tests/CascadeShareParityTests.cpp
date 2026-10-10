// Cascade sibling-share parity — focused regression guard for the Mount-portal
// donor-sharing invariant. During a build the direct children cascade in order
// through the SHARED cascade context: the first element of a share group (the
// donor) resolves fully and publishes its snapshot into the shared cache; every
// later same-key sibling copies that snapshot instead of re-resolving (P4d).
//
// A Mount portal target is built AFTER the host's children with the same shared
// context and can carry the same cascade DfsParent (hence the same share key)
// as those children. Because the donor is already published in the shared cache
// by the time the target is built, the target must share it too — contributing
// exactly one additional CascadeShared.
//
// This is a differential test: build the same Mount host twice — once without a
// portal target, once with a same-key target — and assert the target adds
// exactly one shared cascade. The EXPECT_GT guard makes a broken/no-op setup
// (no sharing at all) fail loudly rather than pass vacuously.
#include "UI/Controls/Mount.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "Rendering/Core/Device.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Two rowa/rowb groups (each with several members so each group has a donor plus
// at least one sharee) under a Mount host, plus an optional rowa portal target
// (same DfsParent == host, same share key as the rowa children). Returns
// CascadeShared for the all-fresh build. `keepTargetAlive` owns the non-owned
// Mount target for the call's life.
uint32_t BuildAndGetCascadeShared(IDevice* dev, bool withTarget,
                                  std::unique_ptr<UIElement>& keepTargetAlive)
{
    constexpr int kPer = 4; // >= 2 per group so a donor + sharee exist

    auto root = std::make_unique<UIElement>();
    root->AddClass("host-root");
    auto mountOwner = std::make_unique<Mount>();
    Mount* mount = mountOwner.get();
    mount->AddClass("host");
    root->AddChild(std::move(mountOwner));
    for (int i = 0; i < kPer; ++i)
    {
        auto c = std::make_unique<UIElement>();
        c->AddClass("rowa");
        mount->AddChild(std::move(c));
    }
    for (int i = 0; i < kPer; ++i)
    {
        auto c = std::make_unique<UIElement>();
        c->AddClass("rowb");
        mount->AddChild(std::move(c));
    }

    UIManager ui(dev);
    ui.SetSubtreeSkipEnabled(true);
    ui.SetUpdateProfilingEnabled(true);
    ui.SetRoot(std::move(root));

    if (withTarget)
    {
        // The portal target must share the rowa children's key: same cascade
        // DfsParent (== host, set by SetTarget) AND no ':root' state bit. The
        // ':root' bit is set for a parentless element (GetParent()==nullptr), so
        // a bare detached target would NOT match the parented children. Give it
        // a detached holder as its real parent (never traversed — not under
        // root) so GetParent() is non-null while GetDfsParent() stays the mount.
        keepTargetAlive = std::make_unique<UIElement>(); // holder (not traversed)
        auto tgtOwner = std::make_unique<UIElement>();
        tgtOwner->AddClass("rowa");
        UIElement* tgt = tgtOwner.get();
        keepTargetAlive->AddChild(std::move(tgtOwner));  // tgt.GetParent() = holder
        // The mount is owned by `ui` (SetRoot propagated ownership); SetTarget
        // adopts the owner and sets tgt's cascade DfsParent to the host. tgt is
        // visited exactly once, via the Mount portal.
        Mount* m = static_cast<Mount*>(ui.GetRootElement()->GetChildren()[0].get());
        m->SetTarget(tgt);
    }

    // Non-empty, non-share-sensitive class rules — required for share
    // eligibility (an empty rule-index set disqualifies sharing outright).
    const auto css = std::filesystem::temp_directory_path() / "cascade_share_parity_test.css";
    {
        std::ofstream f(css);
        f << ".host-root{width:200px;height:200px;} .host{width:100px;}"
             " .rowa{color:#ff0000;} .rowb{color:#00ff00;}\n";
    }
    ui.AttachStyleFromFile(css.string());

    // Single all-fresh frame: every element (including the portal target) is a
    // fresh node, so all cascade and the target participates deterministically.
    ui.Update(0.0f, /*interactive=*/true);

    UIManager::UpdateProfileFrame pf{};
    EXPECT_TRUE(ui.GetLastUpdateProfileFrame(pf));
    return pf.CascadeShared;
}
} // namespace

TEST(CascadeShareParityTests, MountTargetSharesDonorFromSharedCache)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    std::unique_ptr<UIElement> keepA, keepB;
    const uint32_t sharedNoTarget = BuildAndGetCascadeShared(dev, /*withTarget=*/false, keepA);
    const uint32_t sharedWithTarget = BuildAndGetCascadeShared(dev, /*withTarget=*/true, keepB);

    // No-op guard: the rowa/rowb children must actually share, or the differential
    // below is vacuous. If this fails, the elements were not share-eligible and the
    // test proves nothing.
    EXPECT_GT(sharedNoTarget, 0u)
        << "children did not share — setup is not share-eligible, test is a no-op";

    // The Mount portal target (same DfsParent + key as the rowa children) must
    // pick up the donor already published in the shared cache when it is built
    // after the children, contributing exactly one additional shared cascade.
    EXPECT_EQ(sharedWithTarget, sharedNoTarget + 1)
        << "Mount portal target must share the donor published in the shared cache";
}
