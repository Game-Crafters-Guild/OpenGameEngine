/**
 * @file VulkanQueryPool.cpp
 * @brief Vulkan implementation of GPU query system for performance profiling
 */

#include "VulkanQueryPool.h"
#include "VulkanCommandList.h"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace GameEngine {
namespace Rendering {

    VulkanQueryPool::VulkanQueryPool(VkDevice device, VkPhysicalDevice physicalDevice, const Config& config)
        : m_Device(device)
        , m_PhysicalDevice(physicalDevice)
        , m_Config(config)
        , m_FramesInFlight(config.FramesInFlight)
    {
        // Initialize per-frame query tracking
        m_TimestampQueries.resize(m_FramesInFlight);
        m_OcclusionQueries.resize(m_FramesInFlight);
        m_PipelineStatsQueries.resize(m_FramesInFlight);
        m_CachedTimestampValues.resize(m_FramesInFlight);
        m_CachedTimestampValuesValid.assign(m_FramesInFlight, false);

        for (uint32_t i = 0; i < m_FramesInFlight; ++i) {
            m_TimestampQueries[i].reserve(m_Config.MaxTimestampQueries);
            m_OcclusionQueries[i].reserve(m_Config.MaxOcclusionQueries);
            m_PipelineStatsQueries[i].reserve(m_Config.MaxPipelineStatsQueries);
        }
    }

    VulkanQueryPool::~VulkanQueryPool() {
        Shutdown();
    }

    bool VulkanQueryPool::Initialize() {


        // Check device support for different query types
        if (!CheckQuerySupport()) {
            std::cerr << "QueryPool: Failed to check query support" << std::endl;
            return false;
        }

        // Create query pools
        if (!CreateQueryPools()) {
            std::cerr << "QueryPool: Failed to create query pools" << std::endl;
            return false;
        }





        return true;
    }

    void VulkanQueryPool::Shutdown() {
        DestroyQueryPools();
    }

    void VulkanQueryPool::BeginFrame(uint32_t frameIndex) {
        if (frameIndex >= m_FramesInFlight)
            frameIndex %= m_FramesInFlight;

        // Read completed timestamps for the slot we are about to reuse. The caller
        // must have already waited this slot's fence so vkGetQueryPoolResults does
        // not stall or return VK_NOT_READY.
        CollectResults(frameIndex);

        m_CurrentFrame = frameIndex;

        // Reset query counters for current frame
        m_CurrentTimestampQuery = 0;
        m_CurrentOcclusionQuery = 0;
        m_CurrentPipelineStatsQuery = 0;

        // Clear query info for current frame (track high-water to avoid churn)
        auto& ts = m_TimestampQueries[m_CurrentFrame];
        m_MaxTimestampQueriesSeen = std::max(m_MaxTimestampQueriesSeen, (uint32_t)ts.size());
        ts.clear();
        const uint32_t tsTarget = std::max(m_Config.MaxTimestampQueries, m_MaxTimestampQueriesSeen);
        if (ts.capacity() < tsTarget)
            ts.reserve(tsTarget);

        auto& occ = m_OcclusionQueries[m_CurrentFrame];
        m_MaxOcclusionQueriesSeen = std::max(m_MaxOcclusionQueriesSeen, (uint32_t)occ.size());
        occ.clear();
        const uint32_t occTarget = std::max(m_Config.MaxOcclusionQueries, m_MaxOcclusionQueriesSeen);
        if (occ.capacity() < occTarget)
            occ.reserve(occTarget);

        auto& ps = m_PipelineStatsQueries[m_CurrentFrame];
        m_MaxPipelineStatsQueriesSeen = std::max(m_MaxPipelineStatsQueriesSeen, (uint32_t)ps.size());
        ps.clear();
        const uint32_t psTarget = std::max(m_Config.MaxPipelineStatsQueries, m_MaxPipelineStatsQueriesSeen);
        if (ps.capacity() < psTarget)
            ps.reserve(psTarget);

        // Keep the cached values populated by CollectResults() above — RenderGraph
        // reads them in BeginFrame() before this slot is recorded again. The cache
        // is refreshed the next time this slot completes and CollectResults() runs.

        // Prefer host reset after the fence wait. Devices without hostQueryReset use
        // the command-buffer fallback in ResetQueries().
        if (m_Config.SupportsHostQueryReset)
            ResetTimestampPoolSlice(m_CurrentFrame);
    }

    void VulkanQueryPool::InvalidateCachedTimestampResults(uint32_t frameIndex) {
        if (frameIndex >= m_FramesInFlight)
            return;

        m_CachedTimestampValuesValid[frameIndex] = false;
        m_CachedTimestampValues[frameIndex].clear();
    }

    void VulkanQueryPool::ResetTimestampPoolSlice(uint32_t frameIndex) {
        if (frameIndex >= m_FramesInFlight)
            return;

        if (m_TimestampSupported && m_TimestampPool != VK_NULL_HANDLE) {
            const uint32_t frameOffset = frameIndex * m_Config.MaxTimestampQueries;
            vkResetQueryPool(m_Device, m_TimestampPool, frameOffset, m_Config.MaxTimestampQueries);
        }

        if (m_OcclusionSupported && m_OcclusionPool != VK_NULL_HANDLE) {
            const uint32_t frameOffset = frameIndex * m_Config.MaxOcclusionQueries;
            vkResetQueryPool(m_Device, m_OcclusionPool, frameOffset, m_Config.MaxOcclusionQueries);
        }

        if (m_PipelineStatsSupported && m_PipelineStatsPool != VK_NULL_HANDLE) {
            const uint32_t frameOffset = frameIndex * m_Config.MaxPipelineStatsQueries;
            vkResetQueryPool(m_Device, m_PipelineStatsPool, frameOffset, m_Config.MaxPipelineStatsQueries);
        }
    }

    void VulkanQueryPool::EndFrame() {
        // Update profiling statistics
        m_ProfilingStats.DrawCalls = static_cast<uint32_t>(m_TimestampQueries[m_CurrentFrame].size());
        
        // Calculate frame time from first to last timestamp
        if (!m_TimestampQueries[m_CurrentFrame].empty()) {
            auto& firstQuery = m_TimestampQueries[m_CurrentFrame].front();
            auto& lastQuery = m_TimestampQueries[m_CurrentFrame].back();
            
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                lastQuery.CpuTimestamp - firstQuery.CpuTimestamp);
            m_ProfilingStats.FrameTimeMs = duration.count() / 1000.0;
        }
    }

    uint32_t VulkanQueryPool::WriteTimestamp(CommandList* commandList, TimestampPoint,
                                             const std::string& name) {
        if (!m_TimestampSupported || m_TimestampPool == VK_NULL_HANDLE || !commandList) {
            return UINT32_MAX;
        }

        if (m_CurrentTimestampQuery >= m_Config.MaxTimestampQueries) {
            return UINT32_MAX;
        }
        if (m_CurrentTimestampQuery == 0) {
            InvalidateCachedTimestampResults(m_CurrentFrame);
        }
        const uint32_t queryIndex = GetFrameQueryIndex(m_CurrentTimestampQuery, m_Config.MaxTimestampQueries);

        // Record timestamp query (only if command buffer is valid and recording)
        auto* vkCL = static_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return UINT32_MAX;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }

        // Use a broadly-supported stage for timing markers.
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            m_TimestampPool, queryIndex);

        // Track query info
        QueryInfo info;
        info.NameId = name.empty() ? 0 : HashStringId(name);
        info.Type = QueryType::Timestamp;
        info.FrameIndex = m_CurrentFrame;
        info.CpuTimestamp = std::chrono::high_resolution_clock::now();
        info.Active = true;

        m_TimestampQueries[m_CurrentFrame].push_back(info);
        ++m_CurrentTimestampQuery;
        return queryIndex;
    }

    uint32_t VulkanQueryPool::WriteTimestampRaw(VkCommandBuffer commandBuffer, const std::string& name) {
        if (!m_TimestampSupported || m_TimestampPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }
        if (m_CurrentTimestampQuery >= m_Config.MaxTimestampQueries) {
            return UINT32_MAX;
        }
        if (m_CurrentTimestampQuery == 0) {
            InvalidateCachedTimestampResults(m_CurrentFrame);
        }
        const uint32_t queryIndex = GetFrameQueryIndex(m_CurrentTimestampQuery, m_Config.MaxTimestampQueries);

        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            m_TimestampPool, queryIndex);

        QueryInfo info;
        info.NameId = name.empty() ? 0 : HashStringId(name);
        info.Type = QueryType::Timestamp;
        info.FrameIndex = m_CurrentFrame;
        info.CpuTimestamp = std::chrono::high_resolution_clock::now();
        info.Active = true;

        m_TimestampQueries[m_CurrentFrame].push_back(info);
        ++m_CurrentTimestampQuery;
        return queryIndex;
    }

    uint32_t VulkanQueryPool::BeginOcclusionQuery(CommandList* commandList, const std::string& name) {
        if (!m_OcclusionSupported || m_OcclusionPool == VK_NULL_HANDLE || !commandList) {
            return UINT32_MAX;
        }

        if (m_CurrentOcclusionQuery >= m_Config.MaxOcclusionQueries) {
            return UINT32_MAX;
        }
        uint32_t queryIndex = GetFrameQueryIndex(m_CurrentOcclusionQuery, m_Config.MaxOcclusionQueries);

        // Begin occlusion query (only if command buffer is valid and recording)
        VulkanCommandList* vkCL = dynamic_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return UINT32_MAX;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }

        vkCmdBeginQuery(commandBuffer, m_OcclusionPool, queryIndex, 0);

        // Track query info
        QueryInfo info;
        info.NameId = name.empty() ? 0 : HashStringId(name);
        info.Type = QueryType::Occlusion;
        info.FrameIndex = m_CurrentFrame;
        info.CpuTimestamp = std::chrono::high_resolution_clock::now();
        info.Active = true;

        m_OcclusionQueries[m_CurrentFrame].push_back(info);
        return m_CurrentOcclusionQuery++;
    }

    void VulkanQueryPool::EndOcclusionQuery(CommandList* commandList, uint32_t queryIndex) {
        if (!m_OcclusionSupported || m_OcclusionPool == VK_NULL_HANDLE || queryIndex == UINT32_MAX || !commandList) {
            return;
        }

        uint32_t frameQueryIndex = GetFrameQueryIndex(queryIndex, m_Config.MaxOcclusionQueries);
        VulkanCommandList* vkCL = dynamic_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return;
        }

        vkCmdEndQuery(commandBuffer, m_OcclusionPool, frameQueryIndex);
    }

    uint32_t VulkanQueryPool::BeginPipelineStats(CommandList* commandList, const std::string& name) {
        if (!m_PipelineStatsSupported || m_PipelineStatsPool == VK_NULL_HANDLE || !commandList) {
            return UINT32_MAX;
        }

        if (m_CurrentPipelineStatsQuery >= m_Config.MaxPipelineStatsQueries) {
            return UINT32_MAX;
        }
        uint32_t queryIndex = GetFrameQueryIndex(m_CurrentPipelineStatsQuery, m_Config.MaxPipelineStatsQueries);

        // Begin pipeline statistics query (only if command buffer is valid and recording)
        VulkanCommandList* vkCL = dynamic_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return UINT32_MAX;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }

        vkCmdBeginQuery(commandBuffer, m_PipelineStatsPool, queryIndex, 0);

        // Track query info
        QueryInfo info;
        info.NameId = name.empty() ? 0 : HashStringId(name);
        info.Type = QueryType::PipelineStats;
        info.FrameIndex = m_CurrentFrame;
        info.CpuTimestamp = std::chrono::high_resolution_clock::now();
        info.Active = true;

        m_PipelineStatsQueries[m_CurrentFrame].push_back(info);
        return m_CurrentPipelineStatsQuery++;
    }

    void VulkanQueryPool::EndPipelineStats(CommandList* commandList, uint32_t queryIndex) {
        if (!m_PipelineStatsSupported || m_PipelineStatsPool == VK_NULL_HANDLE || queryIndex == UINT32_MAX || !commandList) {
            return;
        }

        uint32_t frameQueryIndex = GetFrameQueryIndex(queryIndex, m_Config.MaxPipelineStatsQueries);
        VulkanCommandList* vkCL = dynamic_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return;
        }

        vkCmdEndQuery(commandBuffer, m_PipelineStatsPool, frameQueryIndex);
    }



    double VulkanQueryPool::TimestampToMs(uint64_t timestamp) const {
        return (timestamp * m_TimestampPeriod) / 1000000.0; // Convert ns to ms
    }

    std::vector<QueryResult> VulkanQueryPool::GetAllTimestampResults() const {
        std::vector<QueryResult> results;
        // Implementation would collect all available timestamp results
        return results;
    }

    void VulkanQueryPool::ResetQueries(CommandList* commandList) {
        if (m_Config.SupportsHostQueryReset) {
            // Query slices were reset on the host in BeginFrame() after the
            // per-frame fence signaled.
            return;
        }
        if (!commandList) {
            return;
        }

        auto* vkCL = dynamic_cast<VulkanCommandList*>(commandList);
        if (!vkCL || !vkCL->IsRecording()) {
            return;
        }
        VkCommandBuffer commandBuffer = vkCL->GetVkCommandBuffer();
        if (commandBuffer == VK_NULL_HANDLE) {
            return;
        }

        uint32_t frameOffset = m_CurrentFrame * m_Config.MaxTimestampQueries;

        if (m_TimestampSupported && m_TimestampPool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(commandBuffer, m_TimestampPool, frameOffset, m_Config.MaxTimestampQueries);
            m_CurrentTimestampQuery = 0;
        }

        if (m_OcclusionSupported && m_OcclusionPool != VK_NULL_HANDLE) {
            frameOffset = m_CurrentFrame * m_Config.MaxOcclusionQueries;
            vkCmdResetQueryPool(commandBuffer, m_OcclusionPool, frameOffset, m_Config.MaxOcclusionQueries);
            m_CurrentOcclusionQuery = 0;
        }

        if (m_PipelineStatsSupported && m_PipelineStatsPool != VK_NULL_HANDLE) {
            frameOffset = m_CurrentFrame * m_Config.MaxPipelineStatsQueries;
            vkCmdResetQueryPool(commandBuffer, m_PipelineStatsPool, frameOffset, m_Config.MaxPipelineStatsQueries);
            m_CurrentPipelineStatsQuery = 0;
        }
    }

    bool VulkanQueryPool::IsValid() const {
        return (m_TimestampSupported && m_TimestampPool != VK_NULL_HANDLE) ||
               (m_OcclusionSupported && m_OcclusionPool != VK_NULL_HANDLE) ||
               (m_PipelineStatsSupported && m_PipelineStatsPool != VK_NULL_HANDLE);
    }

    void VulkanQueryPool::DebugPrintResults() const {
        // Quiet mode: no stdout debug dump in release/CI runs
    }

    bool VulkanQueryPool::CreateQueryPools() {
        bool success = true;

        // Create timestamp query pool
        if (m_TimestampSupported) {
            VkQueryPoolCreateInfo timestampPoolInfo{};
            timestampPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            timestampPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            timestampPoolInfo.queryCount = m_Config.MaxTimestampQueries * m_FramesInFlight;

            VkResult result = vkCreateQueryPool(m_Device, &timestampPoolInfo, nullptr, &m_TimestampPool);
            if (result != VK_SUCCESS) {
                std::cerr << "Failed to create timestamp query pool: " << result << std::endl;
                success = false;
            } else {

            }
        }

        // Create occlusion query pool
        if (m_OcclusionSupported) {
            VkQueryPoolCreateInfo occlusionPoolInfo{};
            occlusionPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            occlusionPoolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
            occlusionPoolInfo.queryCount = m_Config.MaxOcclusionQueries * m_FramesInFlight;

            VkResult result = vkCreateQueryPool(m_Device, &occlusionPoolInfo, nullptr, &m_OcclusionPool);
            if (result != VK_SUCCESS) {
                std::cerr << "Failed to create occlusion query pool: " << result << std::endl;
                success = false;
            } else {

            }
        }

        // Create pipeline statistics query pool
        if (m_PipelineStatsSupported) {
            VkQueryPoolCreateInfo pipelineStatsPoolInfo{};
            pipelineStatsPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            pipelineStatsPoolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            pipelineStatsPoolInfo.queryCount = m_Config.MaxPipelineStatsQueries * m_FramesInFlight;
            pipelineStatsPoolInfo.pipelineStatistics =
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;

            VkResult result = vkCreateQueryPool(m_Device, &pipelineStatsPoolInfo, nullptr, &m_PipelineStatsPool);
            if (result != VK_SUCCESS) {
                std::cerr << "Failed to create pipeline statistics query pool: " << result << std::endl;
                success = false;
            } else {

            }
        }

        return success;
    }

    void VulkanQueryPool::DestroyQueryPools() {
        if (m_TimestampPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_Device, m_TimestampPool, nullptr);
            m_TimestampPool = VK_NULL_HANDLE;
        }

        if (m_OcclusionPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_Device, m_OcclusionPool, nullptr);
            m_OcclusionPool = VK_NULL_HANDLE;
        }

        if (m_PipelineStatsPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_Device, m_PipelineStatsPool, nullptr);
            m_PipelineStatsPool = VK_NULL_HANDLE;
        }
    }

    bool VulkanQueryPool::CheckQuerySupport() {
        // Get physical device properties
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(m_PhysicalDevice, &deviceProperties);

        // Timestamp support:
        // `timestampComputeAndGraphics` is a conservative flag (all graphics+compute queues).
        // For profiling, we can use timestamps as long as *some* queue family supports them.
        m_TimestampPeriod = deviceProperties.limits.timestampPeriod;
        bool anyQueueFamilySupportsTimestamps = false;
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &qCount, nullptr);
        if (qCount > 0)
        {
            std::vector<VkQueueFamilyProperties> qProps(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &qCount, qProps.data());
            for (const auto& q : qProps)
            {
                if (q.timestampValidBits > 0)
                {
                    anyQueueFamilySupportsTimestamps = true;
                    break;
                }
            }
        }
        m_TimestampSupported = anyQueueFamilySupportsTimestamps && (m_TimestampPeriod > 0.0);

        // Check occlusion query support (generally supported on all devices)
        m_OcclusionSupported = true;

        // Check pipeline statistics support (generally supported on all devices)
        m_PipelineStatsSupported = true;

        // Get physical device features for additional validation
        VkPhysicalDeviceFeatures deviceFeatures;
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &deviceFeatures);

        // Occlusion queries require occlusionQueryPrecise feature
        if (!deviceFeatures.occlusionQueryPrecise) {
            m_OcclusionSupported = false;
        }

        // Pipeline statistics require pipelineStatisticsQuery feature
        if (!deviceFeatures.pipelineStatisticsQuery) {
            m_PipelineStatsSupported = false;
        }

        return true; // Always return true - we can work with whatever is supported
    }

    void VulkanQueryPool::CollectResults(uint32_t frameIndex) {
        if (frameIndex >= m_FramesInFlight)
            return;

        // Collect timestamp results for the completed frame slot.
        if (m_TimestampSupported && !m_TimestampQueries[frameIndex].empty()) {
            auto& timestampResults = m_CachedTimestampValues[frameIndex];
            timestampResults.resize(m_TimestampQueries[frameIndex].size());
            const uint32_t frameOffset = frameIndex * m_Config.MaxTimestampQueries;

            VkResult result = vkGetQueryPoolResults(
                m_Device, m_TimestampPool, frameOffset,
                static_cast<uint32_t>(timestampResults.size()),
                timestampResults.size() * sizeof(uint64_t), timestampResults.data(),
                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

            if (result == VK_SUCCESS) {
                // Cache raw timestamp values for this completed frame slice.
                m_CachedTimestampValuesValid[frameIndex] = true;

                // Calculate GPU time from first to last timestamp
                if (timestampResults.size() >= 2) {
                    const uint64_t startTime = timestampResults[0];
                    const uint64_t endTime = timestampResults[timestampResults.size() - 1];
                    m_ProfilingStats.GpuTimeMs = TimestampToMs(endTime - startTime);
                }
            }
        }

        // TODO: Collect occlusion and pipeline statistics results
        // This would follow similar patterns but with different data structures
    }

    uint32_t VulkanQueryPool::GetFrameQueryIndex(uint32_t baseIndex, uint32_t perFrameMax) const {
        return m_CurrentFrame * perFrameMax + baseIndex;
    }

    VkCommandBuffer VulkanQueryPool::GetVkCommandBuffer(CommandList* commandList) const {
        // Cast to VulkanCommandList to get the underlying VkCommandBuffer
        auto* vulkanCommandList = static_cast<VulkanCommandList*>(commandList);
        return vulkanCommandList->GetVkCommandBuffer();
    }

    // Missing interface methods implementation
    bool VulkanQueryPool::GetTimestampResult(uint32_t queryIndex, QueryResult& outResult) {
        outResult = QueryResult{};
        outResult.Type = QueryType::Timestamp;

        if (!m_TimestampSupported || m_TimestampPool == VK_NULL_HANDLE || queryIndex == UINT32_MAX) {
            return false;
        }

        // Fast-path: if this query belongs to a cached completed frame slice, serve it from cache.
        const uint32_t frame = queryIndex / m_Config.MaxTimestampQueries;
        const uint32_t idxInFrame = queryIndex % m_Config.MaxTimestampQueries;
        if (frame < m_FramesInFlight && m_CachedTimestampValuesValid[frame])
        {
            const auto& cached = m_CachedTimestampValues[frame];
            if (idxInFrame < cached.size())
            {
                outResult.Value = cached[idxInFrame];
                outResult.Available = true;
                return true;
            }
        }

        // Query one timestamp (non-blocking; returns VK_NOT_READY until available).
        uint64_t timestamp = 0;
        VkResult result = vkGetQueryPoolResults(
            m_Device,
            m_TimestampPool,
            queryIndex,
            1,
            sizeof(timestamp),
            &timestamp,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);

        if (result != VK_SUCCESS) {
            outResult.Available = false;
            return false;
        }

        outResult.Value = timestamp;
        outResult.Available = true;
        return true;
    }

    bool VulkanQueryPool::GetOcclusionResult(uint32_t queryIndex, QueryResult& outResult) {
        // Implementation would retrieve occlusion query result
        outResult.Available = false;
        return false;
    }

    bool VulkanQueryPool::GetPipelineStatsResult(uint32_t queryIndex, QueryResult& outResult) {
        // Implementation would retrieve pipeline statistics result
        outResult.Available = false;
        return false;
    }

    const VulkanQueryPool::ProfilingStats& VulkanQueryPool::GetProfilingStats() const {
        return m_ProfilingStats;
    }

    double VulkanQueryPool::GetTimestampPeriod() const {
        return m_TimestampPeriod;
    }

} // namespace Rendering
} // namespace GameEngine
