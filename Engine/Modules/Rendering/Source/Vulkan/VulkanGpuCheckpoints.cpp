#include "VulkanGpuCheckpoints.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace GameEngine {
namespace Rendering {

namespace
{
// Copy `name` into a fixed buffer, truncating to fit, and return the length
// written (excluding the terminator). Truncation happens before the table lookup
// so a key and the name stored under it can never disagree.
size_t CopyTruncatedName(char (&dst)[kMaxGpuCheckpointNameLength], const char* name)
{
    size_t length = 0;
    while (length + 1 < kMaxGpuCheckpointNameLength && name[length] != '\0')
    {
        dst[length] = name[length];
        ++length;
    }
    dst[length] = '\0';
    return length;
}

// Short label for the pipeline stage a checkpoint was reported against. Only the
// stages a checkpoint can realistically land on are named; anything else prints
// as its bit value rather than being silently dropped.
std::string PipelineStageLabel(VkPipelineStageFlagBits stage)
{
    switch (stage)
    {
    case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:                     return "TOP_OF_PIPE";
    case VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT:                   return "DRAW_INDIRECT";
    case VK_PIPELINE_STAGE_VERTEX_INPUT_BIT:                    return "VERTEX_INPUT";
    case VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:                   return "VERTEX_SHADER";
    case VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT:     return "TESS_CONTROL";
    case VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT:  return "TESS_EVAL";
    case VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT:                 return "GEOMETRY_SHADER";
    case VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:                 return "FRAGMENT_SHADER";
    case VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT:            return "EARLY_FRAGMENT_TESTS";
    case VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT:             return "LATE_FRAGMENT_TESTS";
    case VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:         return "COLOR_ATTACHMENT_OUTPUT";
    case VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:                  return "COMPUTE_SHADER";
    case VK_PIPELINE_STAGE_TRANSFER_BIT:                        return "TRANSFER";
    case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:                  return "BOTTOM_OF_PIPE";
    case VK_PIPELINE_STAGE_HOST_BIT:                            return "HOST";
    case VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT:                    return "ALL_GRAPHICS";
    case VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:                    return "ALL_COMMANDS";
    default:                                                    break;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "stage=0x%x", static_cast<unsigned>(stage));
    return buffer;
}
} // namespace

const void* GpuCheckpointNameTable::Intern(const char* name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return nullptr;
    }

    const uint64_t ordinal = m_RecordedCount.fetch_add(1, std::memory_order_relaxed) + 1;

    char truncated[kMaxGpuCheckpointNameLength];
    const size_t length = CopyTruncatedName(truncated, name);
    const std::string_view key(truncated, length);

    {
        std::shared_lock<std::shared_mutex> read(m_Mutex);
        const auto it = m_ByName.find(key);
        if (it != m_ByName.end())
        {
            it->second->LastRecordedOrdinal.store(ordinal, std::memory_order_relaxed);
            return it->second;
        }
    }

    std::unique_lock<std::shared_mutex> write(m_Mutex);
    // Another thread may have interned this name between the two locks.
    const auto it = m_ByName.find(key);
    if (it != m_ByName.end())
    {
        it->second->LastRecordedOrdinal.store(ordinal, std::memory_order_relaxed);
        return it->second;
    }

    Record* record = AllocateRecordLocked();
    if (record == nullptr)
    {
        // Table full: still emit, against a shared record, so the checkpoint
        // proves execution reached a marker even though the name is unavailable.
        if (!m_Overflow)
        {
            m_Overflow = std::make_unique<Record>();
            CopyTruncatedName(m_Overflow->Name, "<checkpoint-name-table-full>");
        }
        m_Overflow->LastRecordedOrdinal.store(ordinal, std::memory_order_relaxed);
        m_Overflowed.store(true, std::memory_order_relaxed);
        return m_Overflow.get();
    }

    std::memcpy(record->Name, truncated, length + 1);
    record->LastRecordedOrdinal.store(ordinal, std::memory_order_relaxed);
    // Key views into the record's own storage, which never moves.
    m_ByName.emplace(std::string_view(record->Name, length), record);
    return record;
}

GpuCheckpointNameTable::Record* GpuCheckpointNameTable::AllocateRecordLocked()
{
    if (m_RecordCount >= kMaxGpuCheckpointNames)
    {
        return nullptr;
    }
    const size_t indexInBlock = m_RecordCount % kRecordsPerBlock;
    if (indexInBlock == 0)
    {
        m_Blocks.push_back(std::make_unique<Block>());
    }
    ++m_RecordCount;
    return &(*m_Blocks.back())[indexInBlock];
}

const GpuCheckpointNameTable::Record* GpuCheckpointNameTable::ResolveLocked(const void* payload) const
{
    if (payload == nullptr)
    {
        return nullptr;
    }
    if (m_Overflow && payload == m_Overflow.get())
    {
        return m_Overflow.get();
    }
    const auto address = reinterpret_cast<uintptr_t>(payload);
    for (const auto& block : m_Blocks)
    {
        const auto base = reinterpret_cast<uintptr_t>(block->data());
        const uintptr_t end = base + sizeof(Record) * kRecordsPerBlock;
        if (address < base || address >= end)
        {
            continue;
        }
        // Inside a block, but only a record boundary is a payload we handed out.
        const uintptr_t offset = address - base;
        if (offset % sizeof(Record) != 0)
        {
            return nullptr;
        }
        const Record* record = block->data() + (offset / sizeof(Record));
        // A record past the allocation high-water mark was never interned.
        return record->Name[0] != '\0' ? record : nullptr;
    }
    return nullptr;
}

bool GpuCheckpointNameTable::Describe(const void* payload,
                                      std::string_view& outName,
                                      uint64_t& outLastRecordedOrdinal) const
{
    std::shared_lock<std::shared_mutex> read(m_Mutex);
    const Record* record = ResolveLocked(payload);
    if (record == nullptr)
    {
        return false;
    }
    outName = record->Name;
    outLastRecordedOrdinal = record->LastRecordedOrdinal.load(std::memory_order_relaxed);
    return true;
}

size_t GpuCheckpointNameTable::DistinctNameCount() const
{
    std::shared_lock<std::shared_mutex> read(m_Mutex);
    return m_ByName.size();
}

std::string FormatExecutedGpuCheckpoints(PFN_vkGetQueueCheckpointDataNV getCheckpointData,
                                        const VkQueue* queues,
                                        const char* const* queueNames,
                                        const VkPipelineStageFlags* queueCheckpointStages,
                                        uint32_t queueCount,
                                        const GpuCheckpointNameTable& names)
{
    if (getCheckpointData == nullptr || queues == nullptr || queueNames == nullptr ||
        queueCheckpointStages == nullptr)
    {
        return "<checkpoint retrieval unavailable>";
    }

    struct Entry
    {
        uint64_t Ordinal;
        std::string Text;
    };

    std::string out;
    std::vector<VkQueue> visited;
    std::vector<VkCheckpointDataNV> data;
    std::vector<Entry> entries;
    uint32_t queuesQueried = 0;

    for (uint32_t i = 0; i < queueCount; ++i)
    {
        const VkQueue queue = queues[i];
        if (queue == VK_NULL_HANDLE ||
            std::find(visited.begin(), visited.end(), queue) != visited.end())
        {
            continue; // unset, or an alias of a queue already reported
        }
        visited.push_back(queue);
        ++queuesQueried;

        // A family advertising no checkpoint stages cannot record or return
        // anything, so querying it proves nothing about what ran there. Say that
        // instead of reporting an absence the reader would take as evidence.
        if (queueCheckpointStages[i] == 0)
        {
            if (!out.empty())
                out += "; ";
            out += queueNames[i];
            out += ": <unsupported on this queue family — blind, not idle>";
            continue;
        }

        uint32_t count = 0;
        getCheckpointData(queue, &count, nullptr);
        if (count == 0)
        {
            // The query returns markers for work still OUTSTANDING on the queue, so
            // an empty result is not proof that nothing ran there: a queue that
            // drained cleanly before the loss reports the same nothing as one whose
            // work carried no markers. Report what was returned, not an inference.
            if (!out.empty())
                out += "; ";
            out += queueNames[i];
            out += ": <no outstanding checkpoints reported (drained, or none emitted here)>";
            continue;
        }

        data.assign(count, VkCheckpointDataNV{});
        for (VkCheckpointDataNV& entry : data)
        {
            entry.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        }
        getCheckpointData(queue, &count, data.data());

        entries.clear();
        entries.reserve(count);
        for (uint32_t d = 0; d < count; ++d)
        {
            std::string_view name;
            uint64_t ordinal = 0;
            std::string text = "[" + PipelineStageLabel(data[d].stage) + "] ";
            if (names.Describe(data[d].pCheckpointMarker, name, ordinal))
            {
                text += std::string(name);
                text += " (name last recorded #" + std::to_string(ordinal) + ")";
            }
            else
            {
                text += "<unrecognized payload>";
            }
            entries.push_back(Entry{ordinal, std::move(text)});
        }
        // Oldest first / newest last, matching the CPU marker ring's convention.
        std::stable_sort(entries.begin(), entries.end(),
                         [](const Entry& a, const Entry& b) { return a.Ordinal < b.Ordinal; });

        if (!out.empty())
            out += "; ";
        out += queueNames[i];
        out += ": ";
        for (size_t e = 0; e < entries.size(); ++e)
        {
            if (e != 0)
                out += " -> ";
            out += entries[e].Text;
        }
    }

    if (queuesQueried == 0)
    {
        return "<no queues to query>";
    }

    out += " | recorded " + std::to_string(names.RecordedCount()) + " marker(s) across " +
           std::to_string(names.DistinctNameCount()) + " distinct name(s)";
    if (names.Overflowed())
    {
        out += " (name table overflowed)";
    }
    return out;
}

} // namespace Rendering
} // namespace GameEngine
