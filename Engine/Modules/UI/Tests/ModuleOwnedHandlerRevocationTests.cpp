// Unload-time revocation of module-owned UI event handlers — bookkeeping.
//
// The cross-image half of this feature (does a callable minted in a separately
// linked DLL actually identify that DLL, and does revocation survive a real
// FreeLibrary) is proven in Tests/NativeScripting/NativeModuleImageOwnershipTests
// against a genuine loaded module. THIS file covers the part that needs neither:
// the stamp/index bookkeeping, which has to stay exact through unregister,
// dispatch compaction, `once` handlers, churn and element destruction, because a
// leaked index entry is a dangling pointer and a lost one is an un-revoked
// handler.
//
// SEVERAL OF THE RE-ENTRANCY PINS HERE ARE ASan-ONLY INSTRUMENTS, and they say
// so individually. What they drive is a write into (or a read out of) freed heap;
// an ordinary allocator hands the memory back without complaint and the process
// dies later, in an unrelated test. Their assertions pass either way. The merge
// gate for this module is therefore an ASan run — a green ordinary run says
// nothing about them.
//
// The model: publish THIS TEST BINARY'S image range as if it were a
// hot-swappable module. Handlers minted in this file then stamp exactly as a
// user module's would, while handlers registered by engine controls stay
// Engine.dll's — a faithful two-image split, since UITests.exe and Engine.dll
// really are two images. That is what makes the "engine handlers survive"
// assertion below meaningful rather than tautological.

#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/Controls/AxisHeaderBar.h"
#include "UI/Controls/TreeView.h"
#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

using namespace GameEngine;


namespace
{

struct Range
{
    std::uint64_t Base = 0;
    std::uint64_t Size = 0;
};

Range ImageRangeOf(void* moduleHandle)
{
#if defined(_WIN32)
    Range r;
    const auto* base = static_cast<const unsigned char*>(moduleHandle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return r;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return r;
    r.Base = reinterpret_cast<std::uint64_t>(base);
    r.Size = nt->OptionalHeader.SizeOfImage;
    return r;
#else
    (void)moduleHandle;
    return Range{};
#endif
}

Range ThisBinaryRange()
{
#if defined(_WIN32)
    return ImageRangeOf(::GetModuleHandleW(nullptr));
#else
    return Range{};
#endif
}

void Send(UIElement& el, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// Publishes this binary's range as a hot-swappable image for the duration of a
// test, and retracts it afterwards so one test's range can never leak into the
// next (the state is process-wide by design).
class PretendModuleImage
{
  public:
    PretendModuleImage()
        : m_Range(ThisBinaryRange())
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(m_Range.Base, m_Range.Size);
    }
    ~PretendModuleImage()
    {
        UI::RevokeHandlersOwnedByImage(m_Range.Base, m_Range.Size);
        UI::RetractHotSwappableImage(m_Range.Base);
    }
    PretendModuleImage(const PretendModuleImage&) = delete;
    PretendModuleImage& operator=(const PretendModuleImage&) = delete;

    std::uint64_t Base() const { return m_Range.Base; }
    std::uint64_t Size() const { return m_Range.Size; }
    std::size_t Revoke() const { return UI::RevokeHandlersOwnedByImage(m_Range.Base, m_Range.Size); }
    std::size_t Pins() const { return UI::CountHandlersOwnedByImage(m_Range.Base, m_Range.Size); }

  private:
    Range m_Range;
};

// Publishes an arbitrary image range for the duration of a scope, revoking and
// retracting on the way out however the scope exits. The process-wide state is
// shared with every other test in this binary, so unwinding must not depend on
// the test body reaching its last line.
class PublishedImage
{
  public:
    explicit PublishedImage(Range r) : m_Range(r)
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(m_Range.Base, m_Range.Size);
    }
    ~PublishedImage()
    {
        UI::RevokeHandlersOwnedByImage(m_Range.Base, m_Range.Size);
        UI::RetractHotSwappableImage(m_Range.Base);
    }
    PublishedImage(const PublishedImage&) = delete;
    PublishedImage& operator=(const PublishedImage&) = delete;

  private:
    Range m_Range;
};

class ModuleOwnedHandlers : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#if !defined(_WIN32)
        GTEST_SKIP() << "module image-range stamping is the Windows hot-reload "
                        "mechanism; ImageRangeOf reads PE headers";
#endif
        ASSERT_NE(ThisBinaryRange().Base, 0u) << "cannot read this binary's image range";
        // Nothing may be indexed before a test starts, or its assertions about
        // the index are measuring another test's leftovers.
        ASSERT_EQ(UI::IndexedElementCount(), 0u);
    }
    void TearDown() override { EXPECT_EQ(UI::IndexedElementCount(), 0u) << "index leaked"; }
};

// ---------------------------------------------------------------------------
// The constraint. Engine-owned handlers are not merely skipped by revocation —
// they are never stamped, so there is nothing for revocation to find.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, EngineRegisteredHandlersAreNeverStamped)
{
    PretendModuleImage image;

    // AxisHeaderBar's constructor registers its own handlers from Engine.dll.
    AxisHeaderBar bar;
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(bar), 0u)
        << "an engine control's own handlers were stamped as module-owned";
    EXPECT_EQ(UI::IndexedElementCount(), 0u)
        << "an engine control entered the revocation index";
    EXPECT_EQ(image.Pins(), 0u);
}

TEST_F(ModuleOwnedHandlers, EngineHandlersSurviveARevocationPass)
{
    PretendModuleImage image;

    AxisHeaderBar bar;
    const std::size_t before = UIEventHandlerAccess::HandlerCount(bar, kEventMouseMove);
    ASSERT_GT(before, 0u) << "AxisHeaderBar no longer registers a MouseMove handler";

    // A user element registers alongside, so the pass has something real to do.
    UIElement userElement;
    int userFired = 0;
    userElement.RegisterEventHandler(kEventMouseUp, [&userFired](UIEvent&) { ++userFired; });

    EXPECT_EQ(image.Revoke(), 1u);

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(bar, kEventMouseMove), before)
        << "revocation touched an engine control's handlers";
    // Still live: the engine handler dispatches without faulting or being swept.
    Send(bar, kEventMouseMove);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(bar, kEventMouseMove), before);
}

// ---------------------------------------------------------------------------
// Revocation itself.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, RevocationReleasesTheCallableAndStopsDispatch)
{
    PretendModuleImage image;

    UIElement el;
    int fired = 0;
    const auto token = el.RegisterEventHandler(kEventMouseUp, [&fired](UIEvent&) { ++fired; });
    ASSERT_TRUE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, token.Key));
    EXPECT_EQ(UI::IndexedElementCount(), 1u);
    EXPECT_EQ(image.Pins(), 1u);

    Send(el, kEventMouseUp);
    ASSERT_EQ(fired, 1);

    EXPECT_EQ(image.Revoke(), 1u);
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(el, kEventMouseUp, token.Key))
        << "revocation left the callable alive; the unmap would free code under it";
    EXPECT_EQ(image.Pins(), 0u) << "the ledger would still refuse the unmap";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);

    Send(el, kEventMouseUp);
    EXPECT_EQ(fired, 1) << "a revoked handler fired";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u)
        << "the dispatch compaction did not sweep the revoked entry";
}

// ---------------------------------------------------------------------------
// Member callback slots (Button's SetOnMouseDown, TreeView's fifteen).
//
// These hold a module's callable outside m_EventHandlers, so they need the same
// three properties the table has: the stamp is taken from the callable, the
// image reads as pinned while the slot holds it, and revocation both releases
// the callable and stops it firing.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, MemberSlotFromProbeImageIsClearedByRevoke)
{
    PretendModuleImage image;

    Button btn;
    int mouseDown = 0;
    btn.SetOnMouseDown([&mouseDown](Button&) { ++mouseDown; });
    ASSERT_TRUE(UIEventHandlerAccess::OnMouseDownIsStamped(btn));
    ASSERT_TRUE(UIEventHandlerAccess::OnMouseDownHasCallable(btn));
    EXPECT_EQ(UI::IndexedElementCount(), 1u);
    EXPECT_EQ(image.Pins(), 1u);

    Send(btn, kEventMouseDown);
    ASSERT_EQ(mouseDown, 1);

    EXPECT_EQ(image.Revoke(), 1u);
    EXPECT_FALSE(UIEventHandlerAccess::OnMouseDownHasCallable(btn));
    EXPECT_FALSE(UIEventHandlerAccess::OnMouseDownIsStamped(btn));
    EXPECT_EQ(image.Pins(), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);

    Send(btn, kEventMouseDown);
    EXPECT_EQ(mouseDown, 1) << "a revoked SetOnMouseDown fired";
}

TEST_F(ModuleOwnedHandlers, TwoMemberSlotsOnOneControlArePinnedAndRevokedTogether)
{
    PretendModuleImage image;

    TreeView tv;
    tv.SetOnSelectionChanged([](TreeId) {});
    tv.SetOnItemActivated([](TreeId) {});
    EXPECT_EQ(image.Pins(), 2u);

    EXPECT_EQ(image.Revoke(), 2u);
    EXPECT_FALSE(UIEventHandlerAccess::SelectionChangedHasCallable(tv));
    EXPECT_FALSE(UIEventHandlerAccess::ItemActivatedHasCallable(tv));
    EXPECT_EQ(image.Pins(), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// The ownership rule, on the member-slot path: only a module-owned
// callable is revocable. An engine-owned one is not stamped, so it cannot be.
TEST_F(ModuleOwnedHandlers, MemberSlotIsNotStampedWhileNoImageIsMapped)
{
    Button btn;
    btn.SetOnMouseDown([](Button&) {});
    EXPECT_FALSE(UIEventHandlerAccess::OnMouseDownIsStamped(btn));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// Rebinding a slot must hand the count back before it takes it again, or the
// element leaks an index entry and never leaves the index.
TEST_F(ModuleOwnedHandlers, RebindingAMemberSlotDoesNotLeakTheStampCount)
{
    PretendModuleImage image;

    Button btn;
    btn.SetOnMouseDown([](Button&) {});
    btn.SetOnMouseDown([](Button&) {});
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 1u);
    EXPECT_EQ(image.Pins(), 1u);

    btn.SetOnMouseDown(nullptr);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// The same guard the table carries, now shared rather than duplicated: a slot
// running its callable keeps it, so the ledger refuses the unmap and the frame
// returns into a mapped image.
TEST_F(ModuleOwnedHandlers, AnExecutingMemberSlotKeepsItsCallableAcrossRevocation)
{
    PretendModuleImage image;

    Button btn;
    std::size_t revokedDuringCall = 0;
    std::size_t pinsDuringCall = 0;
    bool ranToCompletion = false;
    btn.SetOnMouseDown([&](Button&) {
        revokedDuringCall = image.Revoke();
        pinsDuringCall = image.Pins();
        ranToCompletion = true;
    });

    Send(btn, kEventMouseDown);

    EXPECT_TRUE(ranToCompletion);
    EXPECT_EQ(revokedDuringCall, 0u)
        << "revocation destroyed a member-slot callable under its own frame";
    EXPECT_EQ(pinsDuringCall, 1u)
        << "an executing slot must still read as a pin, so the unmap is refused";
    EXPECT_TRUE(UIEventHandlerAccess::OnMouseDownHasCallable(btn));
}

// ---------------------------------------------------------------------------
// TreeView's fifteen slots.
//
// Button proves the slot protocol on two members; TreeView is where it has to
// scale, and where the failure mode changes shape: with fifteen slots the risk
// is no longer "does stamping work" but "is every slot reached by all four
// revocation virtuals". A slot listed in Count but forgotten in Release reads as
// a pin that revocation can never clear, which refuses the unmap forever; the
// reverse silently leaves a module callable live across the unmap. The aggregate
// arm below is what makes a forgotten slot fail rather than pass quietly.
// ---------------------------------------------------------------------------

// Sets all fifteen. The counts are the assertion: anything less than 15 means a
// slot is missing from ForEachOwnedSlot, or from one of the virtuals that walks it.
TEST_F(ModuleOwnedHandlers, EveryTreeViewMemberSlotIsPinnedAndRevokedTogether)
{
    PretendModuleImage image;

    TreeView tv;
    tv.SetOnItemResizeGesture([](float) {});
    tv.SetOnExpansionChanged([](TreeId, bool) {});
    tv.SetOnScrollChanged([](float) {});
    tv.SetOnSelectionChanged([](TreeId) {});
    tv.SetOnItemActivated([](TreeId) {});
    tv.SetOnContextMenu([](TreeId, float, float) {});
    tv.SetOnRowBound([](TreeId, UIElement*) {});
    tv.SetOnIconClicked([](TreeId, UIElement*) {});
    tv.SetOnIconStateCheck([](TreeId) { return true; });
    tv.SetOnIconStateSet([](TreeId, bool, UIElement*) {});
    tv.SetOnLockClicked([](TreeId, UIElement*) {});
    tv.SetOnLockStateCheck([](TreeId) { return true; });
    tv.SetOnLockStateSet([](TreeId, bool, UIElement*) {});
    tv.SetOnCanDrop([](const UI::Interaction::DropRequest&) {
        return UI::Interaction::DropFeedback{true, ""};
    });
    tv.SetOnPerformDrop([](const UI::Interaction::DropRequest&) {});

    EXPECT_EQ(image.Pins(), 15u) << "a slot is missing from CountMemberSlotsOwnedByImage";
    EXPECT_EQ(UI::IndexedElementCount(), 1u);

    EXPECT_EQ(image.Revoke(), 15u)
        << "a slot is missing from CollectMemberSlotsOwnedByImage or ReleaseCollectedMemberSlots";

    // Pins() alone CANNOT catch a forgotten slot, and it is important to know why:
    // a slot missing from ForEachOwnedSlot is invisible to CountMemberSlotsOwnedByImage
    // too, so it reads 0 here while the slot still holds the module's callable. The
    // ledger would then see nothing owed and ALLOW the unmap, leaving a std::function
    // pointing into freed code — the exact bug this whole protocol exists to prevent.
    EXPECT_EQ(image.Pins(), 0u);

    // THIS is the assertion that catches it. The element's own m_OwnedHandlerCount is
    // maintained by the stamping path (operator=), not by the walk, so a slot the walk
    // forgot is still counted here and the element never leaves the index. A non-zero
    // count after a full revoke means some slot was stamped but never released.
    EXPECT_EQ(UI::IndexedElementCount(), 0u)
        << "the element is still indexed after revoking every slot: a stamped slot was "
           "never released, so its module callable survives the unmap";
}

// Revocation must stop the callable firing, not merely drop the stamp.
TEST_F(ModuleOwnedHandlers, ARevokedTreeViewSlotStopsFiring)
{
    PretendModuleImage image;

    TreeView tv;
    int expansions = 0;
    tv.SetOnExpansionChanged([&expansions](TreeId, bool) { ++expansions; });
    EXPECT_EQ(image.Pins(), 1u);

    tv.SetExpanded(7, true);
    ASSERT_EQ(expansions, 1);

    EXPECT_EQ(image.Revoke(), 1u);
    tv.SetExpanded(7, false);
    EXPECT_EQ(expansions, 1) << "a revoked TreeView slot fired";
}

// The executing-frame rule, now supplied by the slot type itself rather than by
// each control remembering to open a scope by hand.
TEST_F(ModuleOwnedHandlers, AnExecutingTreeViewSlotKeepsItsCallableAcrossRevocation)
{
    PretendModuleImage image;

    TreeView tv;
    std::size_t revokedDuringCall = 0;
    std::size_t pinsDuringCall = 0;
    bool ranToCompletion = false;
    tv.SetOnExpansionChanged([&](TreeId, bool) {
        revokedDuringCall = image.Revoke();
        pinsDuringCall = image.Pins();
        ranToCompletion = true;
    });

    tv.SetExpanded(7, true);

    EXPECT_TRUE(ranToCompletion);
    EXPECT_EQ(revokedDuringCall, 0u)
        << "revocation destroyed a TreeView slot's callable under its own frame";
    EXPECT_EQ(pinsDuringCall, 1u)
        << "an executing slot must still read as a pin, so the unmap is refused";
}

// A non-void slot returns its callable's value THROUGH the executing-frame scope
// (`return m_Fn(...)` with a live scope object in the frame). Every other arm here
// drives void slots, so without this the return path is compiled but never run —
// and a slot type that dropped or mangled the value would pass all of them.
TEST_F(ModuleOwnedHandlers, ANonVoidSlotReturnsItsCallablesValueThroughTheScope)
{
    PretendModuleImage image;

    TreeView tv;
    // bool(TreeId): the value must survive the scope in both directions, so the
    // callable answers from its argument rather than returning a constant.
    tv.SetOnIconStateCheck([](TreeId id) { return id == 42; });
    EXPECT_EQ(image.Pins(), 1u);

    EXPECT_TRUE(UIEventHandlerAccess::InvokeIconStateCheck(tv, 42));
    EXPECT_FALSE(UIEventHandlerAccess::InvokeIconStateCheck(tv, 7));

    // The scope must be balanced on the way out: a slot that left its frame on the
    // element's executing list would read as executing forever and never revoke.
    EXPECT_EQ(image.Revoke(), 1u) << "the executing-frame scope leaked past the call";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// The ownership rule on TreeView's slots: an engine-owned callable is
// not stamped, so it is structurally unrevocable.
TEST_F(ModuleOwnedHandlers, TreeViewSlotIsNotStampedWhileNoImageIsMapped)
{
    TreeView tv;
    tv.SetOnSelectionChanged([](TreeId) {});
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(tv), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// Rebinding must hand the count back before taking it again, or the element
// leaks an index entry and never leaves the index.
TEST_F(ModuleOwnedHandlers, RebindingATreeViewSlotDoesNotLeakTheStampCount)
{
    PretendModuleImage image;

    TreeView tv;
    tv.SetOnSelectionChanged([](TreeId) {});
    tv.SetOnSelectionChanged([](TreeId) {});
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(tv), 1u);
    EXPECT_EQ(image.Pins(), 1u);

    tv.SetOnSelectionChanged(nullptr);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(tv), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// The per-slot memory cost, measured rather than inferred: the estimate the slot
// protocol was designed against was 8 bytes of owner tag plus 8 bytes of
// back-pointer per slot. TreeView carries fifteen slots, so whatever this is, a
// tree view pays it fifteen times.
TEST_F(ModuleOwnedHandlers, SlotOverheadOverARawStdFunctionIsBounded)
{
    using Slot = UI::ModuleOwnedCallback<void(TreeId)>;
    using Raw = std::function<void(TreeId)>;
    const std::size_t overhead = sizeof(Slot) - sizeof(Raw);

    std::cout << "[ MEASURED ] ModuleOwnedCallback " << sizeof(Slot) << " B, std::function "
              << sizeof(Raw) << " B, overhead " << overhead << " B/slot ("
              << (overhead * 15) << " B per TreeView)\n";

    // Owner tag (8) + element back-pointer (8) + slot bit (4) + padding. Anything
    // larger means a field was added without accounting for it fifteen times over.
    EXPECT_LE(overhead, 24u);
}

TEST_F(ModuleOwnedHandlers, NothingIsStampedWhileNoImageIsMapped)
{
    UIElement el;
    const auto token = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    EXPECT_FALSE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, token.Key));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

TEST_F(ModuleOwnedHandlers, AnExecutingHandlerKeepsItsCallableAcrossRevocation)
{
    PretendModuleImage image;

    UIElement el;
    std::size_t revokedDuringCall = 0;
    std::size_t pinsDuringCall = 0;
    bool ranToCompletion = false;

    el.RegisterEventHandler(kEventMouseUp, [&](UIEvent&) {
        // Revoke from inside the very handler that would be revoked.
        revokedDuringCall = image.Revoke();
        pinsDuringCall = image.Pins();
        // Still executing out of this callable's captures afterwards.
        ranToCompletion = true;
    });

    Send(el, kEventMouseUp);

    EXPECT_TRUE(ranToCompletion);
    EXPECT_EQ(revokedDuringCall, 0u) << "revocation destroyed a callable under its own frame";
    EXPECT_EQ(pinsDuringCall, 1u)
        << "an executing handler must still read as a pin, so the unmap is refused rather "
           "than freeing code under a live frame";
    // The dispatch's compaction sweeps the deactivated entry on the way out.
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
    EXPECT_EQ(image.Pins(), 0u);
}

// ---------------------------------------------------------------------------
// Index removal — every path that can drop an entry.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, ExplicitUnregisterLeavesTheIndex)
{
    PretendModuleImage image;

    UIElement el;
    const auto token = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    ASSERT_EQ(UI::IndexedElementCount(), 1u);

    EXPECT_TRUE(el.UnregisterEventHandler(token));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u) << "unregister left the element indexed";
    EXPECT_EQ(image.Pins(), 0u);
}

TEST_F(ModuleOwnedHandlers, DispatchCompactionLeavesTheIndexForOnceHandlers)
{
    PretendModuleImage image;

    UIElement el;
    int fired = 0;
    el.RegisterEventHandlerOnce(kEventMouseUp, [&fired](UIEvent&) { ++fired; });
    ASSERT_EQ(UI::IndexedElementCount(), 1u);

    Send(el, kEventMouseUp);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u)
        << "a fired `once` handler left its stamp behind";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

TEST_F(ModuleOwnedHandlers, SelfUnregisterLeavesTheIndexViaCompaction)
{
    PretendModuleImage image;

    UIElement el;
    UIElement::EventHandlerToken token{};
    token = el.RegisterEventHandler(kEventMouseUp, [&](UIEvent&) {
        // Unregistering from inside keeps the callable (it is running) and the
        // stamp with it; only the compaction can clear either.
        el.UnregisterEventHandler(token);
        EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 1u);
    });

    Send(el, kEventMouseUp);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

TEST_F(ModuleOwnedHandlers, ElementDestructionLeavesTheIndex)
{
    PretendModuleImage image;
    {
        UIElement el;
        el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
        ASSERT_EQ(UI::IndexedElementCount(), 1u);
    }
    EXPECT_EQ(UI::IndexedElementCount(), 0u)
        << "a destroyed element stayed in the index — the next unload would walk a dangling pointer";
}

// ---------------------------------------------------------------------------
// Multiplicity — several stamps per element, and churn.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, ManyStampedHandlersOnOneElementRevokeTogether)
{
    PretendModuleImage image;

    UIElement el;
    int a = 0, b = 0, c = 0;
    el.RegisterEventHandler(kEventMouseUp, [&a](UIEvent&) { ++a; });
    el.RegisterEventHandler(kEventMouseUp, [&b](UIEvent&) { ++b; });
    el.RegisterEventHandler(kEventMouseDown, [&c](UIEvent&) { ++c; });

    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 3u);
    EXPECT_EQ(UI::IndexedElementCount(), 1u) << "the element must be indexed ONCE, not per handler";
    EXPECT_EQ(image.Pins(), 3u);

    EXPECT_EQ(image.Revoke(), 3u);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u);
    EXPECT_EQ(UI::IndexedElementCount(), 0u);

    Send(el, kEventMouseUp);
    Send(el, kEventMouseDown);
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(c, 0);
}

TEST_F(ModuleOwnedHandlers, PartialUnregisterKeepsTheElementIndexed)
{
    PretendModuleImage image;

    UIElement el;
    const auto first = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    ASSERT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 2u);

    EXPECT_TRUE(el.UnregisterEventHandler(first));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 1u);
    EXPECT_EQ(UI::IndexedElementCount(), 1u)
        << "dropping one of two stamps de-indexed the element, losing the survivor";
    EXPECT_EQ(image.Pins(), 1u);
}

TEST_F(ModuleOwnedHandlers, RegisterUnregisterRegisterChurnKeepsTheCountExact)
{
    PretendModuleImage image;

    UIElement el;
    for (int i = 0; i < 8; ++i)
    {
        const auto token = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
        EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 1u) << "iteration " << i;
        EXPECT_EQ(UI::IndexedElementCount(), 1u) << "iteration " << i;
        EXPECT_TRUE(el.UnregisterEventHandler(token));
        EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u) << "iteration " << i;
        EXPECT_EQ(UI::IndexedElementCount(), 0u) << "iteration " << i;
    }
    // The deactivated entries are still on the vector until a dispatch compacts
    // them; none of them may resurrect a stamp.
    Send(el, kEventMouseUp);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), 0u);
}

// ---------------------------------------------------------------------------
// Two images on one element: revoking one must not touch the other.
// Windows-only: the second image is looked up as a loaded DLL by name, and only
// the DLL build has an Engine.dll to name (macOS links Engine into the test).
// ---------------------------------------------------------------------------

#if defined(_WIN32)
TEST_F(ModuleOwnedHandlers, RevokingOneImageLeavesTheOtherImagesHandlersOnTheSameElement)
{
    // Two module images, ONE element. Engine.dll stands in for the second module:
    // it is genuinely a different image, so publishing both ranges puts handlers
    // from two images on a single element's table — the shape a project module and
    // a package module produce when both wire the same button.
    const Range self = ThisBinaryRange();
    const Range other = ImageRangeOf(::GetModuleHandleW(L"Engine.dll"));
    ASSERT_NE(other.Base, 0u);
    ASSERT_NE(other.Base, self.Base);

    // Scope-guarded, not unwound at the end of the body: an ASSERT firing above
    // would otherwise leave Engine.dll published as hot-swappable for the ~1200
    // remaining tests in this process, stamping every engine handler and turning
    // one real failure into a storm of TearDown failures.
    PublishedImage otherImage(other);

    // Its constructor registers two handlers, minted in Engine.dll.
    AxisHeaderBar el;
    const std::uint32_t fromOther = UIEventHandlerAccess::OwnedHandlerCount(el);
    ASSERT_GT(fromOther, 0u) << "publishing Engine.dll's range did not make its handlers stampable";

    PublishedImage selfImage(self);

    // ...and now one minted HERE, on that same element.
    int mine = 0;
    const auto mineToken =
        el.RegisterEventHandler(kEventMouseUp, [&mine](UIEvent&) { ++mine; });
    ASSERT_TRUE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, mineToken.Key));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), fromOther + 1);
    EXPECT_EQ(UI::IndexedElementCount(), 1u) << "one element, indexed once, whatever the image mix";

    // Revoke only this binary's image.
    EXPECT_EQ(UI::RevokeHandlersOwnedByImage(self.Base, self.Size), 1u);
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(el, kEventMouseUp, mineToken.Key));
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(el), fromOther)
        << "revoking one image released the other image's handlers on the same element";
    EXPECT_EQ(UI::CountHandlersOwnedByImage(self.Base, self.Size), 0u);
    EXPECT_EQ(UI::CountHandlersOwnedByImage(other.Base, other.Size), fromOther);
    EXPECT_EQ(UI::IndexedElementCount(), 1u)
        << "the element still holds the other image's handlers and must stay indexed";

    // Both ranges are unwound by the scope guards above.
}
#endif // _WIN32

// ---------------------------------------------------------------------------
// Reentrancy: revoking runs the module's destructors, and those are module code.
//
// A revoked closure's captures are destroyed as part of revocation. Those
// destructors can do anything the module could do at any other time — including
// registering handlers, which reallocates the very vector a naive walk holds a
// reference into and inserts into the very map it is iterating. Both arms below
// failed before the sink: the first diverged the stamp counter from the true
// stamped-entry count by writing through a reference into freed pre-reallocation
// storage; the second walked into map nodes inserted mid-walk and revoked
// handlers that were registered AFTER revocation began.
// ---------------------------------------------------------------------------

namespace
{
// Registers handlers on an element when destroyed — the stand-in for a module
// closure whose captures re-enter the UI on teardown.
//
// Held by shared_ptr and ARMED AFTER registration on purpose. A bare capture
// would also be destroyed with every temporary the registration itself creates
// (the lambda copied into the std::function), firing the re-entry before
// revocation ever runs and measuring the wrong thing entirely. Through a
// shared_ptr the payload is destroyed exactly once: when the last copy of the
// handler's callable goes, which is the release under test.
struct ReentrantBomb
{
    UIElement* El = nullptr;
    EventId Id{};
    int Count = 0;
    bool SpreadOverEventIds = false;
    int* Registered = nullptr;

    ~ReentrantBomb()
    {
        for (int i = 0; i < Count; ++i)
        {
            El->RegisterEventHandler(SpreadOverEventIds ? static_cast<EventId>(Id + 1 + i) : Id,
                                     [](UIEvent&) {});
            if (Registered)
                ++*Registered;
        }
    }
};
} // namespace

TEST_F(ModuleOwnedHandlers, ARevokedHandlerWhoseDestructorRegistersDoesNotCorruptTheWalk)
{
    PretendModuleImage image;

    UIElement el;
    int registeredDuringRevoke = 0;
    auto bomb = std::make_shared<ReentrantBomb>();
    el.RegisterEventHandler(kEventMouseUp, [bomb](UIEvent&) { (void)bomb; });
    // Armed only now, so the registration's own temporaries cannot fire it.
    bomb->El = &el;
    bomb->Id = kEventMouseUp;
    bomb->Count = 64; // enough to reallocate the vector
    bomb->SpreadOverEventIds = false;
    bomb->Registered = &registeredDuringRevoke;
    bomb.reset(); // the handler's callable now holds the only reference

    const std::size_t revoked = image.Revoke();

    EXPECT_EQ(revoked, 1u)
        << "the pass revoked handlers that were registered after it began";
    EXPECT_GT(registeredDuringRevoke, 0) << "the attack did not actually re-enter";
    EXPECT_EQ(static_cast<std::size_t>(UIEventHandlerAccess::OwnedHandlerCount(el)), image.Pins())
        << "the stamp counter diverged from the actual stamped entries";
}

TEST_F(ModuleOwnedHandlers, ARevokedHandlerWhoseDestructorAddsEventIdsDoesNotCorruptTheWalk)
{
    PretendModuleImage image;

    UIElement el;
    int registeredDuringRevoke = 0;
    auto bomb = std::make_shared<ReentrantBomb>();
    el.RegisterEventHandler(kEventMouseUp, [bomb](UIEvent&) { (void)bomb; });
    // Armed only now, so the registration's own temporaries cannot fire it.
    bomb->El = &el;
    bomb->Id = kEventMouseUp;
    bomb->Count = 64; // enough to reallocate the vector / rehash the map
    bomb->SpreadOverEventIds = true;
    bomb->Registered = &registeredDuringRevoke;
    bomb.reset(); // the handler's callable now holds the only reference

    const std::size_t revoked = image.Revoke();

    EXPECT_EQ(revoked, 1u)
        << "the pass revoked handlers that were registered after it began";
    EXPECT_GT(registeredDuringRevoke, 0) << "the attack did not actually re-enter";
    EXPECT_EQ(static_cast<std::size_t>(UIEventHandlerAccess::OwnedHandlerCount(el)), image.Pins())
        << "the stamp counter diverged from the actual stamped entries";
}

// The same release rule on the ordinary unregister path. No module image is
// involved: UnregisterEventHandler releases the callable whoever owns it, so the
// re-entrant registration reallocates the vector under the same statement.
//
// SILENT without a checking allocator — the write lands in freed heap and the
// process dies later, in an unrelated test. Under ASan (or the Windows debug heap
// a debugger turns on) it fails at the write, which is the only place the defect
// is legible.

TEST_F(ModuleOwnedHandlers, UnregisteringAHandlerWhoseDestructorRegistersDoesNotWriteIntoFreedStorage)
{
    UIElement el;
    int registeredDuringUnregister = 0;
    auto bomb = std::make_shared<ReentrantBomb>();
    const auto token = el.RegisterEventHandler(kEventMouseUp, [bomb](UIEvent&) { (void)bomb; });
    // Armed only now, so the registration's own temporaries cannot fire it.
    bomb->El = &el;
    bomb->Id = kEventMouseUp;
    bomb->Count = 64; // enough to reallocate the vector
    bomb->SpreadOverEventIds = false;
    bomb->Registered = &registeredDuringUnregister;
    bomb.reset(); // the handler's callable now holds the only reference

    EXPECT_TRUE(el.UnregisterEventHandler(token));
    EXPECT_GT(registeredDuringUnregister, 0) << "the attack did not actually re-enter";
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(el, kEventMouseUp, token.Key))
        << "the unregistered entry kept its callable";
}

// A spent `once` handler whose capture destructor registers 64 more handlers on the
// same element and id, from inside the drain that is erasing it.
//
// WHAT THE ASSERTIONS PIN: all 64 of those registrations are in the table when the
// dispatch returns, and the spent `once` entry is not. The deque takes the appends
// without disturbing what the drain is walking, and the erase drops exactly the one
// emptied entry instead of truncating the tail that arrived behind it.
//
// WHAT THEY DO NOT PIN IS THE SINK, and this test must not be read as covering it.
// Both assertions are insensitive to WHERE the destructor runs relative to the
// erase: clearing the callable in the collect pass instead of moving it to `doomed`
// simply runs the destructor earlier, the 64 entries are appended either way, and
// the emptied entry is dropped either way. Nothing here goes red for it.
//
// Nor do the BY-VALUE capture tests below, though they are about a callable's own
// storage too. SITE 1 (UnregisteringAByValueCapturedHandlerDoesNotWriteIntoFreedStorage)
// and SITE 2 (CompactingAByValueCapturedOnceHandlerDoesNotCorruptTheSweep) attack the
// STORAGE half of the release rule — that the per-id container is node-based, so a
// registration performed from inside a destructor cannot move the entry that
// destructor is running out of. Deleting the drain's sink leaves both of them green.
// SITE 1 is not even on this path: it exercises UnregisterEventHandler's own,
// separate sink.
//
// The drain sink's red-green is
// ADrainedHandlersDestructorSeesAnErasedTableAndAnUnblockedDrain, immediately below.
TEST_F(ModuleOwnedHandlers,
       ADroppedOnceHandlerWhoseDestructorRegistersKeepsEveryNewEntry)
{
    UIElement el;
    int registeredAfterCompaction = 0;
    auto bomb = std::make_shared<ReentrantBomb>();
    el.RegisterEventHandlerOnce(kEventMouseUp, [bomb](UIEvent&) { (void)bomb; });
    bomb->El = &el;
    bomb->Id = kEventMouseUp;
    bomb->Count = 64; // enough to reallocate the vector, were it reallocated in place
    bomb->SpreadOverEventIds = false;
    bomb->Registered = &registeredAfterCompaction;
    bomb.reset();

    Send(el, kEventMouseUp);

    EXPECT_GT(registeredAfterCompaction, 0) << "the attack did not actually re-enter";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseUp),
              static_cast<std::size_t>(registeredAfterCompaction))
        << "the fired `once` entry survived compaction, or the re-entrant "
           "registrations landed in a vector the erase then truncated";
}

// ---------------------------------------------------------------------------
// DrainInactiveHandlers' `doomed` sink, and the only capture shape that can see it.
//
// The sink defers exactly one thing: WHEN a dropped callable's captures are
// destroyed. It buys nothing for an inline-held capture, because
// std::function::swap only pointer-swaps when BOTH sides are heap-held — against an
// empty sink it takes the three-way move and destroys the source in place, in the
// collect pass, sink or no sink. A HEAP-held callable is the shape the swap really
// does move, so its destructor runs where the sink says it does, and it is the only
// shape from which the deferral is observable at all.
//
// WHAT THE DEFERRAL BUYS, stated as the header states it: a destructor that
// re-enters this element gets a CONSISTENT TABLE and an UNBLOCKED DRAIN. `doomed` is
// declared before the sweep guard, so the guard dies first — the emptied entries are
// erased, the presence bits are settled and m_SweepingHandlers is down before any
// module code runs. Run that destructor in the collect pass instead and both halves
// are false: it sees the entry it was drained out of still sitting in the deque, and
// a drain it triggers returns empty-handed because the flag is still raised.
//
// The second half has to be sampled from INSIDE the destructor. The dispatch that
// started the drain still has its own tail drain to run on the way out, and that one
// mops up whatever the blocked drain left; the stranding is real but transient, and
// invisible by the time the test body regains control.
//
// Neither half is memory-unsafe on its own — the node-based storage and the walk's
// inactive-entry skip cover that, and are pinned separately by SITES 1-3 below. This
// is the behavioural pin, and it is deterministic under any allocator.
// ---------------------------------------------------------------------------

namespace
{
constexpr EventId kDrainSinkProbeEvent = HashEvent("UI.DrainSinkProbe");

struct DrainSinkFuse
{
    UIElement* El = nullptr;
    EventId DrainedId{};
    bool Detonated = false;
    std::size_t DrainedIdEntriesSeen = 0;
    std::size_t StrandedProbeEntriesSeen = 0;
};

// Armed through a pointer so the moved-from temporaries of registration cannot
// detonate; the pad is what puts the closure far past std::function's inline buffer
// and therefore on the heap, where the sink's swap is a pointer steal that runs no
// destructor at all.
struct DrainSinkBomb
{
    DrainSinkFuse* Fuse = nullptr;
    std::array<std::uint64_t, 24> Pad{};

    ~DrainSinkBomb()
    {
        if (!Fuse || !Fuse->El || Fuse->Detonated)
            return;
        Fuse->Detonated = true;
        UIElement& el = *Fuse->El;
        // Half one: is the entry this callable was drained out of already gone?
        Fuse->DrainedIdEntriesSeen = UIEventHandlerAccess::HandlerCount(el, Fuse->DrainedId);
        // Half two: leave a dead entry behind and dispatch, which is what makes a drain
        // run. Sampled HERE, inside the destructor, and not from the test body: the
        // dispatch that started all this still has its own tail drain to run on the way
        // out, and that one would sweep the strand up before the body could see it.
        const auto stillborn = el.RegisterEventHandler(kDrainSinkProbeEvent, [](UIEvent&) {});
        el.UnregisterEventHandler(stillborn);
        UIEvent e{};
        e.Id = kDrainSinkProbeEvent;
        e.Target = &el;
        e.CurrentTarget = &el;
        el.DispatchEvent(e);
        Fuse->StrandedProbeEntriesSeen =
            UIEventHandlerAccess::HandlerCount(el, kDrainSinkProbeEvent);
    }
};
} // namespace

TEST_F(ModuleOwnedHandlers, ADrainedHandlersDestructorSeesAnErasedTableAndAnUnblockedDrain)
{
    UIElement el;
    DrainSinkFuse fuse;
    el.RegisterEventHandlerOnce(kEventMouseUp, [b = DrainSinkBomb{&fuse}](UIEvent&) { (void)b; });
    // Armed only now, so the registration's own moved-from temporaries cannot fire it.
    fuse.El = &el;
    fuse.DrainedId = kEventMouseUp;

    Send(el, kEventMouseUp);

    ASSERT_TRUE(fuse.Detonated) << "the drained callable was never destroyed";
    EXPECT_EQ(fuse.DrainedIdEntriesSeen, 0u)
        << "module code ran against a table still holding the entry it was drained "
           "out of — the callable was destroyed in the collect pass instead of in "
           "the sink";
    EXPECT_EQ(fuse.StrandedProbeEntriesSeen, 0u)
        << "the drain the destructor triggered declined, because it ran with the "
           "sweep flag still raised";

    fuse.El = nullptr; // disarm: nothing may detonate against a dying element
}

// ---------------------------------------------------------------------------
// The capture shape none of the bombs above can express, at all three moments a
// callable's own storage is touched while code that registers is running.
//
// Every bomb above is held through a shared_ptr, which NULLS on move: the
// moved-from copy's destructor does nothing, so those tests never exercise a
// destructor running at the entry's own address. A by-value capture is copied by
// a move and detonates from the moved-from copy too, which is exactly what puts
// module code inside the table's storage.
//
// All three are ASan-only. Their assertions pass over freed memory just as well.
// ---------------------------------------------------------------------------

namespace
{
struct ByValueFuse
{
    UIElement* El = nullptr;
    EventId Id{};
    int Count = 0;
    int* Registered = nullptr;
};

// A plain pointer: copied by a move, never nulled by one. std::function holds the
// closure inline (one pointer), so the destructor below runs at whatever address
// the table keeps the entry at.
struct ByValueBomb
{
    ByValueFuse* Fuse = nullptr;
    ~ByValueBomb()
    {
        if (!Fuse || !Fuse->El)
            return;
        // ONE SHOT. Every copy of this closure the table moves or drops detonates,
        // so an armed fuse re-arms itself through the insertions it causes and the
        // run ends in a stack overflow instead of one legible failure.
        UIElement* const el = Fuse->El;
        Fuse->El = nullptr;
        for (int i = 0; i < Fuse->Count; ++i)
        {
            el->RegisterEventHandler(Fuse->Id, [](UIEvent&) {});
            if (Fuse->Registered)
                ++*Fuse->Registered;
        }
    }
};

// Read by its own handler AFTER that handler has registered on this same element
// and event.
struct SelfReadCapture
{
    int Magic = 0;
};
} // namespace

// SITE 1 — release. `doomed.swap(he.handler)` destroys the moved-from source in
// place, so the registration runs with `he` live and the swap still has a write
// left to make into it.
TEST_F(ModuleOwnedHandlers, UnregisteringAByValueCapturedHandlerDoesNotWriteIntoFreedStorage)
{
    UIElement el;
    ByValueFuse fuse;
    int registered = 0;
    const auto token =
        el.RegisterEventHandler(kEventMouseUp, [b = ByValueBomb{&fuse}](UIEvent&) { (void)b; });
    // Armed only now, so the registration's own temporaries cannot fire it.
    fuse.El = &el;
    fuse.Id = kEventMouseUp;
    fuse.Count = 64;
    fuse.Registered = &registered;

    EXPECT_TRUE(el.UnregisterEventHandler(token));
    EXPECT_GT(registered, 0) << "the attack did not actually re-enter";
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(el, kEventMouseUp, token.Key))
        << "the unregistered entry kept its callable";

    fuse.El = nullptr; // disarm: nothing may detonate against a dying element
}

// SITE 2 — compaction. remove_if move-assigns the kept entry over the dropped
// one, and that assignment destroys the dropped callable in place, under the
// iterators remove_if is still walking.
//
// The surviving COUNT is deliberately not asserted: the compaction reads
// `vec.end()` twice in one statement and the order of those two evaluations
// against remove_if is unspecified, so whether the re-entrant registrations land
// inside or outside the erased range is not a property this test can pin.
TEST_F(ModuleOwnedHandlers, CompactingAByValueCapturedOnceHandlerDoesNotCorruptTheSweep)
{
    UIElement el;
    ByValueFuse fuse;
    int registered = 0;
    const auto token =
        el.RegisterEventHandlerOnce(kEventMouseUp, [b = ByValueBomb{&fuse}](UIEvent&) { (void)b; });
    // A second, surviving entry: without one there is nothing for remove_if to
    // move over the dropped entry, and the destruction never happens mid-sweep.
    el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    fuse.El = &el;
    fuse.Id = kEventMouseUp;
    fuse.Count = 64;
    fuse.Registered = &registered;

    Send(el, kEventMouseUp);

    EXPECT_GT(registered, 0) << "the attack did not actually re-enter";
    EXPECT_FALSE(UIEventHandlerAccess::HandlerHasCallable(el, kEventMouseUp, token.Key))
        << "the fired `once` entry kept its callable through compaction";
    // The table is still walkable afterwards.
    Send(el, kEventMouseUp);

    fuse.El = nullptr;
}

// SITE 3 — invocation. DispatchEvent calls the table's OWN entry, so an inline
// closure executes out of the table's storage; registering from inside it must
// not move that storage out from under the frame that is running.
//
// Not module-specific: any handler with a by-value capture that wires up another
// handler for the same event on the same element is this shape.
TEST_F(ModuleOwnedHandlers, AHandlerThatRegistersDuringItsOwnDispatchStillReadsItsCaptures)
{
    constexpr int kMagic = 0x5A5A5A5A;
    UIElement el;
    int observed = 0;

    el.RegisterEventHandler(kEventMouseUp,
                            [cap = SelfReadCapture{kMagic}, &el, &observed](UIEvent&) {
                                for (int i = 0; i < 64; ++i)
                                    el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
                                observed = cap.Magic;
                            });

    Send(el, kEventMouseUp);

    EXPECT_EQ(observed, kMagic)
        << "the executing closure's own storage moved while it was running";
}

// SF-4's shape: a revocation runs mid-dispatch — clicking Stop unloads a module
// from inside a UI dispatch — and this pins WHAT the dispatch is still holding
// when it does.
//
// The answer has to be "nothing but the executing entry", because that entry is
// the only one the quiesce ledger can pin. A dispatch that kept its own copies of
// the sibling callables would keep them past the revocation, and the ledger does
// not count copies: the unmap would be allowed and those copies destroyed against
// freed code. The walk therefore visits the table in place, so a revoked sibling
// is released INSIDE the revocation, while the image is certainly still mapped.
TEST_F(ModuleOwnedHandlers, RevocationMidDispatchReleasesSiblingsAndPinsOnlyTheExecutingOne)
{
    PretendModuleImage image;

    UIElement el;
    auto shared = std::make_shared<int>(7);
    std::size_t pinsDuringCall = 0;
    long sharedUseCountAfterRevoke = 0;
    int laterSiblingCalls = 0;

    // Runs BEFORE the revoker, so its callable is the one a dispatch-held copy would
    // still be carrying at the moment of the unmap.
    el.RegisterEventHandler(kEventMouseUp, [shared](UIEvent&) { (void)*shared; });
    el.RegisterEventHandler(kEventMouseUp, [&](UIEvent&) {
        image.Revoke();
        pinsDuringCall = image.Pins();
        sharedUseCountAfterRevoke = shared.use_count();
    });
    // Revoked before the walk reaches it, so it must not run.
    el.RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++laterSiblingCalls; });

    Send(el, kEventMouseUp);

    EXPECT_EQ(sharedUseCountAfterRevoke, 1)
        << "something still held the revoked sibling's callable when the revocation returned; "
           "the ledger does not count that holder, so the unmap would proceed and the callable "
           "would be destroyed against freed code";
    EXPECT_EQ(pinsDuringCall, 1u)
        << "the executing handler must be the one and only thing pinning the image here — a "
           "count of 0 means the frame's own callable could be unmapped under it, and a higher "
           "one means something else is still held";
    EXPECT_EQ(laterSiblingCalls, 0)
        << "a handler revoked earlier in this same dispatch still ran; the walk must re-read "
           "each entry's `active` when it reaches it";
}

// ---------------------------------------------------------------------------
// WHEN a released callable's destructor runs, not just whether it does.
//
// The pass that releases it is standing on things that destructor can take away:
// the other slots of the same control, the control itself, the element whose
// table is being walked. Each of these pins the timing.
// ---------------------------------------------------------------------------

namespace
{
// Clears the OTHER slot from a slot callable's destructor. Held through a
// shared_ptr, so the copy the sink takes on the way out is an owner rather than a
// second bomb, and the payload detonates exactly once — when the last owner dies.
struct SlotClearingBomb
{
    TreeView* Tree = nullptr;
    ~SlotClearingBomb()
    {
        if (Tree)
            Tree->SetOnItemActivated(nullptr);
    }
};

// Destroys the element from a released callable's destructor. A module closure
// holding the last owning reference to an element is an ordinary shape, and
// ModuleOwnedHandlers' own two-phase design names element destruction as a thing
// a released closure does.
template <typename T>
struct OwnerDroppingBomb
{
    std::unique_ptr<T>* Owner = nullptr;
    ~OwnerDroppingBomb()
    {
        if (Owner)
            Owner->reset();
    }
};
} // namespace

// The revocation pass releases two slots of one control. Released in place, the
// first callable's destructor unstamps the second before the walk reaches it, and
// the unload log then reports one revocation where two happened. Parked in the
// pass's sink, it runs after the walk and both slots are still stamped when they
// are visited.
TEST_F(ModuleOwnedHandlers, ReleasingASlotDoesNotLetItsDestructorUnstampTheNextSlotFirst)
{
    PretendModuleImage image;

    TreeView tv;
    auto bomb = std::make_shared<SlotClearingBomb>();
    tv.SetOnSelectionChanged([bomb](TreeId) { (void)bomb; });
    tv.SetOnItemActivated([](TreeId) {});
    ASSERT_EQ(image.Pins(), 2u);

    // Armed only now, so the setter's own temporaries cannot fire it.
    bomb->Tree = &tv;
    bomb.reset();

    EXPECT_EQ(image.Revoke(), 2u)
        << "the first slot's callable was destroyed inside the walk and cleared the second "
           "before the walk got to it";
    EXPECT_FALSE(UIEventHandlerAccess::SelectionChangedHasCallable(tv));
    EXPECT_FALSE(UIEventHandlerAccess::ItemActivatedHasCallable(tv));
    EXPECT_EQ(image.Pins(), 0u);
}

// Rebinding a slot whose old callable owns the control. The slot and the
// element's stamp count are settled BEFORE the old callable dies, so the
// destruction finds nothing left to write to. Assigning over it destroys the old
// callable first and then writes the stamp and dereferences the owning element —
// both into freed memory.
//
// SILENT without a checking allocator. ASan fails it at the write.
TEST_F(ModuleOwnedHandlers, RebindingASlotWhoseOldCallableOwnsTheControlWritesNothingAfterwards)
{
    PretendModuleImage image;

    auto owner = std::make_unique<Button>();
    auto bomb = std::make_shared<OwnerDroppingBomb<Button>>();
    owner->SetOnMouseDown([bomb](Button&) { (void)bomb; });
    bomb->Owner = &owner;
    bomb.reset();

    Button* raw = owner.get();
    raw->SetOnMouseDown([](Button&) {});

    EXPECT_EQ(owner, nullptr) << "the attack did not actually destroy the control";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// A batch releases several of one element's handlers, and the first release
// destroys that element. The sink is the whole function's, so nothing is dropped
// until the walk and every later use of `this` are done. A per-entry sink drops it
// between entries, and the next iteration then reads the table of a dead element —
// which the per-element guard in RevokeHandlersOwnedByImage cannot catch, because
// it tests the index before the batch, not inside it.
//
// Both handlers sit on ONE event id so their order in the batch is registration
// order rather than whatever the map yields: the bomb must be first.
//
// SILENT without a checking allocator. ASan fails it at the read.
TEST_F(ModuleOwnedHandlers, ABatchWhoseFirstReleaseDestroysTheElementDoesNotReadItAgain)
{
    PretendModuleImage image;

    auto owner = std::make_unique<UIElement>();
    auto bomb = std::make_shared<OwnerDroppingBomb<UIElement>>();
    owner->RegisterEventHandler(kEventMouseUp, [bomb](UIEvent&) { (void)bomb; });
    owner->RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    ASSERT_EQ(image.Pins(), 2u);

    bomb->Owner = &owner;
    bomb.reset();

    EXPECT_EQ(image.Revoke(), 2u) << "the batch did not release both handlers";
    EXPECT_EQ(owner, nullptr) << "the attack did not actually destroy the element";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// ---------------------------------------------------------------------------
// The attribution window: registrations made before the range is knowable.
// ---------------------------------------------------------------------------

TEST_F(ModuleOwnedHandlers, RegistrationsInsideTheMapWindowAreAttributedOnClose)
{
    const Range self = ThisBinaryRange();

    UI::OpenImageAttribution();
    UIElement el;
    const auto token = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    // Provisionally stamped: the range does not exist yet, so the alternative is
    // to lose it forever.
    EXPECT_TRUE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, token.Key));
    UI::CloseImageAttribution(self.Base, self.Size);

    EXPECT_TRUE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, token.Key))
        << "a registration made during the map window lost its stamp once the range arrived";
    EXPECT_EQ(UI::CountHandlersOwnedByImage(self.Base, self.Size), 1u);

    UI::RevokeHandlersOwnedByImage(self.Base, self.Size);
    UI::RetractHotSwappableImage(self.Base);
    // (these two tests open the window explicitly, so they close it explicitly)
}

TEST_F(ModuleOwnedHandlers, EngineHandlersRegisteredInsideTheWindowLoseTheProvisionalStamp)
{
    const Range self = ThisBinaryRange();

    UI::OpenImageAttribution();
    AxisHeaderBar bar; // Engine.dll's handlers, registered inside the window
    UIElement mine;
    const auto token = mine.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    UI::CloseImageAttribution(self.Base, self.Size);

    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(bar), 0u)
        << "an engine handler registered during the map window kept a provisional stamp — "
           "the window must resolve against the real range, not trust it";
    EXPECT_TRUE(UIEventHandlerAccess::IsStamped(mine, kEventMouseUp, token.Key));

    UI::RevokeHandlersOwnedByImage(self.Base, self.Size);
    UI::RetractHotSwappableImage(self.Base);
    // (these two tests open the window explicitly, so they close it explicitly)
}

TEST_F(ModuleOwnedHandlers, AFailedMapDropsEveryProvisionalStamp)
{
    UI::OpenImageAttribution();
    UIElement el;
    const auto token = el.RegisterEventHandler(kEventMouseUp, [](UIEvent&) {});
    UI::CloseImageAttribution(0, 0); // LoadLibrary failed

    EXPECT_FALSE(UIEventHandlerAccess::IsStamped(el, kEventMouseUp, token.Key))
        << "a failed map left stamps pointing at an image that never existed";
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

// ---------------------------------------------------------------------------
// Button::SetOnClick — the convenience wrapper is covered by this machinery for
// free, and these pin the two things that make that true rather than assumed:
// the callable reaches the table UNWRAPPED (so it stamps to the caller's image,
// not to Engine.dll), and the token the wrapper keeps is harmless once the sweep
// has emptied the entry it names.
// ---------------------------------------------------------------------------

// The load-bearing one. SetOnClick hands the callable straight to
// RegisterEventHandler, so the stamp resolves through the CALLER's target_type.
// Wrapping it in a lambda of Button.cpp's own — the nesting AdoptHandlerOwnerFrom
// exists to undo — would attribute a user module's callback to Engine.dll and strand
// it at unmap. This goes red the day someone adds that wrapper.
TEST_F(ModuleOwnedHandlers, SetOnClickStampsToTheCallersImageNotToEngine)
{
    PretendModuleImage image;
    Button btn;

    int clicks = 0;
    btn.SetOnClick([&clicks](UIEvent&) { ++clicks; });

    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 1u)
        << "SetOnClick's callable did not attribute to this binary — it reached the table "
           "wrapped in an engine-owned callable, which hides the real owner from revocation";
    EXPECT_EQ(image.Pins(), 1u);
    EXPECT_EQ(UI::IndexedElementCount(), 1u) << "the button never entered the revocation index";

    EXPECT_EQ(image.Revoke(), 1u) << "the sweep did not find SetOnClick's subscription";
    btn.TriggerClick();
    EXPECT_EQ(clicks, 0) << "a revoked convenience callback still fired";
    EXPECT_EQ(image.Pins(), 0u);
}

// Stale token, first half: SetOnClick again while the swept entry is STILL in the
// table (nothing has dispatched, so no drain has run). The wrapper re-presents a
// token naming an emptied entry — it matches, releases an already-empty callable and
// clears an already-clear stamp, both no-ops. What would break here is a stamp
// refcount that double-decremented: the index would go wrong, and TearDown's leak
// check plus OwnedHandlerCount below are what see it.
TEST_F(ModuleOwnedHandlers, SetOnClickAfterASweepIsHarmlessWhileTheSweptEntryRemains)
{
    PretendModuleImage image;
    Button btn;

    int first = 0, second = 0;
    btn.SetOnClick([&first](UIEvent&) { ++first; });
    ASSERT_EQ(image.Revoke(), 1u);
    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 1u)
        << "fixture precondition: the swept entry is still present, just emptied";
    ASSERT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 0u);

    btn.SetOnClick([&second](UIEvent&) { ++second; });
    EXPECT_EQ(UIEventHandlerAccess::OwnedHandlerCount(btn), 1u)
        << "the stamp count is wrong after re-setting over a swept entry";

    btn.TriggerClick();
    EXPECT_EQ(first, 0) << "the swept callback fired";
    EXPECT_EQ(second, 1) << "the replacement did not fire exactly once";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 1u)
        << "the swept entry was not drained, or the replacement leaked one";
}

// Stale token, second half: a dispatch's drain has already erased the swept entry, so
// the token now names an entry that does not exist. UnregisterEventHandler must simply
// miss. It cannot alias a stranger's entry, because keys come from a per-element
// monotonic counter that is never reset or reused (UIElement.h, m_NextHandlerKey) —
// this is the arm that would catch a change making keys recyclable.
TEST_F(ModuleOwnedHandlers, SetOnClickAfterASweepIsHarmlessOnceTheSweptEntryIsGone)
{
    PretendModuleImage image;
    Button btn;

    int first = 0, second = 0, direct = 0;
    btn.SetOnClick([&first](UIEvent&) { ++first; });
    ASSERT_EQ(image.Revoke(), 1u);

    // The dispatch tail drains the emptied entry out of the table.
    btn.TriggerClick();
    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 0u)
        << "fixture precondition: the swept entry must have been drained";

    // A fresh subscriber takes the next key. If the stale token could ever alias a
    // reused key, this is what it would destroy.
    btn.RegisterEventHandler(kEventButtonClick, [&direct](UIEvent&) { ++direct; });
    btn.SetOnClick([&second](UIEvent&) { ++second; });

    btn.TriggerClick();
    EXPECT_EQ(first, 0);
    EXPECT_EQ(direct, 1) << "re-presenting the stale token unregistered a stranger's handler";
    EXPECT_EQ(second, 1);

    // And clearing through a stale token is equally inert.
    btn.SetOnClick(nullptr);
    btn.TriggerClick();
    EXPECT_EQ(direct, 2) << "clearing the convenience slot took the direct subscriber with it";
    EXPECT_EQ(second, 1);
}

// The protocol is main-thread-only and the entry points that write the
// process-wide tables assert it. Without the assert an off-thread registration
// during a module map corrupts the image list or the element index silently,
// far from the thread that caused it.
TEST(ModuleOwnedHandlersThreadGuard, AWriteFromAnotherThreadAsserts)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the owner-thread assert compiles out with NDEBUG";
#elif !GTEST_HAS_DEATH_TEST
    GTEST_SKIP() << "death tests not supported on this platform";
#else
    // Anchor the owner thread on this one; the guard captures on first write.
    UI::OpenImageAttribution();
    UI::CloseImageAttribution(0, 0);

    ASSERT_DEATH(
        {
            std::thread worker([] { UI::OpenImageAttribution(); });
            worker.join();
        },
        "main-thread-only");
#endif
}

} // namespace
