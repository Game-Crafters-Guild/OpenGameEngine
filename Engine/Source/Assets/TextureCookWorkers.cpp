#include "Assets/TextureCookWorkers.h"

#include "Assets/AssetDecodeGate.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>

namespace GameEngine {

TextureCookWorkers::TextureCookWorkers(JobSystem::WorkStealingThreadPool& pool, std::shared_ptr<AssetDecodeGate> gate)
    : m_Pool(pool), m_Gate(std::move(gate))
{
}

TextureCookWorkers::~TextureCookWorkers() = default;

bool TextureCookWorkers::RunBands(uint32 bandCount, std::function<bool(uint32)> encodeBand,
                                  const std::function<bool()>& stopRequested)
{
    // A helper encodes one band within a gate slot for texture work, then goes
    // back to the end of the Background lane while bands remain; one that finds
    // the slots taken ends, and the calling thread encodes what the helpers leave.
    AssetDecodeGate& gate = *m_Gate;
    JobSystem::ParallelForOptions::UnitAdmission textureSlot;
    textureSlot.TryAcquire = [&gate] { return gate.TryAcquire(AssetDecodeGate::Work::Texture); };
    textureSlot.Release = [&gate] { gate.Release(AssetDecodeGate::Work::Texture); };

    JobSystem::ParallelForOptions options;
    // As many helpers as the gate has texture slots free now.
    options.Helpers = std::min<size_t>(bandCount > 0 ? bandCount - 1 : 0, gate.FreeTextureSlots());
    options.HelperPriority = JobSystem::JobPriority::Background;
    options.Stop = &stopRequested;
    options.Admission = &textureSlot;
    return JobSystem::ParallelFor(
        &m_Pool, bandCount, [&encodeBand](size_t band) { return encodeBand(static_cast<uint32>(band)); }, options);
}

} // namespace GameEngine
