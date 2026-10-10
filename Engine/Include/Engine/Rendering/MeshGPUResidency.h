#pragma once

// MeshGPUResidency: answers "are this mesh upload's bytes readable by the GPU
// yet?" for one MeshGPURegistry.
//
// An upload is identified by a monotonic `uploadSeq` stamp minted by
// BeginUpload() and carried on the MeshGPUEntry. The stamp indexes a
// fixed-capacity ring of counters; a counter holds the number of outstanding
// obligations that must complete before every byte the upload owns is readable
// by the GPU. The upload bracket is the only obligation there is today, so a
// counter holds 0 or 1 and a stamp reads NON-resident from BeginUpload() until
// EndUpload() -- which is what closes the window where a mesh has an entry but
// no bytes. Live occupancy is therefore at most one slot: no reading rule
// depends on that, but every claim about window pressure does.
//
// The ring is fixed-capacity because the predicate is read from job workers
// while a writer mints new stamps: a container that reallocates (vector) or
// mutates a block map (deque) is a data race on the container itself, whatever
// the elements are. A ring that never moves makes the read a plain indexed
// atomic load.
//
// Every reading rule fails CLOSED: the never-uploaded sentinel 0, a stamp
// outside the window, and any stamp with an outstanding obligation all answer
// "not resident". A caller that cannot tell does not draw.
//
// Thread safety:
//   - BeginUpload / EndUpload / CancelRecordsFor / ResetAfterDeviceRebuild are
//     WRITER operations and must not overlap each other. They inherit
//     MeshGPURegistry's registration affinity -- which thread that is may vary
//     between calls, so what must hold is mutual exclusion, not affinity to one
//     thread. Overlap is caught by an assert in developer builds.
//   - IsResident is safe from any thread and is read from job workers during
//     parallel draw record. That covers this class's state only: the stamp it
//     is handed lives on MeshGPUEntry as a plain uint64_t, so it inherits the
//     registry's existing single-writer discipline exactly as bucketKey and the
//     pool indices beside it do.

#include <atomic>
#include <cstdint>
#include <memory>

namespace GameEngine
{
namespace Rendering
{

class MeshGPUResidency
{
  public:
    // Ring capacity in stamps, chosen for the widest live window this class is
    // meant to serve -- not measured, and far wider than the at-most-one-open
    // upload the current bracket produces. The sizing is an assumption either
    // way, so exhaustion is not assumed away: it has a fail-closed arm (see
    // BeginUpload) and a counter that must read zero.
    static constexpr uint32_t kDefaultPendingWindow = 1u << 18;

    // `pendingWindow` is rounded up to a power of two (the slot index is a mask,
    // not a modulo) and clamped to at least two slots.
    explicit MeshGPUResidency(uint32_t pendingWindow = kDefaultPendingWindow);

    MeshGPUResidency(const MeshGPUResidency&)            = delete;
    MeshGPUResidency& operator=(const MeshGPUResidency&) = delete;

    // Open a new upload and return its stamp. The stamp reads NON-resident
    // until the matching EndUpload().
    //
    // Returns 0 -- the never-uploaded sentinel, which reads non-resident -- when
    // the window is exhausted. The caller must treat that exactly as a failed
    // pool allocation: no bytes are written, the entry does not draw, and its
    // content hash is reset so a re-registration re-uploads rather than dedups
    // to a permanently non-resident entry. Blocking here instead would deadlock:
    // the wave that registers meshes is joined by the thread that would later
    // drain the window.
    [[nodiscard]] uint64_t BeginUpload();

    // Close the upload's own obligation. After this the stamp is resident iff
    // no other obligation is outstanding.
    void EndUpload(uint64_t uploadSeq);

    // Retire a stamp whose destination is being released, so the ring base can
    // move past it. Callers must ALSO clear the stamp they hold: a retired
    // stamp reads resident (it has no outstanding obligation), so an entry that
    // kept pointing at one would read drawable over released storage.
    // CancelRecordsFor(0) is a defined no-op -- 0 is a shared sentinel across
    // every never-uploaded and every released entry, not an upload identity.
    void CancelRecordsFor(uint64_t uploadSeq);

    // THE read-half predicate. Fails closed on every path it cannot answer.
    [[nodiscard]] bool IsResident(uint64_t uploadSeq) const;

    // A device rebuild freed the storage every outstanding stamp was written
    // into, so the whole table is DISCARDED rather than drained: every slot is
    // cleared and the base jumps past every stamp minted so far. Callers must
    // clear the stamps they hold in the same step -- otherwise a stale stamp
    // reads below-base, hence resident, against storage that no longer exists.
    void ResetAfterDeviceRebuild();

    // BeginUpload() calls refused for window exhaustion, cumulative for the
    // process. Must read 0; a non-zero value means the window constant is
    // wrong, not that a caller misbehaved. With the bracket as a stamp's only
    // obligation the refusal needs a window's worth of simultaneously-open
    // uploads and cannot be reached, so a zero read from a running engine is a
    // tautology rather than evidence -- the arm is held honest by the unit
    // tests that run a 4-slot window.
    [[nodiscard]] uint64_t WindowExhaustedCount() const
    {
        return m_WindowExhausted.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t PendingWindow() const { return m_Window; }

    // Oldest stamp not yet retired. Everything below it is resident.
    [[nodiscard]] uint64_t BaseSeq() const { return m_BaseSeq.load(std::memory_order_acquire); }

  private:
    [[nodiscard]] uint32_t SlotOf(uint64_t seq) const { return static_cast<uint32_t>(seq & m_Mask); }

    // Move the base forward over retired slots. The base may never pass a slot
    // whose counter is non-zero: that is what stops it overtaking an upload
    // which is open but unfinished, whose stamp the below-base rule would then
    // report resident with none of its bytes written.
    void AdvanceBase();

    // Detects overlapping writer operations in developer builds; the bodies are
    // empty when asserts are compiled out.
    class WriterScope
    {
      public:
        explicit WriterScope(const MeshGPUResidency& owner);
        ~WriterScope();

        WriterScope(const WriterScope&)            = delete;
        WriterScope& operator=(const WriterScope&) = delete;

      private:
        const MeshGPUResidency& m_Owner;
    };

    uint32_t m_Window = 0;
    uint64_t m_Mask   = 0;

    std::unique_ptr<std::atomic<uint32_t>[]> m_Pending;

    // Writer-only; readers never touch it.
    uint64_t m_NextSeq = 1;

    std::atomic<uint64_t> m_BaseSeq{1};
    std::atomic<uint64_t> m_WindowExhausted{0};
    mutable std::atomic<bool> m_WriterActive{false};
};

} // namespace Rendering
} // namespace GameEngine
