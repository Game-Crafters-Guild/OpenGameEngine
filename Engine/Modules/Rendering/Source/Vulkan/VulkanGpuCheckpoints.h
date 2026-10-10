/**
 * @file VulkanGpuCheckpoints.h
 * @brief GPU-side execution breadcrumbs via VK_NV_device_diagnostic_checkpoints.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Rendering {

    /// Parse the GE_VK_GPU_CHECKPOINTS toggle. Split out from the env read so the
    /// policy is unit-testable. OFF unless explicitly requested: emission adds a
    /// GPU command per marker and the marker sites sit on per-frame paths. Same
    /// leading-character idiom as GE_VK_USE_DESCRIPTOR_BUFFER.
    inline bool ParseGpuCheckpointsEnabled(const char* envValue) noexcept
    {
        return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0' &&
               envValue[0] != 'f' && envValue[0] != 'F';
    }

    /// Marker names are stored verbatim up to this length (including the
    /// terminator); longer names are truncated to fit the fixed-size record.
    constexpr size_t kMaxGpuCheckpointNameLength = 64;

    /// Distinct names the table will hold before degrading to a shared overflow
    /// record. Marker names come from a closed set of pass/kernel names, so this
    /// is a guard against a caller synthesizing names per frame, not a budget.
    constexpr size_t kMaxGpuCheckpointNames = 4096;

    /**
     * @brief Interns marker names into permanently-addressed records usable as
     *        vkCmdSetCheckpointNV payloads.
     *
     * Payload lifetime is the whole reason this type exists. vkCmdSetCheckpointNV
     * stores an opaque pointer that the driver hands back from
     * vkGetQueueCheckpointDataNV *after* the device is lost, so the pointed-to
     * storage has to outlive the command buffer, the submission and the loss
     * itself. Records are therefore carved from heap blocks that are allocated
     * once and never moved, resized or freed while the table lives: interning
     * only appends, and appending allocates a NEW block instead of growing an
     * existing one, so pointers handed out earlier keep their addresses forever.
     * Only the vector of block owners reallocates, which moves those owning
     * pointers and not the blocks they point at.
     *
     * Thread-safe: marker recording runs on every thread that records command
     * buffers. Steady state is a shared-lock hash lookup; only a first-sighting
     * name takes the exclusive lock.
     */
    class GpuCheckpointNameTable
    {
    public:
        /// Intern @p name and return the stable payload pointer for it, bumping
        /// the recorded-marker ordinal. Null only for a null/empty name (the
        /// caller then skips emission); a full table degrades to a shared
        /// overflow record so the checkpoint still records that execution
        /// reached this point.
        const void* Intern(const char* name);

        /// Resolve a payload the driver returned. Rejects anything that is not
        /// one of this table's record addresses, so a driver handing back a
        /// bogus pointer yields "unrecognized" instead of a wild read inside the
        /// device-lost handler.
        bool Describe(const void* payload,
                      std::string_view& outName,
                      uint64_t& outLastRecordedOrdinal) const;

        /// Total markers recorded (payloads handed out). The gap between this and
        /// the ordinals the GPU reports brackets the faulting region.
        uint64_t RecordedCount() const noexcept
        {
            return m_RecordedCount.load(std::memory_order_relaxed);
        }

        size_t DistinctNameCount() const;

        /// True once a name was dropped to the overflow record.
        bool Overflowed() const noexcept { return m_Overflowed.load(std::memory_order_relaxed); }

    private:
        struct Record
        {
            /// Address handed to the driver — fixed for the table's lifetime.
            char Name[kMaxGpuCheckpointNameLength] = {};
            /// Ordinal of the most recent time this NAME was recorded. Not
            /// necessarily the instance the GPU executed, but an upper bound on
            /// it, which is what makes the CPU/GPU gap readable.
            std::atomic<uint64_t> LastRecordedOrdinal{0};
        };

        static constexpr size_t kRecordsPerBlock = 128;
        /// std::array, not std::vector: a Record holds an atomic and is therefore
        /// immovable, and a fixed-extent block cannot be resized into moving the
        /// records the driver already points at.
        using Block = std::array<Record, kRecordsPerBlock>;

        Record* AllocateRecordLocked();
        const Record* ResolveLocked(const void* payload) const;

        mutable std::shared_mutex m_Mutex;
        std::vector<std::unique_ptr<Block>> m_Blocks;
        std::unordered_map<std::string_view, Record*> m_ByName;
        size_t m_RecordCount = 0;
        std::unique_ptr<Record> m_Overflow;
        std::atomic<uint64_t> m_RecordedCount{0};
        std::atomic<bool> m_Overflowed{false};
    };

    /// Query each distinct queue for the checkpoints the GPU last executed and
    /// format them for the device-lost log, oldest first / newest last to match
    /// the CPU marker ring's convention. Null and duplicate queue handles are
    /// skipped. Returns a bracketed placeholder rather than an empty string so
    /// the log line always says something.
    ///
    /// An empty result is reported as one of two DIFFERENT things, because
    /// collapsing them makes an instrument limit look like a finding: a family
    /// that cannot report checkpoints at all is blind, while a supporting family
    /// with no data reports no *outstanding* marked work — drained, or nothing
    /// emitted there. Neither says anything about what executed.
    ///
    /// @param queueNames Parallel to @p queues; labels for the log.
    /// @param queueCheckpointStages Parallel to @p queues; each queue family's
    ///        VkQueueFamilyCheckpointPropertiesNV::checkpointExecutionStageMask.
    ///        Zero means the family reports no checkpoint stages, so both
    ///        vkCmdSetCheckpointNV and retrieval are meaningless on it.
    std::string FormatExecutedGpuCheckpoints(PFN_vkGetQueueCheckpointDataNV getCheckpointData,
                                            const VkQueue* queues,
                                            const char* const* queueNames,
                                            const VkPipelineStageFlags* queueCheckpointStages,
                                            uint32_t queueCount,
                                            const GpuCheckpointNameTable& names);

} // namespace Rendering
} // namespace GameEngine
