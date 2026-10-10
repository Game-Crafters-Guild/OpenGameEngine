#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <functional>
#include <mutex>

namespace GameEngine {

/**
 * @brief The JobSystem workers that asset work may occupy at once: the one count
 * that asset decode jobs (AssetIOService) and texture-cook helper bands
 * (TextureCookWorkers) both take their slot from.
 *
 * The frame's ECS waves run on the same pool, and a running decode or band cannot
 * be preempted, so the gate bounds what runs rather than what is queued. Every
 * slot is one pool worker busy with asset work:
 *  - at most MaxWorkers() slots are taken: all but two of the pool's workers, which
 *    keeps two free for the frame, and one slot for a pool of two workers or fewer,
 *    which keeps fewer than two free. A GE_ASSET_DECODE_MAX override replaces the
 *    count as given, so one above the pool's workers minus two keeps fewer free;
 *  - texture work (a texture decode, which runs the import cook for minutes on a
 *    cook-cache miss, or one helper band of a cook) takes at most
 *    MaxTextureWorkers() of them, all but two of the gate's slots, so the decodes
 *    the frame waits on (a material during scene resolve) always find a slot no
 *    cook holds. A 16-worker pool gives 14 slots, 12 of them for texture work.
 *
 * A gate of one slot (a pool of three workers or fewer, or GE_ASSET_DECODE_MAX=1)
 * cannot keep both promises: its slot is shared, and a short decode can wait
 * behind a texture decode there. A gate of two or three slots leaves one slot to
 * texture work and the rest to the other decodes.
 *
 * Thread-safe. Held by shared_ptr: a decode job or a helper releases its slot when
 * it ends, possibly after the service that made the gate has stopped.
 */
class AssetDecodeGate
{
  public:
    enum class Work : uint8
    {
        Other,   ///< any decode but a texture's
        Texture, ///< a texture decode, or one helper band of a texture cook
    };

    /**
     * @param poolWorkers The pool's worker count.
     * @param maxWorkersOverride Replaces MaxWorkers() when non-zero (the
     *        GE_ASSET_DECODE_MAX diagnostic); MaxTextureWorkers() follows it.
     * @param slotReleased Called after every release, outside the gate's lock: the
     *        decode readers wait on it to claim the slot.
     */
    AssetDecodeGate(size_t poolWorkers, size_t maxWorkersOverride, std::function<void()> slotReleased);

    AssetDecodeGate(const AssetDecodeGate&) = delete;
    AssetDecodeGate& operator=(const AssetDecodeGate&) = delete;

    size_t MaxWorkers() const { return m_MaxWorkers; }
    size_t MaxTextureWorkers() const { return m_MaxTextureWorkers; }

    /** @brief Take a slot for `work` when one is free; never blocks. */
    bool TryAcquire(Work work);

    /** @brief Give back a slot TryAcquire took for the same `work`. */
    void Release(Work work);

    /** @brief The slots texture work could take now: a snapshot, stale once returned. */
    size_t FreeTextureSlots() const;

  private:
    const size_t m_MaxWorkers;
    const size_t m_MaxTextureWorkers;
    const std::function<void()> m_SlotReleased;

    mutable std::mutex m_Mutex;
    size_t m_Taken = 0;        // guarded by m_Mutex
    size_t m_TextureTaken = 0; // guarded by m_Mutex: of m_Taken, texture work
};

} // namespace GameEngine
