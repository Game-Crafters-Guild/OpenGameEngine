#include "Assets/TextureCookWorkers.h"

#include "Assets/AssetDecodeGate.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>

namespace GameEngine {

namespace
{
// One RunBands call: the bands still to hand out, and the ones in progress.
struct BandRun
{
    std::function<bool(uint32)> EncodeBand;
    uint32 BandCount = 0;

    std::mutex Mutex;
    std::condition_variable Settled; // the last band in progress finished
    uint32 Next = 0;                 // guarded by Mutex: BandCount once no band is left to hand out
    uint32 InProgress = 0;           // guarded by Mutex: handed out, not yet finished
    bool Stopped = false;            // guarded by Mutex: a band failed or threw, or the caller stopped the run
    std::exception_ptr Thrown;       // guarded by Mutex: the first band's throw, rethrown by RunBands
};

std::optional<uint32> ClaimBand(BandRun& run)
{
    std::lock_guard<std::mutex> lock(run.Mutex);
    if (run.Stopped || run.Next >= run.BandCount)
        return std::nullopt;
    ++run.InProgress;
    return run.Next++;
}

void FinishBand(BandRun& run, bool succeeded)
{
    std::lock_guard<std::mutex> lock(run.Mutex);
    --run.InProgress;
    if (!succeeded)
        run.Stopped = true;
    if (run.InProgress == 0)
        run.Settled.notify_all();
}

void StopRun(BandRun& run)
{
    std::lock_guard<std::mutex> lock(run.Mutex);
    run.Stopped = true;
}

bool HasBandsLeft(BandRun& run)
{
    std::lock_guard<std::mutex> lock(run.Mutex);
    return !run.Stopped && run.Next < run.BandCount;
}

// A band that throws fails the run instead of escaping: on a helper the throw
// would never decrement InProgress, and the caller would wait forever. The first
// throw is kept for RunBands to rethrow on the calling thread.
bool EncodeBandCaught(BandRun& run, uint32 band)
{
    try
    {
        return run.EncodeBand(band);
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(run.Mutex);
        if (!run.Thrown)
            run.Thrown = std::current_exception();
    }
    return false;
}

// Every exit from RunBands, a throw from stopRequested or from queueing a helper
// included, hands out no further band and waits for the bands in progress: they
// write into the caller's frame, which the exit unwinds. Only bands already
// started are waited for: a helper that has not run yet finds nothing left to
// claim and ends on its own.
class SettleRunOnExit
{
  public:
    explicit SettleRunOnExit(BandRun& run) : m_Run(run) {}
    ~SettleRunOnExit()
    {
        std::unique_lock<std::mutex> lock(m_Run.Mutex);
        m_Run.Next = m_Run.BandCount;
        m_Run.Settled.wait(lock, [this] { return m_Run.InProgress == 0; });
    }

    SettleRunOnExit(const SettleRunOnExit&) = delete;
    SettleRunOnExit& operator=(const SettleRunOnExit&) = delete;

  private:
    BandRun& m_Run;
};
} // namespace

struct TextureCookWorkers::Slots
{
    JobSystem::WorkStealingThreadPool* Pool = nullptr;
    std::shared_ptr<AssetDecodeGate> Gate;

    // One helper job: one band within a gate slot for texture work, then back to
    // the end of the Background lane while bands remain. A helper that finds no
    // slot or no band left ends; the calling thread encodes whatever the helpers leave.
    static void RunHelperBand(const std::shared_ptr<Slots>& slots, const std::shared_ptr<BandRun>& run)
    {
        if (!slots->Gate->TryAcquire(AssetDecodeGate::Work::Texture))
            return;
        const std::optional<uint32> band = ClaimBand(*run);
        if (band)
            FinishBand(*run, EncodeBandCaught(*run, *band));
        slots->Gate->Release(AssetDecodeGate::Work::Texture);

        // After shutdown EnqueueWork runs the job on this thread, which would
        // recurse once per band; the calling thread encodes the rest instead.
        if (band && HasBandsLeft(*run) && !slots->Pool->IsShuttingDown())
            EnqueueHelper(slots, run);
    }

    static void EnqueueHelper(const std::shared_ptr<Slots>& slots, const std::shared_ptr<BandRun>& run)
    {
        slots->Pool->EnqueueWork([slots, run] { RunHelperBand(slots, run); }, JobSystem::JobPriority::Background);
    }
};

TextureCookWorkers::TextureCookWorkers(JobSystem::WorkStealingThreadPool& pool, std::shared_ptr<AssetDecodeGate> gate)
    : m_Slots(std::make_shared<Slots>())
{
    m_Slots->Pool = &pool;
    m_Slots->Gate = std::move(gate);
}

TextureCookWorkers::~TextureCookWorkers() = default;

bool TextureCookWorkers::RunBands(uint32 bandCount, std::function<bool(uint32)> encodeBand,
                                  const std::function<bool()>& stopRequested)
{
    auto run = std::make_shared<BandRun>();
    run->EncodeBand = std::move(encodeBand);
    run->BandCount = bandCount;

    {
        const SettleRunOnExit settle(*run);

        // As many helpers as the gate has texture slots free now; one that finds the
        // slots taken by the time it runs ends at once.
        JobSystem::WorkStealingThreadPool& pool = *m_Slots->Pool;
        const bool canFork = !pool.IsInlineMode() && !pool.IsShuttingDown() && bandCount > 1;
        const size_t helpers = canFork ? std::min<size_t>(bandCount - 1, m_Slots->Gate->FreeTextureSlots()) : 0;
        for (size_t i = 0; i < helpers; ++i)
            Slots::EnqueueHelper(m_Slots, run);

        for (;;)
        {
            if (stopRequested && stopRequested())
            {
                StopRun(*run);
                break;
            }
            const std::optional<uint32> band = ClaimBand(*run);
            if (!band)
                break;
            FinishBand(*run, EncodeBandCaught(*run, *band));
        }
    }

    std::exception_ptr thrown;
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(run->Mutex);
        thrown = run->Thrown;
        stopped = run->Stopped;
    }
    if (thrown)
        std::rethrow_exception(thrown);
    return !stopped;
}

} // namespace GameEngine
