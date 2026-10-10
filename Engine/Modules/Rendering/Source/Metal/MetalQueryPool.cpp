#include "MetalQueryPool.h"

#include "MetalCommandList.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

namespace
{
constexpr uint32_t kInvalidIndex = UINT32_MAX;

// A slot the GPU wrote this cycle carries a tick past the slice's reopen; an
// error value or an older cycle's leftover does not.
bool IsFresh(uint64_t timestamp, uint64_t sliceOpenTick)
{
    return timestamp != MTL::CounterErrorValue && timestamp >= sliceOpenTick;
}

MTL::CounterSet* FindTimestampCounterSet(MTL::Device* device)
{
    NS::Array* sets = device->counterSets();
    if (sets == nullptr)
    {
        return nullptr;
    }
    for (NS::UInteger i = 0; i < sets->count(); ++i)
    {
        auto* set = static_cast<MTL::CounterSet*>(sets->object(i));
        if (set != nullptr && set->name() != nullptr &&
            set->name()->isEqualToString(MTL::CommonCounterSetTimestamp))
        {
            return set;
        }
    }
    return nullptr;
}
} // namespace

MetalQueryPool::MetalQueryPool(MTL::Device* device, const Config& config)
    : m_Device(device), m_Config(config)
{
}

MetalQueryPool::~MetalQueryPool()
{
    Shutdown();
}

bool MetalQueryPool::Initialize()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Device == nullptr)
    {
        return false;
    }
    // Which sampling points the device offers decides how finely a pass can be
    // timed: only atDraw/atDispatch/atBlit would allow a timestamp INSIDE an
    // encoder, and Apple GPUs offer none of them. Logged so the granularity
    // behind EncoderSpan semantics is visible per device, not assumed.
    const bool stageBoundary = m_Device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary);
    Logger::Log::Info("MetalQueryPool: counter sampling points — atStageBoundary={} atDrawBoundary={} "
                      "atDispatchBoundary={} atTileDispatchBoundary={} atBlitBoundary={}",
                      stageBoundary,
                      m_Device->supportsCounterSampling(MTL::CounterSamplingPointAtDrawBoundary),
                      m_Device->supportsCounterSampling(MTL::CounterSamplingPointAtDispatchBoundary),
                      m_Device->supportsCounterSampling(MTL::CounterSamplingPointAtTileDispatchBoundary),
                      m_Device->supportsCounterSampling(MTL::CounterSamplingPointAtBlitBoundary));
    if (!stageBoundary)
    {
        Logger::Log::Warning("MetalQueryPool: stage-boundary counter sampling unsupported; GPU timestamps disabled");
        return false;
    }
    MTL::CounterSet* timestampSet = FindTimestampCounterSet(m_Device);
    if (timestampSet == nullptr)
    {
        Logger::Log::Warning("MetalQueryPool: no timestamp counter set; GPU timestamps disabled");
        return false;
    }

    m_Frames.resize(m_Config.FramesInFlight);
    for (uint32_t i = 0; i < m_Config.FramesInFlight; ++i)
    {
        MTL::CounterSampleBufferDescriptor* desc = MTL::CounterSampleBufferDescriptor::alloc()->init();
        desc->setCounterSet(timestampSet);
        desc->setStorageMode(MTL::StorageModeShared);
        desc->setSampleCount(m_Config.MaxTimestampQueries);
        const std::string label = "GE.TimestampSamples." + std::to_string(i);
        desc->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
        NS::Error* error = nullptr;
        MTL::CounterSampleBuffer* buffer = m_Device->newCounterSampleBuffer(desc, &error);
        desc->release();
        if (buffer == nullptr)
        {
            Logger::Log::Warning("MetalQueryPool: counter sample buffer creation failed: {}",
                                 error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
            Shutdown();
            return false;
        }
        FrameSlice& slice = m_Frames[i];
        slice.sampleBuffer = buffer;
        slice.querySample.assign(m_Config.MaxTimestampQueries, kInvalidIndex);
        slice.sampleUnit.assign(m_Config.MaxTimestampQueries, kInvalidIndex);
        slice.queryNames.resize(m_Config.MaxTimestampQueries);

        // Occlusion visibility-result buffer: one uint64 fragment count per slot,
        // host-visible so results read back without a blit.
        const uint64_t visBytes = static_cast<uint64_t>(m_Config.MaxOcclusionQueries) * sizeof(uint64_t);
        slice.visibilityBuffer = m_Device->newBuffer(visBytes, MTL::StorageModeShared);
        if (slice.visibilityBuffer != nullptr)
        {
            const std::string visLabel = "GE.VisibilityResults." + std::to_string(i);
            slice.visibilityBuffer->setLabel(NS::String::string(visLabel.c_str(), NS::UTF8StringEncoding));
            std::memset(slice.visibilityBuffer->contents(), 0, visBytes);
        }
        slice.occlusionValues.assign(m_Config.MaxOcclusionQueries, 0);
    }

    MTL::Timestamp cpu = 0;
    MTL::Timestamp gpu = 0;
    m_Device->sampleTimestamps(&cpu, &gpu);
    m_BaseCpuTicks = cpu;
    m_BaseGpuTicks = gpu;
    m_LastSampledGpuTick = gpu;

    m_Valid = true;
    return true;
}

void MetalQueryPool::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    for (FrameSlice& slice : m_Frames)
    {
        if (slice.sampleBuffer != nullptr)
        {
            slice.sampleBuffer->release();
            slice.sampleBuffer = nullptr;
        }
        if (slice.visibilityBuffer != nullptr)
        {
            slice.visibilityBuffer->release();
            slice.visibilityBuffer = nullptr;
        }
    }
    m_Frames.clear();
    m_Valid = false;
}

void MetalQueryPool::UpdateTimestampCorrelation()
{
    MTL::Timestamp cpu = 0;
    MTL::Timestamp gpu = 0;
    m_Device->sampleTimestamps(&cpu, &gpu);
    // sampleTimestamps reports the CPU timestamp in nanoseconds (not raw mach
    // ticks — applying mach_timebase on top inflates the period 41.7x on
    // Apple Silicon). GPU ticks map linearly onto that domain.
    if (cpu > m_BaseCpuTicks && gpu > m_BaseGpuTicks)
    {
        m_GpuPeriodNs = static_cast<double>(cpu - m_BaseCpuTicks) / static_cast<double>(gpu - m_BaseGpuTicks);
    }
    m_LastSampledGpuTick = gpu;
}

void MetalQueryPool::ResolveSlice(uint32_t frameIndex)
{
    FrameSlice& slice = m_Frames[frameIndex];

    // Occlusion results are host-visible — copy this slot's counts out before
    // the slice is reused. Independent of the timestamp path below.
    slice.occlusionCacheValid = false;
    if (slice.visibilityBuffer != nullptr && slice.occlusionCount > 0)
    {
        const auto* counts = static_cast<const uint64_t*>(slice.visibilityBuffer->contents());
        const uint32_t n = std::min(slice.occlusionCount,
                                    static_cast<uint32_t>(slice.occlusionValues.size()));
        for (uint32_t i = 0; i < n; ++i)
            slice.occlusionValues[i] = counts[i];
        slice.occlusionCacheValid = true;
    }

    slice.cachedValues.clear();
    slice.cachedUnits.clear();
    slice.cachedSlots.clear();
    slice.cachedUnitTicks.clear();
    slice.cacheValid = false;
    if (slice.queryCount == 0 || slice.sampleCount == 0 || slice.sampleBuffer == nullptr)
    {
        return;
    }
    NS::Data* data = slice.sampleBuffer->resolveCounterRange(NS::Range::Make(0, slice.sampleCount));
    if (data == nullptr || data->length() < slice.sampleCount * sizeof(MTL::CounterResultTimestamp))
    {
        return;
    }
    const auto* samples = static_cast<const MTL::CounterResultTimestamp*>(data->mutableBytes());
    slice.cachedValues.assign(slice.queryCount, 0);
    slice.cachedUnits.assign(slice.queryCount, kInvalidIndex);
    slice.cachedSlots.assign(slice.queryCount, kInvalidIndex);
    for (uint32_t i = 0; i < slice.queryCount; ++i)
    {
        const uint32_t sampleSlot = slice.querySample[i];
        if (sampleSlot < slice.sampleCount && IsFresh(samples[sampleSlot].timestamp, slice.openTick))
        {
            slice.cachedValues[i] = samples[sampleSlot].timestamp;
            slice.cachedUnits[i] = slice.sampleUnit[sampleSlot];
            slice.cachedSlots[i] = sampleSlot;
        }
    }
    // Per-encoder busy ticks: each stage's own span, so the gap a TBDR GPU
    // leaves between an encoder's vertex and fragment stages is not charged to
    // it. A stage whose boundaries did not resolve contributes nothing (an
    // encoder the GPU never ran, or a render pass with no fragment work).
    slice.cachedUnitTicks.assign(slice.encoderCount, 0);
    const auto stageTicks = [&](uint32_t beginSlot, uint32_t endSlot) -> uint64_t
    {
        if (beginSlot == kInvalidIndex || endSlot == kInvalidIndex || beginSlot >= slice.sampleCount ||
            endSlot >= slice.sampleCount)
        {
            return 0;
        }
        const uint64_t beginTick = samples[beginSlot].timestamp;
        const uint64_t endTick = samples[endSlot].timestamp;
        if (!IsFresh(beginTick, slice.openTick) || !IsFresh(endTick, slice.openTick) ||
            endTick < beginTick)
        {
            return 0;
        }
        return endTick - beginTick;
    };
    for (uint32_t unit = 0; unit < slice.encoderCount && unit < slice.unitSlots.size(); ++unit)
    {
        const EncoderSampleSlots& slots = slice.unitSlots[unit];
        if (!slots.SplitStages())
        {
            slice.cachedUnitTicks[unit] = stageTicks(slots.Start, slots.End);
            continue;
        }
        slice.cachedUnitTicks[unit] = stageTicks(slots.Start, slots.FirstStageEnd) +
                                      stageTicks(slots.SecondStageBegin, slots.End);
    }
    slice.cacheValid = true;
}

void MetalQueryPool::BeginFrame(uint32_t frameIndex)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid)
    {
        return;
    }
    if (frameIndex >= m_Config.FramesInFlight)
    {
        frameIndex %= m_Config.FramesInFlight;
    }

    // The caller waited this slot's frame pacing semaphore, so the slot's GPU
    // work is complete and the samples can be resolved without stalling.
    ResolveSlice(frameIndex);

    m_CurrentFrame = frameIndex;
    FrameSlice& slice = m_Frames[m_CurrentFrame];
    slice.queryCount = 0;
    slice.sampleCount = 0;
    slice.encoderCount = 0;
    slice.unitSlots.clear();
    std::fill(slice.querySample.begin(), slice.querySample.end(), kInvalidIndex);
    std::fill(slice.sampleUnit.begin(), slice.sampleUnit.end(), kInvalidIndex);

    // Reset occlusion slots; zero the buffer so unused/short-circuited slots
    // read back as 0 rather than last cycle's count.
    if (slice.visibilityBuffer != nullptr)
    {
        std::memset(slice.visibilityBuffer->contents(), 0,
                    static_cast<size_t>(m_Config.MaxOcclusionQueries) * sizeof(uint64_t));
    }
    slice.occlusionCount = 0;

    UpdateTimestampCorrelation();
    // Everything the GPU writes into this slice from here on is newer than this
    // tick, which is what makes a leftover value from an older cycle detectable.
    slice.openTick = m_LastSampledGpuTick;
}

void MetalQueryPool::InvalidateCachedTimestampResults(uint32_t frameIndex)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (frameIndex >= m_Frames.size())
    {
        return;
    }
    m_Frames[frameIndex].cacheValid = false;
    m_Frames[frameIndex].cachedValues.clear();
}

void MetalQueryPool::EndFrame()
{
}

uint32_t MetalQueryPool::WriteTimestamp(CommandList* commandList, TimestampPoint point,
                                       const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || commandList == nullptr)
    {
        return kInvalidIndex;
    }
    auto* metalList = static_cast<MetalCommandList*>(commandList);
    if (!metalList->IsRecording())
    {
        return kInvalidIndex;
    }
    FrameSlice& slice = m_Frames[m_CurrentFrame];
    if (slice.queryCount >= m_Config.MaxTimestampQueries)
    {
        return kInvalidIndex;
    }
    if (slice.queryCount == 0)
    {
        InvalidateCachedTimestampResults(m_CurrentFrame);
    }
    const uint32_t idx = slice.queryCount++;
    slice.querySample[idx] = kInvalidIndex;
    slice.queryNames[idx] = name;
    const uint32_t queryIndex = m_CurrentFrame * m_Config.MaxTimestampQueries + idx;
    metalList->RecordTimestampQuery(*this, queryIndex, point);
    return queryIndex;
}

uint32_t MetalQueryPool::BeginOcclusionQuery(CommandList* commandList, const std::string&)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || commandList == nullptr)
    {
        return kInvalidIndex;
    }
    // Occlusion must be enabled before the render pass so its visibility buffer
    // was attached at encoder creation; otherwise setVisibilityResultMode would
    // write nowhere.
    if (!m_OcclusionEnabled.load(std::memory_order_relaxed))
    {
        return kInvalidIndex;
    }
    auto* metalList = static_cast<MetalCommandList*>(commandList);
    if (!metalList->IsRecording())
    {
        return kInvalidIndex;
    }
    FrameSlice& slice = m_Frames[m_CurrentFrame];
    if (slice.visibilityBuffer == nullptr || slice.occlusionCount >= m_Config.MaxOcclusionQueries)
    {
        return kInvalidIndex;
    }
    const uint32_t slot = slice.occlusionCount++;
    // Counting mode writes the number of fragments that passed the depth test.
    metalList->SetVisibilityResultMode(static_cast<uint32_t>(MTL::VisibilityResultModeCounting),
                                       static_cast<uint64_t>(slot) * sizeof(uint64_t));
    return m_CurrentFrame * m_Config.MaxOcclusionQueries + slot;
}

void MetalQueryPool::EndOcclusionQuery(CommandList* commandList, uint32_t queryIndex)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || commandList == nullptr || queryIndex == kInvalidIndex)
    {
        return;
    }
    auto* metalList = static_cast<MetalCommandList*>(commandList);
    metalList->SetVisibilityResultMode(static_cast<uint32_t>(MTL::VisibilityResultModeDisabled), 0);
}

uint32_t MetalQueryPool::BeginPipelineStats(CommandList*, const std::string&)
{
    return kInvalidIndex;
}

void MetalQueryPool::EndPipelineStats(CommandList*, uint32_t)
{
}

bool MetalQueryPool::GetTimestampResult(uint32_t queryIndex, QueryResult& outResult)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    outResult = QueryResult{};
    outResult.Type = QueryType::Timestamp;
    if (!m_Valid || queryIndex == kInvalidIndex)
    {
        return false;
    }
    const uint32_t frame = queryIndex / m_Config.MaxTimestampQueries;
    const uint32_t idx = queryIndex % m_Config.MaxTimestampQueries;
    if (frame >= m_Frames.size() || !m_Frames[frame].cacheValid)
    {
        return false;
    }
    const FrameSlice& slice = m_Frames[frame];
    if (idx >= slice.cachedValues.size() || slice.cachedValues[idx] == 0)
    {
        return false;
    }
    outResult.Value = slice.cachedValues[idx];
    outResult.Available = true;
    return true;
}

bool MetalQueryPool::GetOcclusionResult(uint32_t queryIndex, QueryResult& outResult)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    outResult = QueryResult{};
    outResult.Type = QueryType::Occlusion;
    if (!m_Valid || queryIndex == kInvalidIndex)
    {
        return false;
    }
    const uint32_t frame = queryIndex / m_Config.MaxOcclusionQueries;
    const uint32_t slot = queryIndex % m_Config.MaxOcclusionQueries;
    if (frame >= m_Frames.size() || !m_Frames[frame].occlusionCacheValid)
    {
        return false;
    }
    const FrameSlice& slice = m_Frames[frame];
    if (slot >= slice.occlusionValues.size() || slot >= slice.occlusionCount)
    {
        return false;
    }
    outResult.Value = slice.occlusionValues[slot];
    outResult.Available = true;
    return true;
}

MTL::Buffer* MetalQueryPool::GetCurrentVisibilityBuffer() const
{
    // Zero-touch fast path: a single relaxed load when occlusion is off (the
    // default), so BeginRenderPass attaches nothing and takes no lock.
    if (!m_OcclusionEnabled.load(std::memory_order_relaxed))
        return nullptr;
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return m_Valid ? m_Frames[m_CurrentFrame].visibilityBuffer : nullptr;
}

bool MetalQueryPool::GetPipelineStatsResult(uint32_t, QueryResult& outResult)
{
    outResult = QueryResult{};
    outResult.Type = QueryType::PipelineStats;
    return false;
}

const IQueryPool::ProfilingStats& MetalQueryPool::GetProfilingStats() const
{
    return m_Stats;
}

double MetalQueryPool::GetTimestampPeriod() const
{
    return m_GpuPeriodNs;
}

double MetalQueryPool::TimestampToMs(uint64_t timestamp) const
{
    return static_cast<double>(timestamp) * m_GpuPeriodNs / 1'000'000.0;
}

std::vector<QueryResult> MetalQueryPool::GetAllTimestampResults() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::vector<QueryResult> results;
    for (const FrameSlice& slice : m_Frames)
    {
        if (!slice.cacheValid)
        {
            continue;
        }
        for (uint32_t i = 0; i < slice.cachedValues.size(); ++i)
        {
            QueryResult result{};
            result.Type = QueryType::Timestamp;
            result.Value = slice.cachedValues[i];
            result.Available = slice.cachedValues[i] != 0;
            result.Name = slice.queryNames[i];
            results.push_back(std::move(result));
        }
    }
    return results;
}

void MetalQueryPool::ResetQueries(CommandList*)
{
    // Counter sample buffers need no GPU-side reset; slices recycle in
    // BeginFrame after the frame pacing wait.
}

bool MetalQueryPool::IsValid() const
{
    return m_Valid;
}

void MetalQueryPool::DebugPrintResults() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    for (uint32_t frame = 0; frame < m_Frames.size(); ++frame)
    {
        const FrameSlice& slice = m_Frames[frame];
        if (!slice.cacheValid)
        {
            continue;
        }
        for (uint32_t i = 0; i < slice.cachedValues.size(); ++i)
        {
            Logger::Log::Info("MetalQueryPool: frame {} query {} '{}' = {} ticks", frame, i,
                              slice.queryNames[i], slice.cachedValues[i]);
        }
    }
}

bool MetalQueryPool::ShouldAttachSamples() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return m_Valid && m_Frames[m_CurrentFrame].queryCount > 0;
}

MTL::CounterSampleBuffer* MetalQueryPool::GetCurrentSampleBuffer() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return m_Valid ? m_Frames[m_CurrentFrame].sampleBuffer : nullptr;
}

bool MetalQueryPool::AllocateEncoderSampleSlots(bool splitStages, EncoderSampleSlots& outSlots)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid)
    {
        return false;
    }
    FrameSlice& slice = m_Frames[m_CurrentFrame];
    const uint32_t wanted = splitStages ? 4u : 2u;
    if (slice.sampleCount + wanted > m_Config.MaxTimestampQueries)
    {
        return false;
    }
    const uint32_t unit = slice.encoderCount++;
    EncoderSampleSlots slots;
    slots.Start = slice.sampleCount++;
    slots.End = slice.sampleCount++;
    if (splitStages)
    {
        slots.FirstStageEnd = slice.sampleCount++;
        slots.SecondStageBegin = slice.sampleCount++;
    }
    for (const uint32_t slot : {slots.Start, slots.End, slots.FirstStageEnd, slots.SecondStageBegin})
    {
        if (slot != UINT32_MAX)
            slice.sampleUnit[slot] = unit;
    }
    if (unit >= slice.unitSlots.size())
        slice.unitSlots.resize(unit + 1);
    slice.unitSlots[unit] = slots;
    outSlots = slots;
    return true;
}

bool MetalQueryPool::GetTimestampSpanTicks(uint32_t beginQuery, uint32_t endQuery,
                                           uint64_t& outTicks) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || beginQuery == kInvalidIndex || endQuery == kInvalidIndex)
    {
        return false;
    }
    const uint32_t frame = beginQuery / m_Config.MaxTimestampQueries;
    if (frame != endQuery / m_Config.MaxTimestampQueries || frame >= m_Frames.size() ||
        !m_Frames[frame].cacheValid)
    {
        return false;
    }
    const FrameSlice& slice = m_Frames[frame];
    const uint32_t beginIdx = beginQuery % m_Config.MaxTimestampQueries;
    const uint32_t endIdx = endQuery % m_Config.MaxTimestampQueries;
    if (beginIdx >= slice.cachedUnits.size() || endIdx >= slice.cachedUnits.size())
    {
        return false;
    }
    // Both ends on ONE boundary sample: the span recorded no GPU work of its own.
    if (slice.cachedSlots[beginIdx] == slice.cachedSlots[endIdx])
    {
        outTicks = 0;
        return true;
    }
    const uint32_t beginUnit = slice.cachedUnits[beginIdx];
    const uint32_t endUnit = slice.cachedUnits[endIdx];
    if (beginUnit == kInvalidIndex || endUnit == kInvalidIndex)
    {
        return false;
    }
    const uint32_t first = std::min(beginUnit, endUnit);
    const uint32_t last = std::max(beginUnit, endUnit);
    if (last >= slice.cachedUnitTicks.size())
    {
        return false;
    }
    uint64_t ticks = 0;
    for (uint32_t unit = first; unit <= last; ++unit)
    {
        ticks += slice.cachedUnitTicks[unit];
    }
    outTicks = ticks;
    return true;
}

uint32_t MetalQueryPool::GetTimestampUnitId(uint32_t queryIndex) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || queryIndex == kInvalidIndex)
    {
        return kInvalidTimestampUnit;
    }
    const uint32_t frame = queryIndex / m_Config.MaxTimestampQueries;
    const uint32_t idx = queryIndex % m_Config.MaxTimestampQueries;
    if (frame >= m_Frames.size() || !m_Frames[frame].cacheValid ||
        idx >= m_Frames[frame].cachedUnits.size())
    {
        return kInvalidTimestampUnit;
    }
    const uint32_t unit = m_Frames[frame].cachedUnits[idx];
    return unit == kInvalidIndex ? kInvalidTimestampUnit : unit;
}

void MetalQueryPool::BindQueryToSample(uint32_t queryIndex, uint32_t sampleSlot)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Valid || queryIndex == kInvalidIndex)
    {
        return;
    }
    const uint32_t frame = queryIndex / m_Config.MaxTimestampQueries;
    const uint32_t idx = queryIndex % m_Config.MaxTimestampQueries;
    if (frame >= m_Frames.size() || idx >= m_Frames[frame].querySample.size())
    {
        return;
    }
    m_Frames[frame].querySample[idx] = sampleSlot;
}

} // namespace Rendering
} // namespace GameEngine
