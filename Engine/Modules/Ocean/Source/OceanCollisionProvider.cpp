#include "Ocean/OceanCollisionProvider.h"

#include "Ocean/OceanFFTCollisionAsset.h"
#include "Ocean/OceanRenderFeature.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine::Ocean
{

namespace
{
constexpr uint32 kDefaultMaxCollisionQueryPoints = 8192u;

bool IsGPUCollisionSample(const OceanSurfaceSample& sample)
{
    return sample.Valid &&
           (sample.Source == OceanQuerySource::GPUQuery ||
            sample.Source == OceanQuerySource::CombinedCascade);
}
}

struct OceanFeatureCollisionProvider::Impl
{
    struct Batch
    {
        OceanCollisionQueryHandle Handle = 0;
        OceanCollisionOwnerId Owner = 0;
        float32 MinimumSpatialLength = 0.0f;
        OceanQueryField Fields = OceanQueryField::AllSurface;
        std::vector<OceanSurfaceQueryPoint> Points;
        std::vector<OceanSurfaceSample> Surface;
        std::vector<OceanCurrentSample> Flow;
        std::vector<OceanSurfaceSample> PreviousSurface;
        float32 PreviousTime = 0.0f;
        bool HasPrevious = false;
        bool HasCompleted = false;
        bool Pending = true;
        OceanGPUQueryToken GPURequest = 0u;
        OceanQueryStatus Status = OceanQueryStatus::NotReady;
        OceanQueryStatus CompletedStatus = OceanQueryStatus::NotReady;
    };

    explicit Impl(OceanRenderFeature& inFeature) : Feature(inFeature) {}

    OceanRenderFeature& Feature;
    mutable std::mutex Mutex;
    std::unordered_map<OceanCollisionQueryHandle, Batch> Batches;
    std::unordered_map<OceanCollisionOwnerId, OceanCollisionQueryHandle> OwnerHandles;
    std::atomic<OceanCollisionQueryHandle> NextHandle{1u};
    std::atomic<OceanCollisionProviderMode> Mode{OceanCollisionProviderMode::Auto};
    std::atomic<uint32> MaxQueryPoints{kDefaultMaxCollisionQueryPoints};
    std::atomic<bool> AllowGPUQueries{true};
    std::atomic<bool> AllowBakedFFT{true};
    std::atomic<bool> AllowGerstnerFallback{true};
    std::atomic<float32> DefaultMinimumSpatialLength{0.0f};
    std::shared_ptr<const OceanFFTCollisionAsset> BakedFFT;
    uint64 ExpectedSpectrumHash = 0u;
};

OceanFeatureCollisionProvider::OceanFeatureCollisionProvider(OceanRenderFeature& feature)
    : m_Impl(new Impl(feature))
{
}

OceanFeatureCollisionProvider::~OceanFeatureCollisionProvider()
{
    delete m_Impl;
}

OceanCollisionProviderMode OceanFeatureCollisionProvider::GetMode() const
{
    return m_Impl->Mode.load(std::memory_order_acquire);
}

void OceanFeatureCollisionProvider::SetMode(OceanCollisionProviderMode mode)
{
    m_Impl->Mode.store(mode, std::memory_order_release);
}

void OceanFeatureCollisionProvider::SetBakedFFTAsset(
    std::shared_ptr<const OceanFFTCollisionAsset> asset)
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    m_Impl->BakedFFT = std::move(asset);
}

std::shared_ptr<const OceanFFTCollisionAsset>
OceanFeatureCollisionProvider::GetBakedFFTAsset() const
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    return m_Impl->BakedFFT;
}

void OceanFeatureCollisionProvider::SetExpectedSpectrumHash(uint64 hash)
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    m_Impl->ExpectedSpectrumHash = hash;
}

uint64 OceanFeatureCollisionProvider::GetExpectedSpectrumHash() const
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    return m_Impl->ExpectedSpectrumHash;
}

void OceanFeatureCollisionProvider::SetMaxQueryPoints(uint32 count)
{
    m_Impl->MaxQueryPoints.store(std::max(count, 1u), std::memory_order_release);
}

uint32 OceanFeatureCollisionProvider::GetMaxQueryPoints() const
{
    return m_Impl->MaxQueryPoints.load(std::memory_order_acquire);
}

void OceanFeatureCollisionProvider::SetFallbackPolicy(bool allowGPUQueries,
                                                       bool allowBakedFFT,
                                                       bool allowGerstnerFallback,
                                                       float32 defaultMinimumSpatialLength)
{
    m_Impl->AllowGPUQueries.store(allowGPUQueries, std::memory_order_release);
    m_Impl->AllowBakedFFT.store(allowBakedFFT, std::memory_order_release);
    m_Impl->AllowGerstnerFallback.store(allowGerstnerFallback, std::memory_order_release);
    m_Impl->DefaultMinimumSpatialLength.store(
        std::max(defaultMinimumSpatialLength, 0.0f), std::memory_order_release);
}

OceanCollisionQueryHandle OceanFeatureCollisionProvider::Submit(
    const OceanCollisionQueryDesc& query)
{
    if (query.Owner == 0u || !query.Points || query.Count == 0u ||
        query.Count > GetMaxQueryPoints())
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    OceanCollisionQueryHandle handle = 0u;
    if (const auto existing = m_Impl->OwnerHandles.find(query.Owner);
        existing != m_Impl->OwnerHandles.end())
    {
        handle = existing->second;
    }
    else
    {
        handle = m_Impl->NextHandle.fetch_add(1u, std::memory_order_relaxed);
        if (handle == 0u)
            handle = m_Impl->NextHandle.fetch_add(1u, std::memory_order_relaxed);
        m_Impl->OwnerHandles[query.Owner] = handle;
    }

    Impl::Batch& batch = m_Impl->Batches[handle];
    batch.Handle = handle;
    batch.Owner = query.Owner;
    batch.MinimumSpatialLength = query.MinimumSpatialLength > 0.0f
                                     ? query.MinimumSpatialLength
                                     : m_Impl->DefaultMinimumSpatialLength.load(
                                           std::memory_order_acquire);
    batch.Fields = query.Fields;
    batch.Points.assign(query.Points, query.Points + query.Count);
    batch.Pending = true;
    batch.Status = OceanQueryStatus::NotReady;
    return handle;
}

OceanQueryStatus OceanFeatureCollisionProvider::GetStatus(
    OceanCollisionQueryHandle handle) const
{
    if (handle == 0u)
        return OceanQueryStatus::InvalidArgument;
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    const auto it = m_Impl->Batches.find(handle);
    return it == m_Impl->Batches.end() ? OceanQueryStatus::InvalidArgument
                                      : it->second.Status;
}

bool OceanFeatureCollisionProvider::CopyResults(OceanCollisionQueryHandle handle,
                                                OceanSurfaceSample* outSurface,
                                                OceanCurrentSample* outFlow,
                                                uint32 capacity,
                                                uint32* outCount) const
{
    if (outCount)
        *outCount = 0u;
    if (handle == 0u)
        return false;

    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    const auto it = m_Impl->Batches.find(handle);
    if (it == m_Impl->Batches.end() || !it->second.HasCompleted ||
        !OceanQuerySucceeded(it->second.CompletedStatus))
        return false;

    const uint32 count = static_cast<uint32>(it->second.Surface.size());
    if (outCount)
        *outCount = count;
    if (capacity < count)
        return false;
    if (outSurface)
        std::copy(it->second.Surface.begin(), it->second.Surface.end(), outSurface);
    if (outFlow && it->second.Flow.size() == count)
        std::copy(it->second.Flow.begin(), it->second.Flow.end(), outFlow);
    return true;
}

void OceanFeatureCollisionProvider::Update(uint32 maxBatches)
{
    struct Work
    {
        OceanCollisionQueryHandle Handle = 0u;
        OceanQueryField Fields = OceanQueryField::AllSurface;
        std::vector<OceanSurfaceQueryPoint> Points;
        std::vector<OceanSurfaceSample> Previous;
        float32 PreviousTime = 0.0f;
        bool HasPrevious = false;
        float32 MinimumSpatialLength = 0.0f;
        OceanGPUQueryToken GPURequest = 0u;
        bool HasNewRequest = false;
    };

    std::vector<Work> work;
    {
        std::lock_guard<std::mutex> lock(m_Impl->Mutex);
        for (auto& [handle, batch] : m_Impl->Batches)
        {
            if (!batch.Pending && batch.GPURequest == 0u)
                continue;
            Work item{};
            item.Handle = handle;
            item.Fields = batch.Fields;
            item.Points = batch.Points;
            item.Previous = batch.PreviousSurface;
            item.PreviousTime = batch.PreviousTime;
            item.HasPrevious = batch.HasPrevious;
            item.MinimumSpatialLength = batch.MinimumSpatialLength;
            item.GPURequest = batch.GPURequest;
            item.HasNewRequest = batch.Pending;
            batch.Pending = false;
            work.push_back(std::move(item));
            if (maxBatches != 0u && work.size() >= maxBatches)
                break;
        }
    }

    const OceanCollisionProviderMode mode = GetMode();
    const bool allowGPU = m_Impl->AllowGPUQueries.load(std::memory_order_acquire);
    const bool allowBaked = m_Impl->AllowBakedFFT.load(std::memory_order_acquire);
    const bool allowGerstner =
        m_Impl->AllowGerstnerFallback.load(std::memory_order_acquire);
    const float32 currentTime = m_Impl->Feature.GetParams().Time;
    std::shared_ptr<const OceanFFTCollisionAsset> baked;
    uint64 expectedSpectrumHash = 0u;
    {
        std::lock_guard<std::mutex> lock(m_Impl->Mutex);
        baked = m_Impl->BakedFFT;
        expectedSpectrumHash = m_Impl->ExpectedSpectrumHash;
    }
    for (Work& item : work)
    {
        std::vector<OceanSurfaceSample> surface(item.Points.size());
        std::vector<OceanCurrentSample> flow;
        OceanQueryStatus status = OceanQueryStatus::Success;
        OceanGPUQueryToken nextGPURequest = item.GPURequest;
        bool consumedGPURequest = false;
        std::vector<OceanSurfaceSample> completedGPU;
        const bool haveCompletedGPU =
            item.GPURequest != 0u && m_Impl->Feature.IsGPUQueryReady() &&
            m_Impl->Feature.GetGPUQuery().TryTake(item.GPURequest, completedGPU) &&
            completedGPU.size() == item.Points.size();
        if (haveCompletedGPU)
        {
            consumedGPURequest = true;
            nextGPURequest = 0u;
        }

        if (mode == OceanCollisionProviderMode::None)
        {
            status = OceanQueryStatus::ProviderUnavailable;
        }
        else if (mode == OceanCollisionProviderMode::BakedFFTCPU)
        {
            if (!allowBaked || !baked)
            {
                status = OceanQueryStatus::ProviderUnavailable;
            }
            else if (!baked->IsCompatible(expectedSpectrumHash) ||
                     (item.MinimumSpatialLength > 0.0f &&
                      baked->GetDesc().SmallestWavelength > item.MinimumSpatialLength))
            {
                status = OceanQueryStatus::AssetMismatch;
            }
            else
            {
                for (size_t i = 0u; i < item.Points.size(); ++i)
                {
                    if (!baked->SampleSurface(item.Points[i].X, item.Points[i].Z,
                                              currentTime, surface[i]))
                    {
                        status = OceanQueryStatus::ProviderUnavailable;
                        break;
                    }
                }
            }
        }
        else if (mode == OceanCollisionProviderMode::AnalyticGerstner)
        {
            if (!allowGerstner)
            {
                status = OceanQueryStatus::ProviderUnavailable;
            }
            else
            {
                const OceanParamsGPU params = m_Impl->Feature.GetParams();
                SampleOceanSurfaceAnalyticBatch(params, item.Points.data(),
                                                static_cast<uint32>(item.Points.size()),
                                                surface.data());
            }
        }
        else if (mode == OceanCollisionProviderMode::GPUQueries)
        {
            if (!allowGPU || !m_Impl->Feature.IsGPUQueryReady())
            {
                status = OceanQueryStatus::ProviderUnavailable;
            }
            else if (haveCompletedGPU)
            {
                surface = std::move(completedGPU);
            }
            else if (item.Points.size() > kMaxOceanGPUQueryPointsPerDispatch)
            {
                status = OceanQueryStatus::CapacityExceeded;
            }
            else
            {
                status = OceanQueryStatus::NotReady;
            }
            if (item.HasNewRequest && allowGPU && m_Impl->Feature.IsGPUQueryReady() &&
                item.Points.size() <= kMaxOceanGPUQueryPointsPerDispatch)
                nextGPURequest = m_Impl->Feature.GetGPUQuery().Enqueue(
                    item.Points.data(), static_cast<uint32>(item.Points.size()),
                    item.MinimumSpatialLength);
        }
        else // Auto: newest GPU result, compatible bake, deterministic Gerstner.
        {
            if (haveCompletedGPU)
            {
                surface = std::move(completedGPU);
            }
            const bool bakedCompatible = allowBaked && baked &&
                baked->IsCompatible(expectedSpectrumHash) &&
                (item.MinimumSpatialLength <= 0.0f ||
                 baked->GetDesc().SmallestWavelength <= item.MinimumSpatialLength);
            for (size_t i = 0u; i < surface.size(); ++i)
            {
                if (IsGPUCollisionSample(surface[i]))
                    continue;
                if (bakedCompatible &&
                    baked->SampleSurface(item.Points[i].X, item.Points[i].Z,
                                         currentTime, surface[i]))
                {
                    continue;
                }
                if (allowGerstner)
                {
                    // This compatibility path also layers water-body and typed
                    // shape inputs onto the analytic base. If the GPU branch was
                    // disabled, SampleSurface may internally see a completed
                    // heightfield; normalize the source to analytic so the policy
                    // never advertises that result as a GPU completion.
                    surface[i] = m_Impl->Feature.SampleSurface(item.Points[i].X,
                                                               item.Points[i].Z);
                    if (surface[i].Valid && IsGPUCollisionSample(surface[i]))
                    {
                        const OceanParamsGPU params = m_Impl->Feature.GetParams();
                        surface[i] = SampleOceanSurfaceAnalytic(
                            params, item.Points[i].X, item.Points[i].Z);
                    }
                }
                if (!surface[i].Valid)
                    status = OceanQueryStatus::ProviderUnavailable;
            }
            if (item.HasNewRequest && allowGPU && m_Impl->Feature.IsGPUQueryReady() &&
                item.Points.size() <= kMaxOceanGPUQueryPointsPerDispatch)
                nextGPURequest = m_Impl->Feature.GetGPUQuery().Enqueue(
                    item.Points.data(), static_cast<uint32>(item.Points.size()),
                    item.MinimumSpatialLength);
        }

        if (OceanQuerySucceeded(status) &&
            OceanQueryHasField(item.Fields, OceanQueryField::Velocity) &&
            item.HasPrevious && item.Previous.size() == surface.size())
        {
            const float32 dt = currentTime - item.PreviousTime;
            if (std::abs(dt) > 1e-5f)
            {
                for (size_t i = 0u; i < surface.size(); ++i)
                {
                    for (uint32 axis = 0u; axis < 3u; ++axis)
                        surface[i].VelocityWS[axis] =
                            (surface[i].PositionWS[axis] - item.Previous[i].PositionWS[axis]) / dt;
                }
            }
        }

        if (OceanQuerySucceeded(status) &&
            OceanQueryHasField(item.Fields, OceanQueryField::Flow))
        {
            flow.resize(item.Points.size());
            for (size_t i = 0u; i < item.Points.size(); ++i)
                flow[i] = m_Impl->Feature.SampleFlow(item.Points[i].X, item.Points[i].Z);
        }

        std::lock_guard<std::mutex> lock(m_Impl->Mutex);
        const auto found = m_Impl->Batches.find(item.Handle);
        if (found == m_Impl->Batches.end())
            continue;
        Impl::Batch& batch = found->second;
        if (consumedGPURequest || nextGPURequest != item.GPURequest)
            batch.GPURequest = nextGPURequest;
        batch.Status = status;
        if (OceanQuerySucceeded(status))
        {
            batch.Surface = std::move(surface);
            batch.Flow = std::move(flow);
            batch.CompletedStatus = status;
            batch.HasCompleted = true;
            batch.PreviousSurface = batch.Surface;
            batch.PreviousTime = currentTime;
            batch.HasPrevious = true;
        }
    }
}

bool OceanFeatureCollisionProvider::Cancel(OceanCollisionQueryHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    const auto found = m_Impl->Batches.find(handle);
    if (found == m_Impl->Batches.end())
        return false;
    m_Impl->OwnerHandles.erase(found->second.Owner);
    m_Impl->Batches.erase(found);
    return true;
}

void OceanFeatureCollisionProvider::Clear()
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    m_Impl->Batches.clear();
    m_Impl->OwnerHandles.clear();
}

} // namespace GameEngine::Ocean
