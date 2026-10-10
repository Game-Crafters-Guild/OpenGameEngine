#include "Engine/Rendering/MeshGPUResidency.h"

#include <cassert>

namespace GameEngine
{
namespace Rendering
{
namespace
{

// The slot index is `seq & mask`, so the capacity must be a power of two, and
// at least two slots so a base advance has somewhere to go.
uint32_t RoundUpToPowerOfTwo(uint32_t value)
{
    uint32_t rounded = 2u;
    while (rounded < value && rounded < (1u << 31))
        rounded <<= 1;
    return rounded;
}

} // namespace

MeshGPUResidency::WriterScope::WriterScope(const MeshGPUResidency& owner)
    : m_Owner(owner)
{
#if !defined(NDEBUG)
    const bool wasActive = m_Owner.m_WriterActive.exchange(true, std::memory_order_acq_rel);
    assert(!wasActive && "MeshGPUResidency writer operations must not overlap");
    (void)wasActive;
#endif
}

MeshGPUResidency::WriterScope::~WriterScope()
{
#if !defined(NDEBUG)
    m_Owner.m_WriterActive.store(false, std::memory_order_release);
#endif
}

MeshGPUResidency::MeshGPUResidency(uint32_t pendingWindow)
    : m_Window(RoundUpToPowerOfTwo(pendingWindow))
    , m_Mask(static_cast<uint64_t>(m_Window) - 1u)
    , m_Pending(std::make_unique<std::atomic<uint32_t>[]>(m_Window))
{
    for (uint32_t i = 0; i < m_Window; ++i)
        m_Pending[i].store(0u, std::memory_order_relaxed);
}

uint64_t MeshGPUResidency::BeginUpload()
{
    WriterScope writerScope(*this);

    const uint64_t base = m_BaseSeq.load(std::memory_order_relaxed);
    if (m_NextSeq - base >= m_Window)
    {
        // The slot this stamp would take is still owned by a live upload. Fail
        // closed rather than alias it: the caller degrades to the same
        // not-drawable state a failed pool allocation produces.
        m_WindowExhausted.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }

    const uint64_t seq = m_NextSeq;
    // Open the slot BEFORE the stamp is reachable. The writer is single, so
    // this store is ordered against every later read of m_NextSeq by
    // AdvanceBase, and against readers by the release store of m_BaseSeq.
    m_Pending[SlotOf(seq)].store(1u, std::memory_order_release);
    m_NextSeq = seq + 1u;
    return seq;
}

void MeshGPUResidency::EndUpload(uint64_t uploadSeq)
{
    if (uploadSeq == 0u)
        return;

    WriterScope writerScope(*this);

    const uint64_t base = m_BaseSeq.load(std::memory_order_relaxed);
    if (uploadSeq < base || uploadSeq >= m_NextSeq)
    {
        assert(false && "MeshGPUResidency::EndUpload on a stamp that is not open");
        return;
    }

    std::atomic<uint32_t>& slot = m_Pending[SlotOf(uploadSeq)];
    const uint32_t outstanding = slot.load(std::memory_order_relaxed);
    assert(outstanding >= 1u && "MeshGPUResidency::EndUpload on a stamp with no open obligation");
    // Never decrement below zero: an underflowed slot would read non-resident
    // forever AND pin the base, turning one mis-sequenced call into a
    // permanently stuck ring. Writers are mutually exclusive, so the
    // load/store pair is not a race.
    if (outstanding == 0u)
        return;
    slot.store(outstanding - 1u, std::memory_order_release);

    AdvanceBase();
}

void MeshGPUResidency::CancelRecordsFor(uint64_t uploadSeq)
{
    if (uploadSeq == 0u)
        return;

    WriterScope writerScope(*this);

    const uint64_t base = m_BaseSeq.load(std::memory_order_relaxed);
    if (uploadSeq < base || uploadSeq >= m_NextSeq)
        return; // Already retired, or never minted: nothing to drop.

    m_Pending[SlotOf(uploadSeq)].store(0u, std::memory_order_release);
    AdvanceBase();
}

bool MeshGPUResidency::IsResident(uint64_t uploadSeq) const
{
    if (uploadSeq == 0u)
        return false; // Never uploaded, released, or tombstoned.

    const uint64_t base = m_BaseSeq.load(std::memory_order_acquire);
    if (uploadSeq < base)
        return true; // Retired: the base only ever passes a slot with no obligation left.

    if (uploadSeq - base >= m_Window)
    {
        // Unreachable by construction -- BeginUpload refuses to mint past the
        // window -- so this is a broken invariant, not a state with a defined
        // answer. Assert, and still fail closed in builds without asserts
        // rather than index a slot that belongs to another upload.
        assert(false && "MeshGPUResidency: stamp outside the pending window");
        return false;
    }

    return m_Pending[SlotOf(uploadSeq)].load(std::memory_order_acquire) == 0u;
}

void MeshGPUResidency::ResetAfterDeviceRebuild()
{
    WriterScope writerScope(*this);

    for (uint32_t i = 0; i < m_Window; ++i)
        m_Pending[i].store(0u, std::memory_order_relaxed);

    // Jump past every stamp minted so far in one step: the storage those
    // obligations targeted no longer exists, so there is nothing to drain.
    m_BaseSeq.store(m_NextSeq, std::memory_order_release);
}

void MeshGPUResidency::AdvanceBase()
{
    uint64_t base = m_BaseSeq.load(std::memory_order_relaxed);
    while (base < m_NextSeq && m_Pending[SlotOf(base)].load(std::memory_order_acquire) == 0u)
        ++base;
    m_BaseSeq.store(base, std::memory_order_release);
}

} // namespace Rendering
} // namespace GameEngine
