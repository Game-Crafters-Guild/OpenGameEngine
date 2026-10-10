// What a UIManager leaves behind when it is destroyed. Elements it owns but does not destroy
// (externally owned Mount targets: a dock panel in a floating window that closes) outlive it,
// and must not keep a pointer to it.
//
// The managers here have no device: nothing is rendered, and teardown is the subject.
#include <gtest/gtest.h>

#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIManagerRef.h"
#include "UI/UiContext.h"

#include <memory>
#include <new>

using namespace GameEngine;

namespace
{

// root
//  +- mount  -> target (externally owned)
struct MountHost
{
    std::unique_ptr<UIManager> Manager;
    Mount* HostMount = nullptr;
};

MountHost MakeMountHost()
{
    MountHost host;
    host.Manager = std::make_unique<UIManager>(nullptr);
    auto root = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();
    host.HostMount = mount.get();
    root->AddChild(std::move(mount));
    host.Manager->SetRoot(std::move(root));
    return host;
}

// Records what a UIManagerRef to its manager answers while the probe is being destroyed.
struct RefProbeRecord
{
    bool Destroyed = false;
    bool ResolvedDuringDestruction = false;
};

class RefProbe final : public UIElement
{
  public:
    RefProbe(const UIManagerRef& ref, RefProbeRecord& record) : m_Ref(ref), m_Record(record) {}
    ~RefProbe() override
    {
        m_Record.Destroyed = true;
        m_Record.ResolvedDuringDestruction = m_Ref.Get() != nullptr;
    }

  private:
    const UIManagerRef& m_Ref;
    RefProbeRecord& m_Record;
};

struct AlignedManagerStorageDelete
{
    void operator()(void* p) const { ::operator delete(p, std::align_val_t(alignof(UIManager))); }
};

// A manager constructed in `storage` whose tree mounts `target` (externally owned).
UIManager* ConstructMountingManager(void* storage, UIElement* target, Mount*& hostMount)
{
    auto* manager = new (storage) UIManager(nullptr);
    auto root = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();
    hostMount = mount.get();
    root->AddChild(std::move(mount));
    manager->SetRoot(std::move(root));
    hostMount->SetTarget(target);
    return manager;
}

// Posts a safe action that counts its runs from `element`, as UI code running inside `updating`'s
// update would: with that manager's dispatcher current.
bool PostCountingSafeActionDuringUpdateOf(UIManager& updating, UIElement& element, int& runs)
{
    UI::UiContextScope scope(updating.GetDispatcher(), nullptr);
    return element.PostSafeAction([&runs]() { ++runs; });
}

} // namespace

// panel
//  +- child
//  +- innerMount -> inner (externally owned: a portal inside the surviving subtree)
TEST(UIManagerTeardownTests, ElementsThatOutliveTheirManagerAreLeftWithNoOwner)
{
    auto inner = std::make_unique<UIElement>();
    auto panel = std::make_unique<UIElement>();
    auto childOwned = std::make_unique<UIElement>();
    auto innerMountOwned = std::make_unique<Mount>();
    UIElement* child = childOwned.get();
    Mount* innerMount = innerMountOwned.get();
    panel->AddChild(std::move(childOwned));
    panel->AddChild(std::move(innerMountOwned));

    {
        MountHost window = MakeMountHost();
        window.HostMount->SetTarget(panel.get());
        innerMount->SetTarget(inner.get());
        // Positive control: every survivor really was owned by the dying manager.
        ASSERT_EQ(panel->GetOwnerManager(), window.Manager.get());
        ASSERT_EQ(child->GetOwnerManager(), window.Manager.get());
        ASSERT_EQ(inner->GetOwnerManager(), window.Manager.get());
    }

    // Compared as values only: a failure here is a dangling pointer, never dereferenced.
    EXPECT_EQ(panel->GetOwnerManager(), nullptr);
    EXPECT_EQ(child->GetOwnerManager(), nullptr);
    EXPECT_EQ(inner->GetOwnerManager(), nullptr) << "a portal target inside the survivor kept its owner";

    // Re-homing a detached survivor is an ordinary attach.
    MountHost next = MakeMountHost();
    next.HostMount->SetTarget(panel.get());
    EXPECT_EQ(panel->GetOwnerManager(), next.Manager.get());
    EXPECT_EQ(child->GetOwnerManager(), next.Manager.get());
    next.HostMount->SetTarget(nullptr);
}

// The case a liveness check keyed on the address gets wrong: a second manager constructed where
// the first one was. Placement construction makes the reuse certain instead of waiting for the
// allocator to repeat an address.
TEST(UIManagerTeardownTests, AReferenceDoesNotResolveToANewManagerAtTheSameAddress)
{
    std::unique_ptr<void, AlignedManagerStorageDelete> storage(
        ::operator new(sizeof(UIManager), std::align_val_t(alignof(UIManager))));

    UIManager* first = new (storage.get()) UIManager(nullptr);
    const UIManagerRef firstRef(first);
    ASSERT_EQ(firstRef.Get(), first) << "positive control: a reference to a live manager resolves";
    first->~UIManager();

    UIManager* second = new (storage.get()) UIManager(nullptr);
    ASSERT_EQ(second, first) << "precondition: the second manager occupies the first one's address";
    EXPECT_EQ(firstRef.Get(), nullptr) << "a reference to a destroyed manager resolved to its successor";

    const UIManagerRef secondRef(second);
    EXPECT_EQ(secondRef.Get(), second);
    second->~UIManager();
    EXPECT_EQ(secondRef.Get(), nullptr);
}

// Elements destroyed with their manager's tree must already see it as gone, so nothing they
// hold can call back into a manager that is tearing down.
TEST(UIManagerTeardownTests, AReferenceReadsTheManagerAsGoneForTheWholeTeardown)
{
    UIManagerRef ref;
    RefProbeRecord record;
    {
        UIManager ui(nullptr);
        ref = UIManagerRef(&ui);
        ui.SetRoot(std::make_unique<RefProbe>(ref, record));
        ASSERT_EQ(ref.Get(), &ui) << "positive control: the reference resolves before teardown";
    }
    ASSERT_TRUE(record.Destroyed) << "the probe was not destroyed with its manager's tree";
    EXPECT_FALSE(record.ResolvedDuringDestruction)
        << "an element destroyed inside ~UIManager saw the manager as alive";
}

// On the UI thread a safe action is queued on the dispatcher of the manager that is updating,
// which need not be the element's own: a floating window's element posting during the main
// window's update. If the element's manager is destroyed before that drain, and a new manager
// takes its address and mounts the element, the action must still read its manager as gone.
TEST(UIManagerTeardownTests, ASafeActionInAnotherManagersQueueDoesNotRunUnderItsOwnersSuccessor)
{
    UIManager drainer(nullptr);
    auto element = std::make_unique<UIElement>();
    std::unique_ptr<void, AlignedManagerStorageDelete> storage(
        ::operator new(sizeof(UIManager), std::align_val_t(alignof(UIManager))));
    Mount* hostMount = nullptr;
    UIManager* owner = ConstructMountingManager(storage.get(), element.get(), hostMount);
    ASSERT_EQ(element->GetOwnerManager(), owner);

    int runs = 0;
    ASSERT_TRUE(PostCountingSafeActionDuringUpdateOf(drainer, *element, runs));
    drainer.DrainDeferredActionsOnce();
    ASSERT_EQ(runs, 1) << "positive control: the other manager's drain runs a live element's action";

    ASSERT_TRUE(PostCountingSafeActionDuringUpdateOf(drainer, *element, runs));
    owner->~UIManager();
    UIManager* successor = ConstructMountingManager(storage.get(), element.get(), hostMount);
    ASSERT_EQ(successor, owner) << "precondition: the successor occupies the owner's address";
    ASSERT_EQ(element->GetOwnerManager(), successor);

    drainer.DrainDeferredActionsOnce();
    EXPECT_EQ(runs, 1) << "the action ran because its owner's successor resolved the element";

    hostMount->SetTarget(nullptr);
    successor->~UIManager();
}
