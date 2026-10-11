#pragma once

#include "Types/Types.h"

#include <functional>
#include <memory>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine {

class AssetDecodeGate;

/**
 * @brief The hand-off of one texture level's block-row bands onto the pool
 * workers the asset decode gate leaves to texture work.
 *
 * A texture cook encodes each level as independent block-row bands
 * (CompressTextureCookLevel). Given a TextureCookWorkers, the cooking thread
 * keeps encoding bands itself and also hands them to helper jobs on the pool,
 * so one large texture spreads across workers instead of running on one core.
 * Each band writes its own slice of the level's payload, so the bytes are the
 * one-thread encode's whatever the split.
 *
 * Budget: a helper encodes a band only while it holds an AssetDecodeGate slot
 * for texture work, the same slots texture decodes take, so the pool workers
 * busy with texture work (texture decodes and helper bands together) never pass
 * the gate's texture share, and all asset work never passes the gate. The
 * calling thread is not counted here: on the pool it is a texture decode, which
 * holds its own gate slot for the thread it runs on; off the pool (the build's
 * bake thread) it takes no pool worker. A caller therefore never waits for a
 * slot, and every run finishes even when helpers hold the whole share.
 *
 * Helpers run on the Background lane, so frame work (Normal priority) is always
 * dequeued first. A helper holds its slot for one band only and then goes back
 * to the end of the Background lane, so the decodes queued there, and the
 * decode readers waiting on the gate, are reached between bands.
 *
 * Thread-safe. A helper still queued when RunBands returns ends without
 * touching this object, the gate or the run's callbacks, so this object may
 * be destroyed while such helpers are queued; the pool must outlive them.
 */
class TextureCookWorkers
{
  public:
    TextureCookWorkers(JobSystem::WorkStealingThreadPool& pool, std::shared_ptr<AssetDecodeGate> gate);
    ~TextureCookWorkers();

    TextureCookWorkers(const TextureCookWorkers&) = delete;
    TextureCookWorkers& operator=(const TextureCookWorkers&) = delete;

    /**
     * @brief Run `encodeBand(i)` once for every band i in [0, bandCount), on the
     * calling thread and on helpers holding a gate slot; returns once every band
     * that started has finished.
     *
     * `encodeBand` runs on several threads at once and must touch only band i's
     * own output. `stopRequested` is polled by the calling thread only, before
     * each band it takes. Returns false when a band returned false or when
     * `stopRequested` returned true; no further band starts after either. A band
     * that throws, on any thread, stops the run the same way, and RunBands
     * rethrows the first such exception on the calling thread once the bands in
     * progress have finished, as a one-thread encode would have thrown it. A throw
     * from `stopRequested` stops the run too and leaves RunBands once the bands
     * in progress have finished. A pool in inline mode encodes every band on the
     * calling thread.
     */
    bool RunBands(uint32 bandCount, std::function<bool(uint32)> encodeBand,
                  const std::function<bool()>& stopRequested);

  private:
    JobSystem::WorkStealingThreadPool& m_Pool;
    std::shared_ptr<AssetDecodeGate> m_Gate;
};

} // namespace GameEngine
