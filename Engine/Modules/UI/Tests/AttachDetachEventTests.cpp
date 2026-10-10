// kEventAttachedToPanel / kEventDetachedFromPanel — the queued-and-settled attach/detach
// events.
//
// Every arm drives a REAL mutation path — SetRoot, AddChild, TakeChild, RemoveChild, a
// Mount's tab swap, a .uxml reconcile — and pumps UIManager::Update, because what is
// under test is that those paths reach the settle at all and that the settle's verdict is
// the edge, not the mutation. Dispatching by hand would pass on a build where no site
// enqueues anything.
//
// The properties pinned here are the ones a subscriber's correctness rests on:
//   * an initial build attaches every element exactly once, parents first;
//   * a detach that keeps the subtree alive fires leaf-first;
//   * a detach and re-attach that complete inside ONE frame fire NOTHING — the reconcile
//     property the whole design exists for;
//   * destruction is not a detach;
//   * a handler mutating the tree during the settle neither recurses nor is lost.
#include <gtest/gtest.h>

#include "UIRgTestHarness.h"

#include "UI/Assets/UILayoutAsset.h"
#include "UI/Controls/Mount.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Rendering/Core/Device.h"

#include <array>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// One log for a whole tree, so the ORDER between elements is assertable and not just the
// per-element count. Entries read "attach:<name>" / "detach:<name>".
struct AttachLog
{
    std::vector<std::string> Order;

    std::size_t CountOf(const std::string& entry) const
    {
        std::size_t n = 0;
        for (const std::string& e : Order)
            if (e == entry)
                ++n;
        return n;
    }

    // For failure messages: the whole sequence, so a wrong count says what DID happen.
    std::string Joined() const
    {
        std::string s;
        for (const std::string& e : Order)
            s += e + " ";
        return s;
    }
};

void Watch(UIElement& el, std::string name, AttachLog& log)
{
    el.RegisterEventHandler(kEventAttachedToPanel,
                            [&log, name](UIEvent&) { log.Order.push_back("attach:" + name); });
    el.RegisterEventHandler(kEventDetachedFromPanel,
                            [&log, name](UIEvent&) { log.Order.push_back("detach:" + name); });
}

std::unique_ptr<UIElement> MakeElement(const char* id)
{
    auto el = std::make_unique<UIElement>();
    el->SetId(id);
    return el;
}

// root
//  +- a
//  |   +- a1
//  +- b
// Every element is watched BEFORE the tree is given to the manager, so the initial attach
// is observable.
struct WatchedTree
{
    std::unique_ptr<UIElement> Root;
    UIElement* A = nullptr;
    UIElement* A1 = nullptr;
    UIElement* B = nullptr;
};

WatchedTree BuildWatchedTree(AttachLog& log)
{
    WatchedTree t;
    t.Root = MakeElement("root");
    auto a = MakeElement("a");
    auto a1 = MakeElement("a1");
    auto b = MakeElement("b");
    t.A = a.get();
    t.A1 = a1.get();
    t.B = b.get();

    Watch(*t.Root, "root", log);
    Watch(*a, "a", log);
    Watch(*a1, "a1", log);
    Watch(*b, "b", log);

    a->AddChild(std::move(a1));
    t.Root->AddChild(std::move(a));
    t.Root->AddChild(std::move(b));
    return t;
}

// A capture whose DESTRUCTOR removes a LATER element of the walk that is running.
// Fat enough that std::function keeps it on the heap, so the destructor fires when the
// handler-table drain destroys its sink — not earlier, during a move.
struct LaterElementRemover
{
    UIElement* Parent = nullptr;
    UIElement* Victim = nullptr;
    int* Runs = nullptr;
    bool Armed = false;
    std::array<std::uint64_t, 24> Pad{};

    LaterElementRemover(UIElement* parent, UIElement* victim, int* runs)
        : Parent(parent), Victim(victim), Runs(runs), Armed(true)
    {
    }
    LaterElementRemover(const LaterElementRemover& o)
        : Parent(o.Parent), Victim(o.Victim), Runs(o.Runs), Armed(false)
    {
    }
    LaterElementRemover(LaterElementRemover&& o) noexcept
        : Parent(o.Parent), Victim(o.Victim), Runs(o.Runs), Armed(o.Armed)
    {
        o.Armed = false;
    }
    LaterElementRemover& operator=(const LaterElementRemover&) = delete;
    LaterElementRemover& operator=(LaterElementRemover&&) = delete;
    ~LaterElementRemover()
    {
        if (!Armed)
            return;
        Armed = false;
        ++*Runs;
        Parent->RemoveChild(Victim);
    }
};

std::filesystem::path MakeAttachTestUxml(const char* name, const char* contents)
{
    auto dir = std::filesystem::temp_directory_path() / "ui_attach_detach_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream f(path);
    f << contents;
    return path;
}

} // namespace

// DESTRUCTION FIRES. RemoveChild destroys the child, and the subscribers of everything in
// that subtree are told before a single destructor runs — synchronously, post-order, while
// every element is still whole. This is what makes the event usable as the unsubscribe
// hook its Unity namesake is.
TEST(AttachDetachEventTests, RemoveChildAnnouncesDetachForTheWholeDoomedSubtree)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a = tree.A;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    log.Order.clear();

    // Synchronous: the announcement is complete when RemoveChild returns, because after it
    // returns there is nothing left to announce to.
    ui.GetRootElement()->RemoveChild(a);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"detach:a1", "detach:a"}))
        << "children before parents, and before any teardown; log=" << log.Joined();

    EXPECT_EQ(ui.GetRootElement()->FindById("a"), nullptr);

    // And nothing further: the settle must not re-announce a transition already closed.
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order.size(), 2u);
}

// The edge rule governs destruction too. An element whose subscribers were never told
// "attached" is not told "detached" on the way out — built and destroyed inside one frame,
// before any settle ran, is not a transition in either direction. This is also the case
// where a destruction races a still-pending queue slot for the same element.
TEST(AttachDetachEventTests, DestroyingAnElementThatWasNeverAnnouncedAttachedIsSilent)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    ui.SetRoot(MakeElement("root"));
    ui.Update(0.0f, /*interactive=*/false);

    // Added and removed within the same frame: it is queued, but the settle never ran, so
    // no attach was ever dispatched.
    auto fresh = MakeElement("fresh");
    UIElement* freshRaw = fresh.get();
    Watch(*fresh, "fresh", log);
    ui.GetRootElement()->AddChild(std::move(fresh));
    ui.GetRootElement()->RemoveChild(freshRaw);

    EXPECT_TRUE(log.Order.empty()) << "no attach was dispatched, so there is no edge to close";

    // The pending queue slot for the destroyed element must not resurface either.
    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(log.Order.empty());
}

// A manager's death is a destruction of its whole tree, and is announced as one. The
// registries a handler resolves through are still intact at that point.
TEST(AttachDetachEventTests, AManagerDyingAnnouncesItsWholeTree)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    {
        UIManager ui(dev.get());
        WatchedTree tree = BuildWatchedTree(log);
        ui.SetRoot(std::move(tree.Root));
        ui.Update(0.0f, /*interactive=*/false);
        ASSERT_EQ(log.Order.size(), 4u) << "positive control: the tree attached";
        log.Order.clear();
    }

    EXPECT_EQ(log.Order, (std::vector<std::string>{"detach:a1", "detach:a", "detach:b",
                                                   "detach:root"}))
        << "post-order over the whole tree; log=" << log.Joined();
}

// Replacing the root destroys the previous tree, so it announces like any other
// destruction — and the new tree attaches on the next settle.
TEST(AttachDetachEventTests, ReplacingTheRootAnnouncesTheTreeItDestroys)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    log.Order.clear();

    ui.SetRoot(MakeElement("replacement"));
    EXPECT_EQ(log.Order, (std::vector<std::string>{"detach:a1", "detach:a", "detach:b",
                                                   "detach:root"}))
        << "log=" << log.Joined();
}

// A handler reacting to the teardown cannot adopt into the doomed subtree, and cannot
// rescue an element out of it: both would dangle the moment the dispatch returns.
TEST(AttachDetachEventTests, TreeEditsFromInsideADestructionDetachAreRefused)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a = tree.A;
    UIElement* a1 = tree.A1;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);

    bool rescueReturnedSomething = false;
    std::size_t childCountSeenAfterAdopt = 0;
    a->RegisterEventHandler(kEventDetachedFromPanel,
                            [&](UIEvent&)
                            {
                                // (1) adopt a fresh element into the condemned subtree
                                a->AddChild(MakeElement("adopted"));
                                childCountSeenAfterAdopt = a->GetChildren().size();
                                // (2) rescue a condemned child out of it
                                rescueReturnedSomething = a->TakeChild(a1) != nullptr;
                            });

    ui.GetRootElement()->RemoveChild(a);

    EXPECT_EQ(childCountSeenAfterAdopt, 1u) << "the adopt must be refused, leaving only a1";
    EXPECT_FALSE(rescueReturnedSomething) << "a condemned element must not be rescuable";
    EXPECT_EQ(ui.GetRootElement()->FindById("a"), nullptr);
}

// THE COMBINATION NEITHER BRANCH BUILT: the handler-table drain running inside a
// destruction-detach walk.
//
// A detach handler that unsubscribes itself is the canonical use of this event, and that
// deactivation makes the drain run when the dispatch's last executing frame unwinds —
// which is INSIDE this walk, on a condemned-but-alive element, with a static buffer of
// raw pointers to the rest of the doomed set still being iterated. The drain then
// destroys the handler's callable, so an arbitrary module destructor gets control there.
//
// The sharp case is that destructor removing an element LATER in the same walk. What
// makes it safe is not the doomed-set refusals (those cover adopt and rescue) but the
// settle's dispatch guard: it raises UIElement::IsInEventDispatch for the whole walk, so
// RemoveChild self-defers instead of destroying the victim under the buffer. dtorRuns is
// the positive control — without it a guard that simply never let the destructor run
// would look identical to one that handled it.
TEST(AttachDetachEventTests, ADrainReleasedCaptureCannotRemoveALaterElementOfTheSameDestructionWalk)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a1 = tree.A1;
    UIElement* b = tree.B;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);

    UIElement* root = ui.GetRootElement();
    ASSERT_NE(root, nullptr);

    int dtorRuns = 0;
    UIElement::EventHandlerToken tok{};
    tok = a1->RegisterEventHandler(
        kEventDetachedFromPanel,
        [a1, &tok, cap = LaterElementRemover(root, b, &dtorRuns)](UIEvent&)
        {
            // Unsubscribing on detach is the point of the event. It deactivates this
            // entry, which is what asks the drain to run at scope close.
            a1->UnregisterEventHandler(tok);
        });

    // Post-order over the tree is a1, a, b, root — so a1 runs first and b is strictly
    // later in the same walk.
    ui.SetRoot(MakeElement("replacement"));

    EXPECT_EQ(dtorRuns, 1)
        << "the probe never armed: the drain did not release the capture inside the walk";
    EXPECT_EQ(log.CountOf("detach:b"), 1u)
        << "b was destroyed out from under the walk instead of being announced: " << log.Joined();
    EXPECT_EQ(log.CountOf("detach:a1"), 1u) << log.Joined();
    EXPECT_EQ(log.CountOf("detach:root"), 1u)
        << "the walk did not survive to its last element: " << log.Joined();
}

// Mass teardown with nothing subscribed dispatches nothing and costs one bit test per
// element — the same presence bit every other event rides on. The arm pins the zero-
// dispatch property; the cost claim is structural (destruction is already O(n)).
TEST(AttachDetachEventTests, MassTeardownWithNoSubscribersDispatchesNothing)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    {
        UIManager ui(dev.get());
        auto root = MakeElement("root");
        for (int i = 0; i < 200; ++i)
        {
            auto child = MakeElement("bulk");
            child->AddChild(MakeElement("bulkchild"));
            root->AddChild(std::move(child));
        }
        // One watched element only, to prove the walk RAN over a tree where nothing else
        // subscribes — a zero count with no instrument at all would prove nothing.
        auto watched = MakeElement("watched");
        Watch(*watched, "watched", log);
        root->AddChild(std::move(watched));
        ui.SetRoot(std::move(root));
        ui.Update(0.0f, /*interactive=*/false);
        ASSERT_EQ(log.CountOf("attach:watched"), 1u);
    }

    EXPECT_EQ(log.CountOf("detach:watched"), 1u);
    EXPECT_EQ(log.Order.size(), 2u) << "the 401 unsubscribed elements dispatched nothing";
}


TEST(AttachDetachEventTests, InitialBuildAttachesEveryElementPreOrder)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    ui.SetRoot(std::move(tree.Root));

    // Nothing is announced at the mutation site: the events are settled, not raised where
    // the tree changed.
    EXPECT_TRUE(log.Order.empty());

    ui.Update(0.0f, /*interactive=*/false);

    // Pre-order: a parent's handler finds its children already attached, and the root
    // hears first.
    EXPECT_EQ(log.Order,
              (std::vector<std::string>{"attach:root", "attach:a", "attach:a1", "attach:b"}));
}

TEST(AttachDetachEventTests, ASettledTreeAnnouncesNothingOnLaterFrames)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    const std::size_t afterFirstFrame = log.Order.size();
    // Positive control: without it this arm would pass on a build that dispatches nothing
    // at all, which is the failure mode a "nothing happened" assertion cannot see.
    ASSERT_EQ(afterFirstFrame, 4u);

    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    // Edge-triggered: a static tree re-announces nothing, however many frames run.
    EXPECT_EQ(log.Order.size(), afterFirstFrame);
}

TEST(AttachDetachEventTests, TakeChildDetachesTheSubtreePostOrder)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a = tree.A;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    log.Order.clear();

    // TakeChild keeps the subtree ALIVE — the unmount case, as opposed to RemoveChild,
    // which destroys.
    std::unique_ptr<UIElement> detached = ui.GetRootElement()->TakeChild(a);
    ASSERT_NE(detached, nullptr);
    EXPECT_TRUE(log.Order.empty());

    ui.Update(0.0f, /*interactive=*/false);

    // Post-order: teardown runs leaf-upward, the mirror of attach.
    EXPECT_EQ(log.Order, (std::vector<std::string>{"detach:a1", "detach:a"}));
}

TEST(AttachDetachEventTests, ReAttachingADetachedSubtreeAnnouncesItAgain)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a = tree.A;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);

    std::unique_ptr<UIElement> detached = ui.GetRootElement()->TakeChild(a);
    ASSERT_NE(detached, nullptr);
    ui.Update(0.0f, /*interactive=*/false);
    log.Order.clear();

    ui.GetRootElement()->AddChild(std::move(detached));
    ui.Update(0.0f, /*interactive=*/false);

    // The state bit reset with the detach, so the same elements attach again — the
    // inactive-dock-tab semantics.
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:a", "attach:a1"}));
}

TEST(AttachDetachEventTests, DetachAndReAttachInOneFrameAnnouncesNothing)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* a = tree.A;
    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    log.Order.clear();

    // ReconcileChildren in miniature: unlink everything, then put back what is kept. The
    // settle compares against the last state DISPATCHED, so this round trip is not a
    // transition and produces no pair.
    std::unique_ptr<UIElement> detached = ui.GetRootElement()->TakeChild(a);
    ASSERT_NE(detached, nullptr);
    ui.GetRootElement()->AddChild(std::move(detached));

    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_TRUE(log.Order.empty())
        << "a detach and re-attach completing inside one frame must dispatch nothing";
}


TEST(AttachDetachEventTests, MountTabActivationIsAnAttachAndDeactivationADetach)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    auto mountOwned = std::make_unique<Mount>();
    Mount* mount = mountOwned.get();
    root->AddChild(std::move(mountOwned));
    ui.SetRoot(std::move(root));

    // The two tab panels are externally owned, exactly as a dockspace owns its panels.
    // Declared after the manager so they are destroyed before it.
    auto tabA = MakeElement("tabA");
    auto tabB = MakeElement("tabB");
    Watch(*tabA, "tabA", log);
    Watch(*tabB, "tabB", log);

    mount->SetTarget(tabA.get());
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:tabA"}));
    log.Order.clear();

    // The dockspace's activation path (DockspaceElement::PatchActiveTab). No owner
    // changes for tabA and no AddChild happens anywhere — only the portal edge moves.
    mount->SwapTargetForActivation(tabB.get());
    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(log.CountOf("detach:tabA"), 1u)
        << "deactivating a tab makes its subtree unreachable, which is a detach";
    EXPECT_EQ(log.CountOf("attach:tabB"), 1u);
    EXPECT_EQ(log.Order.size(), 2u);
}

TEST(AttachDetachEventTests, HandlerMutatingTheTreeDuringTheSettleDefersToTheNextFrame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    Watch(*root, "root", log);

    // The natural setup point for a managed element: build your children when you are
    // told you are attached.
    int dispatchDepthSeenInHandler = -1;
    root->RegisterEventHandler(kEventAttachedToPanel,
                               [&](UIEvent&)
                               {
                                   dispatchDepthSeenInHandler = UIElement::IsInEventDispatch() ? 1 : 0;
                                   auto child = MakeElement("late");
                                   Watch(*child, "late", log);
                                   rootRaw->AddChild(std::move(child));
                               });

    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);

    // The settle raises the dispatch-depth guard, so a handler's RemoveChild defers the
    // way it does inside any other dispatch instead of mutating the walked child list.
    EXPECT_EQ(dispatchDepthSeenInHandler, 1);
    // One generation per settle: the child the handler added is announced NEXT frame, not
    // by re-entering the walk that is already running.
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:root"}));

    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:root", "attach:late"}));
}

TEST(AttachDetachEventTests, HandlerRemovingAnElementDuringTheSettleIsDeferredThenAnnounced)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    WatchedTree tree = BuildWatchedTree(log);
    UIElement* rootRaw = tree.Root.get();
    UIElement* b = tree.B;

    tree.A->RegisterEventHandler(kEventAttachedToPanel,
                                 [rootRaw, b](UIEvent&) { rootRaw->RemoveChild(b); });

    ui.SetRoot(std::move(tree.Root));
    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    // The removal is deferred out of the dispatch, and when it finally runs it destroys
    // `b` — which now announces, because destruction is a detach. The walk it was removed
    // from did not fault.
    EXPECT_EQ(ui.GetRootElement()->FindById("b"), nullptr);
    EXPECT_EQ(log.CountOf("attach:b"), 1u);
    EXPECT_EQ(log.CountOf("detach:b"), 1u)
        << "the deferred RemoveChild destroys b, and destruction announces";
}

// An element can hold TWO settle-queue slots at once, and destroying it must clear both.
//
// The drain clears an entry's queue pointer as it consumes it, so a handler running later
// in the same generation can queue that same element again for the next one — leaving the
// spent slot and the fresh slot both pointing at it until the drain erases the consumed
// generation. A ForgetAttachSettle that stopped at the first match would null the spent
// slot and leave the live one aimed at freed memory.
//
// This arm drives exactly that sequence, and it catches the memory error itself, not just
// its symptom: with the fix reverted to stopping at the first slot, this test faults
// outright — SEH 0xc0000005 in plain DebugFast, no sanitizer needed.
TEST(AttachDetachEventTests, DestroyingAnElementQueuedTwiceClearsBothSlots)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    auto mountOwned = std::make_unique<Mount>();
    Mount* mount = mountOwned.get();
    UIElement* rootRaw = root.get();
    root->AddChild(std::move(mountOwned));
    ui.SetRoot(std::move(root));

    // Externally owned, like a dockspace's panel: the Mount does not own its target, so a
    // handler can destroy it while the queue still refers to it.
    auto portalTarget = MakeElement("portal");
    Watch(*portalTarget, "portal", log);
    mount->SetTarget(portalTarget.get());
    ui.Update(0.0f, /*interactive=*/false);
    ASSERT_EQ(log.CountOf("attach:portal"), 1u); // positive control: the settle is live

    // Queue the portal target FIRST (site 3), so the drain consumes its slot before the
    // trigger's handler runs...
    mount->SetTarget(nullptr);

    // ...then queue a trigger behind it whose attach handler re-queues the portal target
    // and destroys it inside the same settle.
    auto trigger = MakeElement("trigger");
    UIElement* triggerRaw = trigger.get();
    rootRaw->AddChild(std::move(trigger));
    triggerRaw->RegisterEventHandler(kEventAttachedToPanel,
                                     [&](UIEvent&)
                                     {
                                         mount->SetTarget(portalTarget.get()); // second slot
                                         portalTarget.reset();                 // both must clear
                                     });

    ui.Update(0.0f, /*interactive=*/false);
    // The frame that would read a dangling slot.
    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(mount->GetTarget(), nullptr) << "a destroyed target must not stay mounted";
    // Exactly one of each, and the counts say which events came from where: the attach is
    // the original mount, and the detach is the UNMOUNT at the top of this arm — a real
    // transition, dispatched while the element was still alive. The re-mount inside the
    // handler and the destruction that followed it add nothing, because the element left
    // the queue before any settle could judge it again. Anything above these counts would
    // mean a dead element was walked.
    EXPECT_EQ(log.CountOf("attach:portal"), 1u);
    EXPECT_EQ(log.CountOf("detach:portal"), 1u);
}

// The headline arm: the reconcile path this design exists for. ReconcileChildren detaches
// every child of the reconciled parent before matching any of them, then re-adds the ones
// the template still wants. A preserved element must therefore see NO pair, while an
// element the template newly introduces must see its attach.
TEST(AttachDetachEventTests, ReconcilePreservedElementsGetNoPairAndNewOnesAttach)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_attach_detach_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto path = MakeAttachTestUxml("attach_reconcile.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <UIElement id=\"kept\"/>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());

    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* kept = ui.GetRootElement()->FindById("kept");
    ASSERT_NE(kept, nullptr);
    const uint64_t keptId = kept->GetInstanceId();

    // Subscribe AFTER the initial build, so the log starts empty and anything in it comes
    // from the reload.
    AttachLog log;
    Watch(*kept, "kept", log);

    {
        std::ofstream f(path);
        f << "<UIElement id=\"root\">\n"
             "  <UIElement id=\"kept\"/>\n"
             "  <UIElement id=\"added\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* keptAfter = ui.GetRootElement()->FindById("kept");
    ASSERT_NE(keptAfter, nullptr);
    // The reuse precondition: if the reconcile rebuilt this element the arm below would be
    // vacuously green, since a rebuilt element carries none of the subscriptions.
    ASSERT_EQ(keptAfter->GetInstanceId(), keptId);
    EXPECT_TRUE(log.Order.empty())
        << "a .uxml save must not fire a detach/attach pair on the elements it preserved";

    UIElement* added = ui.GetRootElement()->FindById("added");
    ASSERT_NE(added, nullptr);
    AttachLog addedLog;
    Watch(*added, "added", addedLog);
    // The new element was created and parented during the reload, so its attach is already
    // settled — subscribing now is too late to hear it, which is exactly why an element's
    // own subscription has to be wired natively at materialize time rather than through
    // the resolve path (section 13.4a, the wiring trap).
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(addedLog.Order.empty());
}

// The other half of a reconcile: an element the template REPLACES. The replacement is a
// different element and attaches; the original is destroyed by the reconcile, and
// destruction is not a detach, so nothing is dispatched for it.
TEST(AttachDetachEventTests, ReconcileAnnouncesTheElementItReplaces)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_attach_detach_replace_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto path = MakeAttachTestUxml("attach_reconcile_replace.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <UIElement id=\"slot\"/>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());

    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* before = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(before, nullptr);
    const uint64_t idBefore = before->GetInstanceId();

    AttachLog log;
    Watch(*before, "slot", log);

    {
        std::ofstream f(path);
        f << "<UIElement id=\"root\">\n"
             "  <Label id=\"slot\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* after = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(after, nullptr);
    ASSERT_NE(after->GetInstanceId(), idBefore) << "a tag change must rebuild, not adopt";
    // The original was DESTROYED inside the reconcile, and destruction is announced. This
    // arm previously asserted the opposite — that it "announced nothing on the way out" —
    // and passed for the wrong reason: the reconcile's type-change path destroyed it
    // outside every decision point, so the silence it pinned was the bug, not the contract.
    EXPECT_EQ(log.CountOf("detach:slot"), 1u)
        << "a replaced element is destroyed, and destruction announces; log=" << log.Joined();
    EXPECT_EQ(log.Order.size(), 1u) << "and nothing else; log=" << log.Joined();

    // Positive control, for the same reason as the arms above: prove the settle is live in
    // this fixture by making the replacement produce a transition that must be heard.
    AttachLog replacementLog;
    Watch(*after, "replacement", replacementLog);
    std::unique_ptr<UIElement> unmounted = ui.GetRootElement()->TakeChild(after);
    ASSERT_NE(unmounted, nullptr);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(replacementLog.Order, (std::vector<std::string>{"detach:replacement"}));
}

// ---------------------------------------------------------------------------------------
// Arms derived from the adversarial review of PR #1097. The first two are the blockers it
// found; the rest are properties it attacked and that hold. Re-derived here rather than
// carried as a separate probe file, so they gate every future change to the settle.
// ---------------------------------------------------------------------------------------

// REVIEW BLOCKER 1. The walk must not lose a sibling when a handler INSERTS during it.
//
// AddChild/InsertChild are not deferred during dispatch (only removal is), and an insert
// at a low index shifts every later sibling along. A walk that read the live child vector
// while dispatching would step past the last one — and a skip has no backstop: the element
// was already in the tree, so nothing re-queues it and its attach is lost outright rather
// than deferred a frame.
TEST(AttachDetachEventTests, InsertChildFromAnAttachHandlerDoesNotSkipASibling)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    Watch(*root, "root", log);
    for (const char* id : {"c0", "c1", "c2"})
    {
        auto c = MakeElement(id);
        Watch(*c, id, log);
        root->AddChild(std::move(c));
    }
    // The root's own attach handler inserts a new FIRST child, shifting c0..c2 by one.
    root->RegisterEventHandler(kEventAttachedToPanel,
                               [&](UIEvent&)
                               {
                                   auto n = MakeElement("inserted");
                                   Watch(*n, "inserted", log);
                                   rootRaw->InsertChild(0, std::move(n));
                               });
    ui.SetRoot(std::move(root));

    for (int i = 0; i < 4; ++i)
        ui.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(log.CountOf("attach:c0"), 1u);
    EXPECT_EQ(log.CountOf("attach:c1"), 1u);
    EXPECT_EQ(log.CountOf("attach:c2"), 1u)
        << "a sibling shifted by a handler's insert must still be announced";
    // The inserted element is next frame's business, and it must arrive exactly once.
    EXPECT_EQ(log.CountOf("attach:inserted"), 1u);
}

// REVIEW BLOCKER 2. An element that outlives its manager must be announceable again.
//
// A manager's death dispatches nothing — subscribers of a dying tree have no safe way to
// touch it — but the dispatched-state bit records what subscribers were TOLD. Left true on
// a survivor, the next manager's settle reads "no change" and stays silent when the
// element is genuinely re-attached. The live shape is a panel undocked into a floating
// window, re-docked after that window closes.
TEST(AttachDetachEventTests, ASurvivingMountTargetIsAnnouncedAgainByItsNextManager)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    auto panel = MakeElement("panel");
    Watch(*panel, "panel", log);

    {
        UIManager a(dev.get());
        auto rootA = MakeElement("rootA");
        auto mountOwned = std::make_unique<Mount>();
        Mount* m = mountOwned.get();
        rootA->AddChild(std::move(mountOwned));
        a.SetRoot(std::move(rootA));
        m->SetTarget(panel.get());
        a.Update(0.0f, /*interactive=*/false);
        ASSERT_EQ(log.CountOf("attach:panel"), 1u); // positive control: the settle is live
        ASSERT_EQ(log.CountOf("detach:panel"), 0u);
    }

    UIManager b(dev.get());
    auto rootB = MakeElement("rootB");
    auto mountOwned = std::make_unique<Mount>();
    Mount* m = mountOwned.get();
    rootB->AddChild(std::move(mountOwned));
    b.SetRoot(std::move(rootB));
    m->SetTarget(panel.get());
    b.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(log.CountOf("attach:panel"), 2u)
        << "re-mounting a surviving panel into a live manager must announce an attach";

    m->SetTarget(nullptr);
    b.Update(0.0f, /*interactive=*/false);
}

// The stated contract, checked AT THE MOMENT OF DISPATCH rather than after the fact: an
// element announced ATTACHED is one the scripting ABI can resolve, always.
//
// The case that can break it is a Mount targeted BEFORE the Mount itself is parented. The
// Mount cannot propagate an owner it does not have, so the target starts ownerless while
// the portal edge already routes it into the tree; Mount::OnPostLayout adopts it later in
// the same frame, which is after the settle has already run. A walk that inherited the
// queued root's verdict straight down the portal edge would therefore announce the target
// while it is still ownerless — and an ownerless element is registered nowhere, so
// FindElementByInstanceId cannot resolve it and IsAlive reports false. The announcement
// waits for the adoption instead.
TEST(AttachDetachEventTests, AnElementIsNeverAnnouncedAttachedBeforeTheAbiCanResolveIt)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev.get());
    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);

    auto target = MakeElement("t");
    AttachLog log;
    Watch(*target, "t", log);
    const uint64_t tid = target->GetInstanceId();

    // The invariant itself, evaluated inside the dispatch: whatever else happens, an
    // element must be resolvable at the instant it is told it is attached.
    int resolvableWhenAnnounced = -1;
    target->RegisterEventHandler(
        kEventAttachedToPanel,
        [&](UIEvent&) { resolvableWhenAnnounced = ui.FindElementByInstanceId(tid) ? 1 : 0; });

    auto mountOwned = std::make_unique<Mount>();
    Mount* m = mountOwned.get();
    // SetTarget BEFORE the Mount is parented: the Mount has no owner, so its
    // owner-propagation block is skipped and the target starts ownerless.
    m->SetTarget(target.get());
    ASSERT_EQ(target->GetOwnerManager(), nullptr) << "precondition: unowned target";
    rootRaw->AddChild(std::move(mountOwned));

    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.CountOf("attach:t"), 0u)
        << "the target was still ownerless when this frame's settle ran, so announcing it "
           "would have named an element no lookup could resolve";

    // OnPostLayout adopted it during that frame, which changed its owner and queued it.
    ASSERT_EQ(target->GetOwnerManager(), &ui) << "Mount::OnPostLayout adopts the target";

    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.CountOf("attach:t"), 1u) << "announced once the owner is real";
    EXPECT_EQ(resolvableWhenAnnounced, 1)
        << "an element announced ATTACHED must be resolvable at that instant";
    EXPECT_NE(ui.FindElementByInstanceId(tid), nullptr);

    m->SetTarget(nullptr);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.CountOf("detach:t"), 1u);
}

// The mirror of DetachAndReAttachInOneFrameAnnouncesNothing: an attach and a detach that
// both complete inside one generation are jointly a non-event.
TEST(AttachDetachEventTests, AttachThenDetachInOneFrameAnnouncesNothing)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);

    auto fresh = MakeElement("fresh");
    UIElement* freshRaw = fresh.get();
    Watch(*fresh, "fresh", log);
    rootRaw->AddChild(std::move(fresh));
    std::unique_ptr<UIElement> takenBack = rootRaw->TakeChild(freshRaw);
    ASSERT_NE(takenBack, nullptr);

    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(log.Order.empty())
        << "an attach and detach completing inside one frame must dispatch nothing";

    // Positive control: the settle is live in this fixture.
    rootRaw->AddChild(std::move(takenBack));
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:fresh"}));
}

// A handler that re-mutates on EVERY attach must not spin the settle, and must not have
// its generations collapsed or dropped: exactly one per frame, forever.
TEST(AttachDetachEventTests, AHandlerRemutatingOnEveryAttachAdvancesOneGenerationPerFrame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());
    auto root = MakeElement("root");

    int depth = 0;
    std::function<void(UIElement&)> arm = [&](UIElement& el)
    {
        el.RegisterEventHandler(kEventAttachedToPanel,
                                [&, parent = &el](UIEvent&)
                                {
                                    if (depth >= 5)
                                        return;
                                    ++depth;
                                    auto child = MakeElement("gen");
                                    UIElement* raw = child.get();
                                    Watch(*child, "gen" + std::to_string(depth), log);
                                    parent->AddChild(std::move(child));
                                    arm(*raw);
                                });
    };
    arm(*root);
    ui.SetRoot(std::move(root));

    for (int i = 0; i < 8; ++i)
        ui.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(log.CountOf("attach:gen1"), 1u);
    EXPECT_EQ(log.CountOf("attach:gen5"), 1u);
    EXPECT_EQ(log.Order.size(), 5u) << "one generation per frame: none lost, none collapsed";
}

// Order checked against a real three-level tree that crosses a Mount portal, rather than
// against the comment claiming it.
TEST(AttachDetachEventTests, OrderHoldsAcrossAMountPortalEdge)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto portal = MakeElement("portal");
    auto portalChild = MakeElement("portalChild");
    Watch(*portal, "portal", log);
    Watch(*portalChild, "portalChild", log);
    portal->AddChild(std::move(portalChild));

    auto root = MakeElement("root");
    auto a = MakeElement("a");
    auto mountOwned = std::make_unique<Mount>();
    Mount* m = mountOwned.get();
    Watch(*root, "root", log);
    Watch(*a, "a", log);
    a->AddChild(std::move(mountOwned));
    root->AddChild(std::move(a));
    ui.SetRoot(std::move(root));
    m->SetTarget(portal.get());

    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"attach:root", "attach:a", "attach:portal",
                                                   "attach:portalChild"}))
        << "attach is pre-order, and the portal edge is part of the order";

    log.Order.clear();
    m->SetTarget(nullptr);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"detach:portalChild", "detach:portal"}))
        << "detach is post-order across the same edge";
}

// ---------------------------------------------------------------------------------------
// Arms derived from the round-2 delta review. The first pair is the blocker that the
// snapshot walk introduced and the one-variable control that isolated its cause; the
// second pair attacks the snapshot's liveness contract directly.
// ---------------------------------------------------------------------------------------
// D1. ONE OWNER SLOT PER REFERENCE KIND.
//
// The invariant: an element's queue membership and its walk membership each carry their own
// manager identity, so an element queued in A and simultaneously walked by B is correctly
// described by both, and leaves both vectors when it dies. Collapsing them into a single
// back-pointer breaks it — the slot names whichever manager wrote last, the other manager's
// vector keeps a pointer no destructor will ever tombstone, and its next settle writes
// through that slot and dereferences it.
//
// This arm drives that exact sequence: X is queued in A, reached by B's walk snapshot, then
// destroyed, and A settles afterwards. It fails as an access violation rather than an
// assertion, so run it in isolation when it does.
TEST(AttachDetachEventTests, ElementQueuedInOneManagerAndWalkedByAnotherLeavesADanglingQueueSlot)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager a(dev.get());
    UIManager b(dev.get());

    auto rootA = MakeElement("rootA");
    auto mountAOwned = std::make_unique<Mount>();
    Mount* mA = mountAOwned.get();
    rootA->AddChild(std::move(mountAOwned));
    a.SetRoot(std::move(rootA));

    b.SetRoot(MakeElement("rootB"));
    UIElement* rootBRaw = b.GetRootElement();

    a.Update(0.0f, /*interactive=*/false);
    b.Update(0.0f, /*interactive=*/false);

    // X is externally owned, the ordinary Mount-target shape.
    auto x = MakeElement("x");
    Watch(*x, "x", log);

    // Queue X in manager A — and deliberately do NOT let A settle, so the queue entry
    // survives into the next step.
    mA->SetTarget(x.get());
    ASSERT_EQ(x->GetOwnerManager(), &a) << "precondition: X is owned by, and queued in, A";

    // Now route X's DFS-parent edge into B's tree through a second, initially unowned Mount
    // (so B never gets a chance to claim X through EnqueueAttachSettle's guard).
    auto mountBOwned = std::make_unique<Mount>();
    Mount* mB = mountBOwned.get();
    mB->SetTarget(x.get());
    rootBRaw->AddChild(std::move(mountBOwned));
    ASSERT_EQ(x->GetDfsParent(), mB) << "precondition: B's walk will reach X through mB";

    // B's settle collects [mB, X] and MarkWalking overwrites X's settle-owner A -> B.
    b.Update(0.0f, /*interactive=*/false);

    // X dies. Its destructor forgets itself from B only; A's queue slot still names it.
    x.reset();

    // A's settle now reads that slot: it writes the membership bits through it and then
    // derefs it. Freed memory both times.
    a.Update(0.0f, /*interactive=*/false);
    a.Update(0.0f, /*interactive=*/false);

    // The semantic half of the same root cause: X is owned by A and reaches neither
    // manager's root, so NOBODY may announce it attached. Before the fix, B's walk
    // inherited its queued root's verdict straight down the portal edge and announced X
    // while X's own predicate said detached and no lookup could resolve it.
    EXPECT_EQ(log.CountOf("attach:x"), 0u)
        << "an element owned by A and unreachable from A's root must not be announced by "
           "B's walk; log=" << log.Joined();
}

// D1-CONTROL. The identical sequence with ONE variable removed: B never settles, so
// MarkWalking never runs and X's settle-owner still names A when it dies. If this arm is
// green while D1 faults, the overwrite in MarkWalking is the cause and nothing else in the
// sequence is.
TEST(AttachDetachEventTests, ControlSameSequenceWithoutBsWalkIsSafe)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager a(dev.get());
    UIManager b(dev.get());

    auto rootA = MakeElement("rootA");
    auto mountAOwned = std::make_unique<Mount>();
    Mount* mA = mountAOwned.get();
    rootA->AddChild(std::move(mountAOwned));
    a.SetRoot(std::move(rootA));

    b.SetRoot(MakeElement("rootB"));
    UIElement* rootBRaw = b.GetRootElement();

    a.Update(0.0f, false);
    b.Update(0.0f, false);

    auto x = MakeElement("x");
    mA->SetTarget(x.get());
    ASSERT_EQ(x->GetOwnerManager(), &a);

    auto mountBOwned = std::make_unique<Mount>();
    Mount* mB = mountBOwned.get();
    mB->SetTarget(x.get());
    rootBRaw->AddChild(std::move(mountBOwned));
    ASSERT_EQ(x->GetDfsParent(), mB);

    // THE ONLY DIFFERENCE: no b.Update() here.
    x.reset();

    a.Update(0.0f, false);
    a.Update(0.0f, false);
    SUCCEED();
}

// D2. A handler destroys an element the snapshot names but has NOT visited yet. The
// revalidation half of the fix has to skip it. Expected GREEN — this is the property the
// StillIndexed discipline exists for.
TEST(AttachDetachEventTests, DestroyingASnapshottedButUnvisitedElementIsSkipped)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    auto m1Owned = std::make_unique<Mount>();
    auto m2Owned = std::make_unique<Mount>();
    Mount* m1 = m1Owned.get();
    Mount* m2 = m2Owned.get();
    root->AddChild(std::move(m1Owned));
    root->AddChild(std::move(m2Owned));
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);

    // Both targets externally owned; t2 is later in pre-order than t1.
    auto t1 = MakeElement("t1");
    auto t2 = MakeElement("t2");
    Watch(*t1, "t1", log);
    Watch(*t2, "t2", log);
    m1->SetTarget(t1.get());
    m2->SetTarget(t2.get());

    // t1's attach handler destroys t2 while t2 sits unvisited in the same snapshot.
    t1->RegisterEventHandler(kEventAttachedToPanel, [&](UIEvent&) { t2.reset(); });

    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_EQ(log.CountOf("attach:t1"), 1u);
    EXPECT_EQ(log.CountOf("attach:t2"), 0u) << "a destroyed element must not be walked";
    // Silent for the RESIDUE's reason, not the deleted one. Destruction IS a detach now,
    // but t2 is externally owned and destroyed directly by this test — the one path with no
    // engine-side decision point to announce from (see the API rule at the ids: unmount or
    // remove before destroying). Had it been removed through the tree, this would be 1.
    // If that residue is ever closed, this expectation flips to 1 — it does not mean the
    // old "destruction is not a detach" rule came back.
    EXPECT_EQ(log.CountOf("detach:t2"), 0u)
        << "externally owned and destroyed directly: no decision point announces it";
    EXPECT_EQ(m2->GetTarget(), nullptr);
}

// D3. A handler detaches an ANCESTOR of nodes the snapshot has not visited yet. The
// snapshot's verdict was inherited from the queued root before the mutation, so those nodes
// are announced ATTACHED while already unreachable. The design's claim is that the
// mechanism is edge-triggered and self-correcting; this pins whether the correction
// actually arrives, i.e. whether the stale state is transient or permanent.
//
// The whole tree is wired BEFORE the first Update so that n1, the host and the portal all
// attach in one generation with n1 visited first — an earlier version of this arm
// registered n1's handler after n1 had already attached, so the handler never ran and the
// arm reported a false clean.
TEST(AttachDetachEventTests, DetachingAnAncestorMidWalkSelfCorrectsNextFrame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto portal = MakeElement("portal");
    auto portalChild = MakeElement("portalChild");
    Watch(*portal, "portal", log);
    Watch(*portalChild, "portalChild", log);
    portal->AddChild(std::move(portalChild));

    auto root = MakeElement("root");
    auto n1 = MakeElement("n1");
    UIElement* n1Raw = n1.get();
    auto hostOwned = std::make_unique<Mount>();
    Mount* host = hostOwned.get();
    Watch(*n1, "n1", log);
    root->AddChild(std::move(n1));       // n1 precedes host in pre-order
    root->AddChild(std::move(hostOwned));

    host->SetTarget(portal.get());

    int handlerRuns = 0;
    n1Raw->RegisterEventHandler(kEventAttachedToPanel,
                                [&](UIEvent&)
                                {
                                    ++handlerRuns;
                                    host->SetTarget(nullptr);
                                });

    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);
    const std::string afterFrame1 = log.Joined();

    // Instrument check FIRST: without this the arm reports a false clean if the handler
    // never fired (which is exactly how the first version of it failed).
    ASSERT_EQ(handlerRuns, 1) << "the mid-walk mutation must actually have happened";
    ASSERT_EQ(host->GetTarget(), nullptr) << "the portal must actually be unmounted";

    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    // The steady state must agree with reality: the portal is unmounted, so it must not be
    // left believing it is attached.
    EXPECT_EQ(log.CountOf("attach:portal"), log.CountOf("detach:portal"))
        << "an element left announced-attached while unreachable is a permanent stale state; "
        << "frame1=[" << afterFrame1 << "] all=[" << log.Joined() << "]";
    EXPECT_EQ(log.CountOf("attach:portalChild"), log.CountOf("detach:portalChild"))
        << "all=[" << log.Joined() << "]";
}

// ---------------------------------------------------------------------------------------
// Arms derived from the round-5 delta review of the destruction contract: the iterator held
// across a dispatch, and the three destroyed-or-cleared-while-attached paths that bypassed
// the decision points. Two of them are the isolating controls, kept because they pin what
// must stay ALLOWED.
// ---------------------------------------------------------------------------------------
TEST(AttachDetachEventTests, DetachHandlerAddingASiblingDoesNotCorruptRemoveChild)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, false);

    // Small capacity to start; the handler's AddChild will force a reallocation.
    auto doomed = MakeElement("doomed");
    UIElement* doomedRaw = doomed.get();
    Watch(*doomed, "doomed", log);
    rootRaw->AddChild(std::move(doomed));
    ui.Update(0.0f, false);
    ASSERT_EQ(log.CountOf("attach:doomed"), 1u) << "positive control: the settle is live";

    // The handler edits the LIVE parent, which no refusal covers.
    doomedRaw->RegisterEventHandler(kEventDetachedFromPanel,
                                    [&](UIEvent&)
                                    {
                                        for (int i = 0; i < 8; ++i)
                                        {
                                            auto sib = MakeElement("sibling");
                                            rootRaw->AddChild(std::move(sib));
                                        }
                                    });

    rootRaw->RemoveChild(doomedRaw);

    ui.Update(0.0f, false);
    EXPECT_EQ(rootRaw->FindById("doomed"), nullptr) << "the doomed element must be gone";
    SUCCEED() << "log=" << log.Joined();
}

// R2. The three LoadLayout* paths assign m_Root directly (UIManager_Assets.cpp:514, :614,
// :969) instead of going through SetRoot, so they destroy the previous tree without the
// destruction-detach that SetRoot (:1542) now performs.
TEST(AttachDetachEventTests, LoadingASecondLayoutAnnouncesTheTreeItDestroys)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_r4_loadlayout_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pathA = MakeAttachTestUxml("ui_r4_loadlayout_a.uxml",
                          "<UIElement id=\"root\">\n  <UIElement id=\"first\"/>\n</UIElement>\n");
    auto pathB = MakeAttachTestUxml("ui_r4_loadlayout_b.uxml",
                          "<UIElement id=\"rootB\">\n  <UIElement id=\"second\"/>\n</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathA));
    ASSERT_TRUE(reg.RegisterAsset(pathB));

    auto assetA = assets.LoadAssetAsync(reg.GetAssetGUID(pathA), AssetLoadPriority::Normal).get();
    auto assetB = assets.LoadAssetAsync(reg.GetAssetGUID(pathB), AssetLoadPriority::Normal).get();
    ASSERT_TRUE(assetA);
    ASSERT_TRUE(assetB);
    auto* layoutA = dynamic_cast<UILayoutAsset*>(assetA.get());
    auto* layoutB = dynamic_cast<UILayoutAsset*>(assetB.get());
    ASSERT_NE(layoutA, nullptr);
    ASSERT_NE(layoutB, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layoutA));
    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    AttachLog log;
    UIElement* first = ui.GetRootElement()->FindById("first");
    ASSERT_NE(first, nullptr);
    Watch(*first, "first", log);
    // Make the attach real so there is an edge to close.
    UIElement* firstRaw = first;
    (void)firstRaw;
    // `first` was announced attached during the initial build, before Watch; re-announce it
    // by round-tripping through a detach/attach so this arm has a dispatched edge of its own.
    std::unique_ptr<UIElement> taken = ui.GetRootElement()->TakeChild(first);
    ASSERT_NE(taken, nullptr);
    ui.Update(0.0f, false);
    ui.GetRootElement()->AddChild(std::move(taken));
    ui.Update(0.0f, false);
    ASSERT_EQ(log.CountOf("attach:first"), 1u) << "positive control; log=" << log.Joined();
    ASSERT_EQ(log.CountOf("detach:first"), 1u);

    // Now replace the whole layout. The old tree is destroyed by `m_Root = CloneElement(...)`.
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layoutB));
    ui.Update(0.0f, false);

    EXPECT_EQ(log.CountOf("detach:first"), 2u)
        << "loading a new layout destroys the old tree and owes it a detach; log="
        << log.Joined();
}

// R3. The reconcile's TYPE-CHANGE path destroys the preserved element via `use.reset()`
// (UIHotReload.cpp:461), which happens before — and is not covered by — the new
// destruction-detach loop over the byId/noId leftovers (:508, :513).
TEST(AttachDetachEventTests, ReconcileTypeChangeAnnouncesTheElementItReplaces)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_r4_typechange_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto path = MakeAttachTestUxml("ui_r4_typechange_tc.uxml",
                         "<UIElement id=\"root\">\n  <UIElement id=\"slot\"/>\n</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));
    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    UIElement* before = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(before, nullptr);
    const uint64_t idBefore = before->GetInstanceId();

    AttachLog log;
    Watch(*before, "slot", log);
    // Give it a dispatched attach edge of its own.
    std::unique_ptr<UIElement> taken = ui.GetRootElement()->TakeChild(before);
    ASSERT_NE(taken, nullptr);
    ui.Update(0.0f, false);
    ui.GetRootElement()->AddChild(std::move(taken));
    ui.Update(0.0f, false);
    ASSERT_EQ(log.CountOf("attach:slot"), 1u) << "positive control; log=" << log.Joined();
    const std::size_t detachesBefore = log.CountOf("detach:slot");

    {
        std::ofstream f(path);
        f << "<UIElement id=\"root\">\n  <Label id=\"slot\"/>\n</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));
    ui.Update(0.0f, false);
    DriveUiRender(ui, rg);

    UIElement* after = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(after, nullptr);
    ASSERT_NE(after->GetInstanceId(), idBefore) << "precondition: a tag change rebuilds";

    EXPECT_EQ(log.CountOf("detach:slot"), detachesBefore + 1)
        << "the replaced element was destroyed by the reconcile and owes a detach; log="
        << log.Joined();
}

// R4. A manager dying announces its tree — but an externally owned Mount target is not in
// that tree. It was announced attached; ~UIManager clears its dispatched bit without
// announcing anything, so the attach never gets its detach.
TEST(AttachDetachEventTests, AManagerDyingAnnouncesItsExternallyOwnedMountTargetToo)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    auto panel = MakeElement("panel");
    Watch(*panel, "panel", log);

    {
        UIManager a(dev.get());
        auto rootA = MakeElement("rootA");
        auto mountOwned = std::make_unique<Mount>();
        Mount* m = mountOwned.get();
        rootA->AddChild(std::move(mountOwned));
        a.SetRoot(std::move(rootA));
        m->SetTarget(panel.get());
        a.Update(0.0f, false);
        ASSERT_EQ(log.CountOf("attach:panel"), 1u) << "positive control";
        ASSERT_EQ(log.CountOf("detach:panel"), 0u);
    }

    EXPECT_EQ(log.CountOf("detach:panel"), 1u)
        << "the panel was announced attached and its manager died; the contract owes it a "
           "detach; log=" << log.Joined();
}

// R5. The edge rule, both orderings, for a destruction that races a pending queue slot.
TEST(AttachDetachEventTests, EdgeRuleHoldsForDestructionAgainstAPendingQueueSlot)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    // Ordering A: queued attach never dispatched, then destroyed -> silent both ways.
    {
        AttachLog log;
        UIManager ui(dev.get());
        auto root = MakeElement("root");
        UIElement* rootRaw = root.get();
        ui.SetRoot(std::move(root));
        ui.Update(0.0f, false);

        auto fresh = MakeElement("fresh");
        UIElement* freshRaw = fresh.get();
        Watch(*fresh, "fresh", log);
        rootRaw->AddChild(std::move(fresh)); // queued, not yet settled
        rootRaw->RemoveChild(freshRaw);      // destroyed before any settle
        ui.Update(0.0f, false);
        ui.Update(0.0f, false);
        EXPECT_TRUE(log.Order.empty())
            << "no attach was ever dispatched, so there is no edge to close; log="
            << log.Joined();
    }

    // Ordering B: attach dispatched, then a queued detach pending, then destroyed.
    {
        AttachLog log;
        UIManager ui(dev.get());
        auto root = MakeElement("root");
        UIElement* rootRaw = root.get();
        ui.SetRoot(std::move(root));
        ui.Update(0.0f, false);

        auto el = MakeElement("el");
        UIElement* elRaw = el.get();
        Watch(*el, "el", log);
        rootRaw->AddChild(std::move(el));
        ui.Update(0.0f, false);
        ASSERT_EQ(log.CountOf("attach:el"), 1u);

        rootRaw->RemoveChild(elRaw); // synchronous destruction detach
        ui.Update(0.0f, false);
        ui.Update(0.0f, false);
        EXPECT_EQ(log.CountOf("detach:el"), 1u) << "exactly one, not two; log=" << log.Joined();
        EXPECT_EQ(log.CountOf("attach:el"), 1u);
    }
}

// R1-CONTROL. Identical to R1 except the handler adds its siblings to a DIFFERENT live
// parent — one whose m_Children no iterator is currently held into. If this is green while
// R1 faults, the held iterator in RemoveChildImpl is the cause and nothing else is.
TEST(AttachDetachEventTests, ControlDetachHandlerAddingToAnUnrelatedParentIsSafe)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    AttachLog log;
    UIManager ui(dev.get());

    auto root = MakeElement("root");
    UIElement* rootRaw = root.get();
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, false);

    auto other = MakeElement("other");
    UIElement* otherRaw = other.get();
    rootRaw->AddChild(std::move(other));

    auto doomed = MakeElement("doomed");
    UIElement* doomedRaw = doomed.get();
    Watch(*doomed, "doomed", log);
    rootRaw->AddChild(std::move(doomed));
    ui.Update(0.0f, false);
    ASSERT_EQ(log.CountOf("attach:doomed"), 1u);

    // THE ONLY DIFFERENCE: the target parent is `other`, not the parent mid-RemoveChild.
    doomedRaw->RegisterEventHandler(kEventDetachedFromPanel,
                                    [&](UIEvent&)
                                    {
                                        for (int i = 0; i < 8; ++i)
                                            otherRaw->AddChild(MakeElement("sibling"));
                                    });

    rootRaw->RemoveChild(doomedRaw);
    ui.Update(0.0f, false);
    EXPECT_EQ(rootRaw->FindById("doomed"), nullptr);
    SUCCEED();
}
