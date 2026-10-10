#include "WebGpuQueryPool.h"

#include "WebGpuCommandList.h"
#include "WebGpuConversions.h"
#include "WebGpuDevice.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Rendering
{

namespace
{
constexpr uint64_t kTimestampBytes = sizeof(uint64_t);
}

WebGpuQueryPool::WebGpuQueryPool(WebGpuDevice& device, WGPUDevice raw, const Config& config)
    : m_Device(device)
    , m_Raw(raw)
    , m_Config(config)
{
}

WebGpuQueryPool::~WebGpuQueryPool()
{
    Shutdown();
}

bool WebGpuQueryPool::Initialize()
{
    if (m_Raw == nullptr)
        return false;

    const uint32_t frames = std::max(1u, m_Config.FramesInFlight);
    // The whole set is one allocation shared by every slot; a slot owns a
    // contiguous range so its resolve is one call.
    m_QueriesPerSlot = std::max(2u, m_Config.MaxTimestampQueries / frames);
    const uint32_t total = m_QueriesPerSlot * frames;

    WGPUQuerySetDescriptor qsd{};
    qsd.label = WebGpu::MakeStringView("GE.Timestamps");
    qsd.type = WGPUQueryType_Timestamp;
    qsd.count = total;
    m_QuerySet = wgpuDeviceCreateQuerySet(m_Raw, &qsd);
    if (m_QuerySet == nullptr)
    {
        Logger::Log::Info("WebGpuQueryPool: timestamp query set unavailable; GPU timings stay off");
        return false;
    }

    m_Slots.resize(frames);
    for (uint32_t i = 0; i < frames; ++i)
    {
        FrameSlot& slot = m_Slots[i];
        slot.FirstQuery = i * m_QueriesPerSlot;

        WGPUBufferDescriptor rd{};
        rd.label = WebGpu::MakeStringView("GE.TimestampResolve");
        rd.size = static_cast<uint64_t>(m_QueriesPerSlot) * kTimestampBytes;
        rd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
        slot.Resolve = wgpuDeviceCreateBuffer(m_Raw, &rd);

        WGPUBufferDescriptor bd{};
        bd.label = WebGpu::MakeStringView("GE.TimestampReadback");
        bd.size = rd.size;
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        slot.Readback = wgpuDeviceCreateBuffer(m_Raw, &bd);

        if (slot.Resolve == nullptr || slot.Readback == nullptr)
        {
            Logger::Log::Warning("WebGpuQueryPool: timestamp buffers failed; GPU timings stay off");
            Shutdown();
            return false;
        }
    }

    Logger::Log::Info("WebGpuQueryPool: {} timestamps across {} frame slots", total, frames);
    return true;
}

void WebGpuQueryPool::Shutdown()
{
    for (FrameSlot& slot : m_Slots)
    {
        // A pending map still references the buffer; unmapping first is what
        // makes the release legal.
        if (slot.MapPending && slot.Readback != nullptr)
            wgpuBufferUnmap(slot.Readback);
        if (slot.Resolve != nullptr)
            wgpuBufferRelease(slot.Resolve);
        if (slot.Readback != nullptr)
            wgpuBufferRelease(slot.Readback);
    }
    m_Slots.clear();
    if (m_QuerySet != nullptr)
    {
        wgpuQuerySetRelease(m_QuerySet);
        m_QuerySet = nullptr;
    }
}

void WebGpuQueryPool::OnMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1,
                               void* userdata2)
{
    auto* pool = static_cast<WebGpuQueryPool*>(userdata1);
    const auto slotIndex = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userdata2));
    if (pool == nullptr || slotIndex >= pool->m_Slots.size())
        return;

    FrameSlot& slot = pool->m_Slots[slotIndex];
    slot.MapPending = false;
    if (status != WGPUMapAsyncStatus_Success)
    {
        // Not fatal and not worth a per-frame line: the slot simply has no
        // timings this turn and the next frame re-arms it.
        (void)message;
        return;
    }

    const uint64_t bytes = static_cast<uint64_t>(slot.ReadbackCount) * kTimestampBytes;
    const void* mapped = wgpuBufferGetConstMappedRange(slot.Readback, 0, bytes);
    if (mapped != nullptr)
    {
        slot.Values.resize(slot.ReadbackCount);
        std::memcpy(slot.Values.data(), mapped, static_cast<size_t>(bytes));
        slot.HaveResults = true;
        if (!pool->m_LoggedFirstReadback)
        {
            pool->m_LoggedFirstReadback = true;
            Logger::Log::Info("WebGpuQueryPool: first timestamp readback ({} queries, {} ns span)",
                              slot.ReadbackCount,
                              slot.Count >= 2 ? static_cast<long long>(slot.Values.back()) -
                                                    static_cast<long long>(slot.Values.front())
                                              : 0LL);
        }
    }
    wgpuBufferUnmap(slot.Readback);
}

void WebGpuQueryPool::BeginFrame(uint32_t frameIndex)
{
    if (m_Slots.empty())
        return;
    m_FrameIndex = frameIndex % static_cast<uint32_t>(m_Slots.size());
    FrameSlot& slot = m_Slots[m_FrameIndex];

    // This slot's previous turn has retired by contract (the caller waited its
    // fence). Its results stay readable: the render graph resolves them right
    // after this and clears them itself. Only the write cursor re-arms here.
    slot.Count = 0;
    m_SlotCursor = 0;
    if (!slot.HaveResults && !slot.MapPending)
    {
        // Nothing to read back (profiling was off): drop the turn's bookkeeping.
        slot.Names.clear();
        slot.Alias.clear();
    }
}

uint32_t WebGpuQueryPool::ReserveQuery()
{
    if (m_QuerySet == nullptr || m_Slots.empty() || m_SlotCursor >= m_QueriesPerSlot)
        return kInvalidQuery;
    FrameSlot& slot = m_Slots[m_FrameIndex];
    const uint32_t local = m_SlotCursor++;
    slot.Count = m_SlotCursor;
    return local;
}

uint32_t WebGpuQueryPool::AbsoluteIndex(uint32_t local) const
{
    if (m_Slots.empty() || local == kInvalidQuery)
        return WGPU_QUERY_SET_INDEX_UNDEFINED;
    return m_Slots[m_FrameIndex].FirstQuery + local;
}

void WebGpuQueryPool::AliasQuery(uint32_t from, uint32_t to)
{
    if (from == kInvalidQuery || to == kInvalidQuery || from == to)
        return;
    // Chase so a chain of aliases costs one lookup at read time.
    auto& alias = m_Slots[m_FrameIndex].Alias;
    const auto it = alias.find(to);
    alias[from] = it != alias.end() ? it->second : to;
}

void WebGpuQueryPool::InvalidateCachedTimestampResults(uint32_t frameIndex)
{
    if (m_Slots.empty())
        return;
    FrameSlot& slot = m_Slots[frameIndex % static_cast<uint32_t>(m_Slots.size())];
    slot.HaveResults = false;
    slot.Values.clear();
    slot.Names.clear();
    slot.Alias.clear();
}

uint32_t WebGpuQueryPool::WriteTimestamp(CommandList* commandList, TimestampPoint point, const std::string& name)
{
    if (m_QuerySet == nullptr || m_Slots.empty() || commandList == nullptr)
        return kInvalidQuery;
    if (m_SlotCursor >= m_QueriesPerSlot)
        return kInvalidQuery; // slot full: the frame keeps rendering, untimed

    auto* webCl = static_cast<WebGpuCommandList*>(commandList);
#if defined(__EMSCRIPTEN__)
    // Browser WebGPU has no encoder-level write. The query is written at the
    // next pass boundary instead: a SpanBegin rides the beginning of the next
    // pass, a SpanEnd resolves to the end of the last pass recorded since.
    (void)point;
    FrameSlot& slot = m_Slots[m_FrameIndex];
    const uint32_t local = m_SlotCursor++;
    slot.Count = m_SlotCursor;
    if (!name.empty())
        slot.Names[local] = name;
    webCl->NoteTimestamp(point, local);
    return local;
#else
    (void)point;
    // Bare encoder only: a write while a pass is open is illegal, and the graph
    // brackets its passes, so a mid-pass request is a caller bug — refuse
    // rather than record something the backend would reject wholesale.
    WGPUCommandEncoder encoder = webCl->GetOpenEncoderForTimestamp();
    if (encoder == nullptr)
        return kInvalidQuery;

    FrameSlot& slot = m_Slots[m_FrameIndex];
    const uint32_t local = m_SlotCursor++;
    slot.Count = m_SlotCursor;
    wgpuCommandEncoderWriteTimestamp(encoder, m_QuerySet, slot.FirstQuery + local);
    if (!name.empty())
        slot.Names[local] = name;
    return local;
#endif
}

void WebGpuQueryPool::EndFrame()
{
    if (m_QuerySet == nullptr || m_Slots.empty())
        return;
    FrameSlot& slot = m_Slots[m_FrameIndex];
    if (slot.Count == 0 || slot.MapPending)
        return;

    // Resolve + copy on an encoder of its own, submitted after the frame's work
    // is already queued: queue order is what puts it behind the writes, so this
    // needs no access to whichever command list recorded them.
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(m_Raw, nullptr);
    if (encoder == nullptr)
        return;

    slot.ReadbackCount = slot.Count;
    const uint64_t bytes = static_cast<uint64_t>(slot.Count) * kTimestampBytes;
    wgpuCommandEncoderResolveQuerySet(encoder, m_QuerySet, slot.FirstQuery, slot.Count,
                                      slot.Resolve, 0);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, slot.Resolve, 0, slot.Readback, 0, bytes);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (cb == nullptr)
        return;
    WGPUQueue queue = m_Device.GetWgpuQueue();
    wgpuQueueSubmit(queue, 1, &cb);
    wgpuCommandBufferRelease(cb);

    // Mapped without ever pumping for it: the result lands on some later frame
    // and is read when this slot comes round again. Waiting here would yield
    // mid-frame, which costs the canvas texture on this backend.
    WGPUBufferMapCallbackInfo info{};
    info.mode = WGPUCallbackMode_AllowProcessEvents;
    info.callback = &WebGpuQueryPool::OnMapped;
    info.userdata1 = this;
    info.userdata2 = reinterpret_cast<void*>(static_cast<uintptr_t>(m_FrameIndex));
    slot.MapPending = true;
    (void)wgpuBufferMapAsync(slot.Readback, WGPUMapMode_Read, 0, bytes, info);
}

bool WebGpuQueryPool::GetTimestampResult(uint32_t queryIndex, QueryResult& outResult)
{
    if (m_Slots.empty() || queryIndex == kInvalidQuery)
        return false;
    const FrameSlot& slot = m_Slots[m_FrameIndex];
    const uint32_t nameIndex = queryIndex;
    if (const auto alias = slot.Alias.find(queryIndex); alias != slot.Alias.end())
        queryIndex = alias->second;
    if (!slot.HaveResults || queryIndex >= slot.Values.size())
        return false;

    outResult.Value = slot.Values[queryIndex];
    outResult.Available = true;
    outResult.Type = QueryType::Timestamp;
    if (const auto it = slot.Names.find(nameIndex); it != slot.Names.end())
        outResult.Name = it->second;
    if (!m_LoggedFirstRead)
    {
        m_LoggedFirstRead = true;
        Logger::Log::Info("WebGpuQueryPool: first resolved timestamp read (query {} -> {})", nameIndex, queryIndex);
    }
    return true;
}

std::vector<QueryResult> WebGpuQueryPool::GetAllTimestampResults() const
{
    std::vector<QueryResult> out;
    if (m_Slots.empty())
        return out;
    const FrameSlot& slot = m_Slots[m_FrameIndex];
    if (!slot.HaveResults)
        return out;
    out.reserve(slot.Values.size());
    for (size_t i = 0; i < slot.Values.size(); ++i)
    {
        QueryResult r{};
        r.Value = slot.Values[i];
        r.Available = true;
        r.Type = QueryType::Timestamp;
        if (const auto it = slot.Names.find(static_cast<uint32_t>(i)); it != slot.Names.end())
            r.Name = it->second;
        out.push_back(std::move(r));
    }
    return out;
}

void WebGpuQueryPool::DebugPrintResults() const
{
    for (const QueryResult& r : GetAllTimestampResults())
        Logger::Log::Info("  [gpu] {} = {:.3f} ms", r.Name, TimestampToMs(r.Value));
}

} // namespace GameEngine::Rendering
