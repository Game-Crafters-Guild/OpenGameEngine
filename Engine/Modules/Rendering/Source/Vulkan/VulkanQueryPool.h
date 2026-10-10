/**
 * @file VulkanQueryPool.h
 * @brief Vulkan implementation of GPU query system for performance profiling
 */

#pragma once

#include "Rendering/Core/QueryPool.h"
#include "Types/StringId.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>

namespace GameEngine {
namespace Rendering {

    // Forward declarations
    class VulkanCommandList;

    /**
     * @brief Vulkan implementation of GPU profiling and query management
     * 
     * Provides comprehensive GPU query support for performance profiling,
     * occlusion testing, and pipeline statistics collection using Vulkan API.
     * 
     * Features:
     * - Timestamp queries for GPU profiling
     * - Occlusion queries for visibility testing
     * - Pipeline statistics for performance analysis
     * - Automatic query pool management
     * - Frame-based query cycling
     * - CPU/GPU time correlation
     */
    class VulkanQueryPool : public IQueryPool {
    public:
        VulkanQueryPool(VkDevice device, VkPhysicalDevice physicalDevice, const Config& config = {});
        ~VulkanQueryPool() override;

        // Non-copyable, non-movable
        VulkanQueryPool(const VulkanQueryPool&) = delete;
        VulkanQueryPool& operator=(const VulkanQueryPool&) = delete;
        VulkanQueryPool(VulkanQueryPool&&) = delete;
        VulkanQueryPool& operator=(VulkanQueryPool&&) = delete;

        // IQueryPool interface implementation
        bool Initialize() override;
        void Shutdown() override;

        // Frame management
        void BeginFrame(uint32_t frameIndex) override;
        void InvalidateCachedTimestampResults(uint32_t frameIndex) override;
        void EndFrame() override;

        // Query operations
        uint32_t WriteTimestamp(CommandList* commandList, TimestampPoint point,
                                const std::string& name = "") override;
        // vkCmdWriteTimestamp is a pipeline point, so a span's two ends need no
        // different treatment and TimestampPoint is unused here.
        TimestampSemantics GetTimestampSemantics() const override
        {
            return TimestampSemantics::PipelinePoint;
        }
        uint32_t GetTimestampUnitId(uint32_t) const override { return kInvalidTimestampUnit; }
        bool GetTimestampSpanTicks(uint32_t, uint32_t, uint64_t&) const override { return false; }
        // Vulkan-specific raw-cmd-buffer variant used by the device's per-frame
        // bracket (the present-transition cmd buffer doesn't go through a
        // VulkanCommandList wrapper). Behaves identically to WriteTimestamp:
        // claims a slot in the current frame's slice, records vkCmdWriteTimestamp,
        // returns the query index (UINT32_MAX if the slot cap is hit).
        uint32_t WriteTimestampRaw(VkCommandBuffer cmd, const std::string& name = "");
        uint32_t BeginOcclusionQuery(CommandList* commandList, const std::string& name = "") override;
        void EndOcclusionQuery(CommandList* commandList, uint32_t queryIndex) override;
        uint32_t BeginPipelineStats(CommandList* commandList, const std::string& name = "") override;
        void EndPipelineStats(CommandList* commandList, uint32_t queryIndex) override;

        // Result retrieval
        bool GetTimestampResult(uint32_t queryIndex, QueryResult& outResult) override;
        bool GetOcclusionResult(uint32_t queryIndex, QueryResult& outResult) override;
        bool GetPipelineStatsResult(uint32_t queryIndex, QueryResult& outResult) override;

        // Statistics and utilities
        const ProfilingStats& GetProfilingStats() const override;
        double GetTimestampPeriod() const override;
        double TimestampToMs(uint64_t timestamp) const override;
        std::vector<QueryResult> GetAllTimestampResults() const override;
        void ResetQueries(CommandList* commandList) override;
        bool IsValid() const override;
        void DebugPrintResults() const override;

    private:
        VkDevice m_Device;
        VkPhysicalDevice m_PhysicalDevice;
        Config m_Config;

        // Query pools for different types
        VkQueryPool m_TimestampPool = VK_NULL_HANDLE;
        VkQueryPool m_OcclusionPool = VK_NULL_HANDLE;
        VkQueryPool m_PipelineStatsPool = VK_NULL_HANDLE;

        // Frame management
        uint32_t m_CurrentFrame = 0;
        uint32_t m_FramesInFlight;

        // Query tracking
        struct QueryInfo {
            StringId NameId = 0;
            QueryType Type;
            uint32_t FrameIndex;
            std::chrono::high_resolution_clock::time_point CpuTimestamp;
            bool Active = false;
        };

        std::vector<std::vector<QueryInfo>> m_TimestampQueries;  // Per frame
        std::vector<std::vector<QueryInfo>> m_OcclusionQueries;  // Per frame
        std::vector<std::vector<QueryInfo>> m_PipelineStatsQueries;  // Per frame

        // Cached timestamp values (raw GPU ticks) for recently completed frames.
        // This lets higher-level systems query per-pass timings without stalling
        // or issuing many vkGetQueryPoolResults calls.
        std::vector<std::vector<uint64_t>> m_CachedTimestampValues; // Per frame
        std::vector<bool> m_CachedTimestampValuesValid;             // Per frame

        // Current frame query counters
        uint32_t m_CurrentTimestampQuery = 0;
        uint32_t m_CurrentOcclusionQuery = 0;
        uint32_t m_CurrentPipelineStatsQuery = 0;

        // Device properties
        double m_TimestampPeriod = 1.0;
        bool m_TimestampSupported = false;
        bool m_OcclusionSupported = false;
        bool m_PipelineStatsSupported = false;

        // Statistics
        ProfilingStats m_ProfilingStats;

        // Private implementation methods
        bool CreateQueryPools();
        void DestroyQueryPools();
        bool CheckQuerySupport();
        void CollectResults(uint32_t frameIndex);
        void ResetTimestampPoolSlice(uint32_t frameIndex);
        uint32_t GetFrameQueryIndex(uint32_t baseIndex, uint32_t perFrameMax) const;

        // Helper method to get VkCommandBuffer from CommandList
        VkCommandBuffer GetVkCommandBuffer(CommandList* commandList) const;

        // High-water marks to avoid per-frame reallocation churn
        uint32_t m_MaxTimestampQueriesSeen = 0;
        uint32_t m_MaxOcclusionQueriesSeen = 0;
        uint32_t m_MaxPipelineStatsQueriesSeen = 0;
    };

} // namespace Rendering
} // namespace GameEngine
