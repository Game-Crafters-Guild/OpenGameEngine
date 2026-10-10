#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Rendering {

    // Forward declarations
    class CommandList;

    /**
     * @brief Query types supported by the system
     */
    enum class QueryType {
        Timestamp,      // GPU timestamp queries
        Occlusion,      // Occlusion queries
        PipelineStats   // Pipeline statistics
    };

    /**
     * @brief Which end of a timing span a timestamp marks.
     *
     * Backends that can only sample at coarse boundaries (Metal: encoder stage
     * boundaries) need to know which end they are placing, because the nearest
     * samplable point differs: a span's begin belongs at the START of the work
     * unit it opens, its end at that unit's END. Backends with per-command
     * timestamps ignore it.
     */
    enum class TimestampPoint : uint8_t {
        SpanBegin,
        SpanEnd
    };

    /**
     * @brief What the interval between a span's two timestamps actually measures.
     *
     * PipelinePoint - the timestamps are per-command pipeline points, so the
     *   interval is the bracketed commands' own execution (Vulkan).
     * EncoderSpan - the finest granularity the device can sample is a GPU work
     *   unit (a Metal command encoder). The value is the summed span of the
     *   unit(s) the bracketed work created (see GetTimestampSpanTicks), which
     *   (a) still charges whatever else shares those units - one encoder can
     *   carry several passes, reported identically to each, (b) overlaps
     *   neighbouring units because the GPU runs them concurrently. Such values
     *   are upper bounds on cost and are NOT additive across passes.
     */
    enum class TimestampSemantics : uint8_t {
        PipelinePoint,
        EncoderSpan
    };

    /**
     * @brief Query result data
     */
    struct QueryResult {
        uint64_t Value = 0;
        bool Available = false;
        std::string Name;
        QueryType Type;
        std::chrono::high_resolution_clock::time_point CpuTimestamp;
    };

    /**
     * @brief Abstract GPU profiling and query management interface
     *
     * Provides comprehensive GPU query support for performance profiling,
     * occlusion testing, and pipeline statistics collection.
     *
     * Features:
     * - Timestamp queries for GPU profiling
     * - Occlusion queries for visibility testing
     * - Pipeline statistics for performance analysis
     * - Automatic query pool management
     * - Frame-based query cycling
     * - CPU/GPU time correlation
     * - Cross-platform abstraction (Vulkan, DirectX 12)
     */
    class IQueryPool {
    public:
        /**
         * @brief Query pool configuration
         */
        struct Config {
            uint32_t MaxTimestampQueries = 1024;
            uint32_t MaxOcclusionQueries = 256;
            uint32_t MaxPipelineStatsQueries = 64;
            uint32_t FramesInFlight = 3;
            bool EnableValidation = true;
            bool SupportsHostQueryReset = false;
        };

        /**
         * @brief Profiling statistics
         */
        struct ProfilingStats {
            double FrameTimeMs = 0.0;
            double GpuTimeMs = 0.0;
            uint32_t DrawCalls = 0;
            uint32_t ComputeDispatches = 0;
            uint64_t VerticesProcessed = 0;
            uint64_t FragmentsProcessed = 0;
        };

        virtual ~IQueryPool() = default;

    protected:
        // Protected constructor - only derived classes can instantiate
        IQueryPool() = default;

    public:
        // Non-copyable, non-movable
        IQueryPool(const IQueryPool&) = delete;
        IQueryPool& operator=(const IQueryPool&) = delete;
        IQueryPool(IQueryPool&&) = delete;
        IQueryPool& operator=(IQueryPool&&) = delete;

        /**
         * @brief Initialize the query pools
         */
        virtual bool Initialize() = 0;

        /**
         * @brief Shutdown and cleanup
         */
        virtual void Shutdown() = 0;

        /**
         * @brief Begin a new frame for the given device frame slot.
         *
         * Callers must wait the per-frame fence for `frameIndex` before invoking this.
         * Completed GPU timestamps for that slot are read back before the slot is reset.
         */
        virtual void BeginFrame(uint32_t frameIndex) = 0;

        /**
         * @brief Discard cached timestamp readback for a completed frame slot.
         *
         * RenderGraph consumes cached timestamps during BeginFrame(). After that,
         * leaving the cache valid lets GetTimestampResult() serve stale values once
         * the slot is reset and query indices are reused.
         */
        virtual void InvalidateCachedTimestampResults(uint32_t frameIndex) = 0;

        /**
         * @brief End the current frame and collect results
         */
        virtual void EndFrame() = 0;

        /**
         * @brief Sentinel returned by GetTimestampUnitId when the backend has no
         *        coarser work unit than the timestamp itself.
         */
        static constexpr uint32_t kInvalidTimestampUnit = ~0u;

        /**
         * @brief Write one end of a GPU timing span
         * @param commandList Command list to record into
         * @param point Which end of the span this timestamp marks
         * @param name Debug name for the timestamp
         * @return Query index for later retrieval
         */
        virtual uint32_t WriteTimestamp(CommandList* commandList, TimestampPoint point,
                                        const std::string& name = "") = 0;

        /**
         * @brief What an interval between two of this pool's timestamps measures.
         */
        virtual TimestampSemantics GetTimestampSemantics() const = 0;

        /**
         * @brief GPU ticks the work units between a span's two timestamps were
         *        BUSY, excluding the gaps between them.
         *
         * A backend that can only sample at work-unit boundaries times a span
         * as the extent from its first unit's start to its last unit's end,
         * which folds in however long the GPU waited between those units. Where
         * the backend can account per unit it reports the sum of the units'
         * own spans here instead, which is the tighter bound. False when the
         * backend has no unit accounting (PipelinePoint), leaving the caller
         * with the plain timestamp difference.
         */
        virtual bool GetTimestampSpanTicks(uint32_t beginQuery, uint32_t endQuery,
                                           uint64_t& outTicks) const = 0;

        /**
         * @brief Identity of the GPU work unit a timestamp was sampled at.
         *
         * Two spans that report the same pair of unit ids were sampled at the
         * same boundaries and cannot be told apart - their intervals are one
         * measurement, not two. kInvalidTimestampUnit under PipelinePoint
         * semantics, where every timestamp is its own point.
         */
        virtual uint32_t GetTimestampUnitId(uint32_t queryIndex) const = 0;

        /**
         * @brief Begin an occlusion query
         * @param commandList Command list to record into
         * @param name Debug name for the query
         * @return Query index for later retrieval
         */
        virtual uint32_t BeginOcclusionQuery(CommandList* commandList, const std::string& name = "") = 0;

        /**
         * @brief End an occlusion query
         * @param commandList Command list to record into
         * @param queryIndex Query index from BeginOcclusionQuery
         */
        virtual void EndOcclusionQuery(CommandList* commandList, uint32_t queryIndex) = 0;

        /**
         * @brief Begin pipeline statistics collection
         * @param commandList Command list to record into
         * @param name Debug name for the statistics
         * @return Query index for later retrieval
         */
        virtual uint32_t BeginPipelineStats(CommandList* commandList, const std::string& name = "") = 0;

        /**
         * @brief End pipeline statistics collection
         * @param commandList Command list to record into
         * @param queryIndex Query index from BeginPipelineStats
         */
        virtual void EndPipelineStats(CommandList* commandList, uint32_t queryIndex) = 0;

        /**
         * @brief Get timestamp query result
         * @param queryIndex Query index
         * @param outResult Output result
         * @return true if result is available
         */
        virtual bool GetTimestampResult(uint32_t queryIndex, QueryResult& outResult) = 0;

        /**
         * @brief Get occlusion query result
         * @param queryIndex Query index
         * @param outResult Output result
         * @return true if result is available
         */
        virtual bool GetOcclusionResult(uint32_t queryIndex, QueryResult& outResult) = 0;

        /**
         * @brief Get pipeline statistics result
         * @param queryIndex Query index
         * @param outResult Output result
         * @return true if result is available
         */
        virtual bool GetPipelineStatsResult(uint32_t queryIndex, QueryResult& outResult) = 0;

        /**
         * @brief Get current frame profiling statistics
         */
        virtual const ProfilingStats& GetProfilingStats() const = 0;

        /**
         * @brief Get GPU timestamp period in nanoseconds
         */
        virtual double GetTimestampPeriod() const = 0;

        /**
         * @brief Convert GPU timestamp to milliseconds
         */
        virtual double TimestampToMs(uint64_t timestamp) const = 0;

        /**
         * @brief Get all timestamp results for the current frame
         */
        virtual std::vector<QueryResult> GetAllTimestampResults() const = 0;

        /**
         * @brief Reset all queries for the current frame
         */
        virtual void ResetQueries(CommandList* commandList) = 0;

        /**
         * @brief Check if query pools are valid
         */
        virtual bool IsValid() const = 0;

        /**
         * @brief Debug print query results
         */
        virtual void DebugPrintResults() const = 0;

    };

} // namespace Rendering
} // namespace GameEngine
