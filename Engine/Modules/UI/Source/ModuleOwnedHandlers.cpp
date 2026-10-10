#include "UI/ModuleOwnedHandlers.h"

#include "UI/UIElement.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>
#include <thread>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace UI
{
namespace
{

struct ImageRange
{
    std::uint64_t Base = 0;
    std::uint64_t Size = 0;
    bool Contains(std::uint64_t addr) const { return addr >= Base && addr < Base + Size; }
};

// One process-wide state, living in Engine.dll. A function-local static is the
// established shape for engine-wide UI registries here (UIManager's alive-manager
// registry is the same), and it is what keeps the header-inline registration path
// in every consuming image talking to ONE table rather than a per-image clone.
struct OwnedHandlerState
{
    std::vector<ImageRange> Images;
    std::unordered_set<UIElement*> Elements;
    // Depth, not a bool: nested module maps are not expected, but a counter
    // cannot be left stuck open by an unbalanced pair the way a bool can.
    int AttributionDepth = 0;
#ifndef NDEBUG
    std::thread::id OwnerThread{};
#endif
};

OwnedHandlerState& State()
{
    static OwnedHandlerState s;
    return s;
}

// The main-thread-only rule the header states, made to fail loudly instead of
// silently: a registration racing a module map corrupts the process-wide image
// list and element index, and container corruption surfaces nowhere near the
// thread that caused it. Every entry point below that writes either table calls
// this first.
//
// Capture-on-first-use rather than a declared id, because nothing hands this
// module a main-thread id and the first write is whichever of a UI registration
// or a module image map happens first — the same shape as
// UIManager::AssertUiThread and UITextureRegistry's owner-thread assert.
void AssertOwnerThread()
{
#ifndef NDEBUG
    auto& s = State();
    if (s.OwnerThread == std::thread::id{})
        s.OwnerThread = std::this_thread::get_id();
    assert(std::this_thread::get_id() == s.OwnerThread &&
           "ModuleOwnedHandlers is main-thread-only; an off-thread call races the image list and "
           "the element index");
#endif
}

bool InsideAnyMappedImage(const OwnedHandlerState& s, std::uint64_t addr)
{
    for (const auto& r : s.Images)
        if (r.Contains(addr))
            return true;
    return false;
}

bool AddrIsMapped(std::uint64_t addr)
{
    return InsideAnyMappedImage(State(), addr);
}

// Elements are removed from the index as their last stamp clears, so iterating
// the live set while revoking would invalidate under us. Snapshot first.
//
// The snapshot holds RAW pointers, so nothing in a walk over it may run module
// code: a revoked closure's captures can own UI elements, and destroying one
// mid-walk dangles every entry the snapshot has not reached yet. That is why
// revocation is split into a decide pass and a release pass — see
// RevokeHandlersOwnedByImage.
std::vector<UIElement*> SnapshotIndex(const OwnedHandlerState& s)
{
    return std::vector<UIElement*>(s.Elements.begin(), s.Elements.end());
}

} // namespace

bool AddressIsInsideMappedImage(std::uint64_t addr)
{
    return AddrIsMapped(addr);
}

void OpenImageAttribution()
{
    AssertOwnerThread();
    ++State().AttributionDepth;
}

void CloseImageAttribution(std::uint64_t base, std::uint64_t size)
{
    AssertOwnerThread();
    auto& s = State();
    if (s.AttributionDepth > 0)
        --s.AttributionDepth;

    if (base != 0 && size != 0)
    {
        const bool known = std::any_of(s.Images.begin(), s.Images.end(),
                                       [base](const ImageRange& r) { return r.Base == base; });
        if (!known)
            s.Images.push_back(ImageRange{base, size});
    }

    // Provisional stamps taken during the window are resolved here — the window
    // exists precisely because the range was not knowable while they were taken.
    // Anything outside every mapped image is engine-owned and loses its stamp.
    if (s.AttributionDepth != 0)
        return;
    for (UIElement* el : SnapshotIndex(s))
        el->DropStampsOutsideMappedImages();
}

void RetractHotSwappableImage(std::uint64_t base)
{
    AssertOwnerThread();
    auto& s = State();
    s.Images.erase(std::remove_if(s.Images.begin(), s.Images.end(),
                                  [base](const ImageRange& r) { return r.Base == base; }),
                   s.Images.end());
}

std::size_t CountHandlersOwnedByImage(std::uint64_t base, std::uint64_t size)
{
    auto& s = State();
    std::size_t n = 0;
    for (UIElement* el : SnapshotIndex(s))
        n += el->CountHandlersOwnedByImage(base, size);
    return n;
}

std::size_t RevokeHandlersOwnedByImage(std::uint64_t base, std::uint64_t size)
{
    AssertOwnerThread();
    auto& s = State();
    std::size_t revoked = 0;
    // TWO PHASES, because releasing a callable runs the MODULE'S code.
    //
    // The closure's captured destructors can do anything the module could do at
    // any other moment: register another handler (appending to the very deque a
    // walk holds a reference into — stable for existing entries, not for
    // iterators — and inserting new event ids into the very map
    // it is iterating), unregister one, or destroy UI elements the element-index
    // snapshot still points at.
    //
    // Deferring the release by moving each callable into a sink would also work
    // for the re-entrancy above, but not for the element-death case: a released
    // closure can destroy UI elements, and the element-index snapshot holds raw
    // pointers. Splitting the pass instead covers both, and it keeps the rule
    // simple enough to hold onto — NO module code runs until every table has been
    // read.
    //
    // Phase 1 decides. Setting a flag runs no user code, so nothing can change
    // underneath it.
    std::vector<UIElement::RevocationBatch> batches;
    for (UIElement* el : SnapshotIndex(s))
        el->CollectHandlersOwnedByImage(base, size, batches);

    // Phase 2 releases, holding no iterator and no reference across a release.
    // Entries registered by those destructors are not in `batches`, so they are
    // not revoked — they belong to whatever registered them, not to this unmap.
    for (auto& batch : batches)
    {
        // An earlier release may have destroyed this element; ~UIElement leaves
        // the index, which is the signal. Checked per element rather than per
        // entry because releasing an element's own last stamp also de-indexes it.
        if (s.Elements.find(batch.Element) == s.Elements.end())
            continue;
        revoked += batch.Element->ReleaseCollectedHandlers(batch);
    }
    if (revoked != 0)
    {
        Logger::Log::Info("[UI] revoked {} event handler(s) owned by the module image at 0x{:x} "
                          "before its unmap",
                          revoked, base);
    }
    return revoked;
}

std::uint64_t AttributeCallableOwner(const std::type_info& ti)
{
    // Reads the same process-wide image list every writer here asserts on. A
    // registration racing a module load would read it mid-append, so the rule is
    // the same for readers as for writers; the check compiles out with NDEBUG.
    AssertOwnerThread();
    const auto& s = State();
    // Fast path for every host that never maps a hot-swappable image, and for
    // the editor's entire chrome-registration burst before a project's module
    // loads: nothing to test against, so nothing is stamped or indexed.
    if (s.Images.empty() && s.AttributionDepth == 0)
        return 0;

    const auto addr = reinterpret_cast<std::uint64_t>(&ti);
    if (InsideAnyMappedImage(s, addr))
        return addr;
    // Inside the map window the owning range does not exist yet; take the stamp
    // provisionally and let CloseImageAttribution keep or drop it.
    return s.AttributionDepth > 0 ? addr : 0;
}

void NoteElementOwnsModuleHandler(UIElement* el)
{
    AssertOwnerThread();
    State().Elements.insert(el);
}

void ForgetElementOwnsModuleHandler(UIElement* el)
{
    AssertOwnerThread();
    State().Elements.erase(el);
}

std::size_t IndexedElementCount()
{
    return State().Elements.size();
}

} // namespace UI

// ---------------------------------------------------------------------------
// UIElement's side of the protocol. Defined here rather than in UIElement.cpp:
// this file owns unload-time handler ownership, and these three are only ever
// called from the entry points above.
// ---------------------------------------------------------------------------

std::size_t UIElement::CountHandlersOwnedByImage(std::uint64_t base, std::uint64_t size) const
{
    if (m_OwnedHandlerCount == 0)
        return 0;
    std::size_t n = 0;
    for (const auto& kv : m_EventHandlers)
        for (const auto& he : kv.second)
            if (he.ownerCodeAddr >= base && he.ownerCodeAddr < base + size)
                ++n;
    return n + CountMemberSlotsOwnedByImage(base, size);
}

void UIElement::CollectHandlersOwnedByImage(std::uint64_t base, std::uint64_t size,
                                            std::vector<RevocationBatch>& batches)
{
    if (m_OwnedHandlerCount == 0)
        return;
    RevocationBatch batch;
    batch.Element = this;
    for (auto& kv : m_EventHandlers)
    {
        for (auto& he : kv.second)
        {
            if (he.ownerCodeAddr < base || he.ownerCodeAddr >= base + size)
                continue;
            // Deactivate first, including for an executing handler: it runs no
            // user code, it stops the handler firing again, and it is what makes
            // the sweep at the end of the OUTERMOST dispatch on this element erase
            // the entry on the way out. A nested dispatch deliberately does not
            // sweep, so an executing entry is never erased under its own frame.
            DeactivateHandler(he);
            // Same guard the explicit unregister carries: releasing the
            // std::function under its own frame frees the captures it is running
            // out of. An executing handler keeps its callable AND its stamp, so
            // the ledger reports the image as pinned rather than letting it be
            // unmapped while a live frame is still inside it.
            if (IsHandlerExecuting(he.key))
                continue;
            batch.Entries.push_back({kv.first, he.key});
        }
    }
    CollectMemberSlotsOwnedByImage(base, size, batch.MemberSlotMask);
    if (!batch.Entries.empty() || batch.MemberSlotMask != 0)
        batches.push_back(std::move(batch));
}

std::size_t UIElement::ReleaseCollectedHandlers(const RevocationBatch& batch)
{
    // ONE sink for the whole function, not one per entry. A callable dropped
    // between entries runs the module's code while this walk is still running and
    // while the tail below still uses `this` — and that code can destroy this
    // element. RevokeHandlersOwnedByImage's per-element guard cannot catch that:
    // it tests the index BEFORE a batch, so a death caused INSIDE the batch is
    // invisible to it. Reserved so the collection itself never reallocates and
    // moves what it already holds.
    std::vector<EventHandler> doomed;
    doomed.reserve(batch.Entries.size());
    std::size_t revoked = 0;
    for (const auto& entry : batch.Entries)
    {
        // Re-found from scratch every time: an earlier release may have run module
        // code that appended to this deque or rehashed this map.
        auto it = m_EventHandlers.find(entry.Id);
        if (it == m_EventHandlers.end())
            continue;
        auto he = std::find_if(it->second.begin(), it->second.end(),
                               [&entry](const HandlerEntry& e) { return e.key == entry.Key; });
        if (he == it->second.end() || !he->handler)
            continue;
        // Stamp first, then move the callable out. `he` is dead from the swap on.
        ClearOwnerStamp(*he);
        doomed.emplace_back();
        doomed.back().swap(he->handler);
        ++revoked;
    }
    return revoked + ReleaseCollectedMemberSlots(batch.MemberSlotMask);
}

std::size_t UIElement::DropStampsOutsideMappedImages()
{
    if (m_OwnedHandlerCount == 0)
        return 0;
    std::size_t dropped = 0;
    for (auto& kv : m_EventHandlers)
    {
        for (auto& he : kv.second)
        {
            if (he.ownerCodeAddr == 0 || UI::AddrIsMapped(he.ownerCodeAddr))
                continue;
            ClearOwnerStamp(he);
            ++dropped;
        }
    }
    return dropped + DropMemberSlotStampsOutsideMappedImages();
}

} // namespace GameEngine
