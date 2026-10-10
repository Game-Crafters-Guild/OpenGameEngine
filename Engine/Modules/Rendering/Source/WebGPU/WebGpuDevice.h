#pragma once

// WebGPU IDevice backend. Desktop builds bind wgpu-native (Metal on macOS,
// D3D12 on Windows, Vulkan on Linux) so the web renderer can be developed and
// debugged natively; the browser build binds emdawnwebgpu against the same
// standard webgpu.h.
//
// Scope of the current slice: instance/adapter/device/queue, buffers, textures,
// views, samplers, bind groups, graphics + compute pipelines, command recording
// and the desktop surface. Everything the GPU-driven desktop path relies on —
// bindless, descriptor buffers, buffer device addresses, native indirect-count,
// mesh shaders, ray tracing — is absent from WebGPU and is reported false in
// the capability struct, so the compatibility renderer profile never asks.

#include "Rendering/Core/CommandList.h"
#include "WebGpuQueryPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/WebGpuPushConstantRing.h"

#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{

class WebGpuCommandList;
class WebGpuSwapchain;

// WebGPU COPY_BYTES_PER_ROW_ALIGNMENT: a buffer<->texture copy spanning more
// than one row must state a row pitch that is a multiple of this.
inline constexpr uint32_t kWebGpuCopyBytesPerRowAlignment = 256;

// GPU buffer-to-buffer copies address in 4-byte units: offsets and sizes must
// be multiples of this, which bounds what the copy path can repack.
inline constexpr uint32_t kWebGpuCopyBufferAlignment = 4;

// Half-open byte span of a buffer's CPU shadow that a producer declared it
// wrote (IDevice::FlushMappedRange).
struct WebGpuDirtySpan
{
    uint64_t Begin = 0;
    uint64_t End = 0;
};

struct WebGpuBuffer
{
    WGPUBuffer buffer = nullptr; // owned (+1)
    uint64_t size = 0;
    uint32_t usage = 0;
    BufferMemoryUsage memoryUsage = BufferMemoryUsage::Auto;
    // WebGPU has no persistent host mapping for GPU-visible buffers. Upload
    // buffers therefore carry a CPU shadow that MapBuffer hands out and
    // UnmapBuffer flushes through wgpuQueueWriteBuffer. Readback buffers map
    // for real (MapRead) and keep this empty.
    std::vector<uint8_t> shadow;
    bool mappedForRead = false;
    // Deferred read-map (web): a mapAsync whose callback has not landed yet.
    // 0 = none, 1 = pending, 2 = ready, 3 = failed. Mid-frame the map cannot
    // block: every pump yield ends the browser task that owns the swapchain
    // texture, so a blocking map poisons every later submit that touches the
    // surface. MapBuffer parks the request instead and answers nullptr; the
    // callback lands on a natural inter-frame yield and the next call maps.
    uint32_t pendingReadMap = 0;
    // A persistent mapping (RGUploadRing, the ring-buffer helpers) is mapped
    // once and never unmapped, so no Unmap can flush it; the submit path does.
    bool shadowMappedForWrite = false;
    // What the next flush uploads. `dirtyWhole` covers a mapping whose extent
    // is unknown — the frames between MapBuffer and the first submit, and any
    // Unmap by a producer that declared nothing.
    std::vector<WebGpuDirtySpan> dirtySpans;
    bool dirtyWhole = false;
    // Page hashes of the shadow as last uploaded, for the flush audit
    // (kFlushAuditEnvVar). Empty unless the audit is armed.
    std::vector<uint64_t> auditPageHashes;
    std::string debugName;
};

struct WebGpuTexture
{
    WGPUTexture texture = nullptr;   // owned (+1) unless this is a surface slot
    WGPUTextureView defaultView = nullptr; // owned (+1)
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t sampleCount = 1;
    uint32_t usage = 0;
    // Surface slots alias the texture returned by wgpuSurfaceGetCurrentTexture;
    // the swapchain owns the reference and releases it after the submit that
    // consumed it completed.
    bool isSurfaceSlot = false;
    std::string debugName;
};

struct WebGpuTextureView
{
    WGPUTextureView view = nullptr; // owned (+1)
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    std::string debugName;
};

struct WebGpuSampler
{
    WGPUSampler sampler = nullptr; // owned (+1)
    std::string debugName;
};

// A descriptor set is a WebGPU bind group. Bind groups are immutable, while the
// engine creates a set and then patches bindings into it, so the entries are
// shadowed CPU-side and the bind group is (re)built lazily on first bind after
// a change.
struct WebGpuDescriptorSet
{
    WGPUBindGroupLayout layout = nullptr; // borrowed from the device's layout cache
    WGPUBindGroup bindGroup = nullptr;    // owned (+1); rebuilt when dirty
    std::vector<WGPUBindGroupEntry> entries;
    // Binding numbers the layout declares. A bind group may only carry entries
    // the layout knows, so a write to anything else is dropped rather than
    // invalidating the whole group.
    std::vector<uint32_t> declaredBindings;
    bool dirty = true;
    std::string debugName;
};

struct WebGpuPipeline
{
    WGPURenderPipeline renderPipeline = nullptr;   // owned (+1)
    // Twin of `renderPipeline` with depthWriteEnabled forced off, bound instead
    // whenever the pass attaches depth read-only. WebGPU bakes depth-write into
    // the pipeline and rejects a depth-writing pipeline in such a pass, so the
    // dynamic override Vulkan does with vkCmdSetDepthWriteEnable (and Metal with
    // a read-only depth-stencil state) has to be a second pipeline object here.
    // Null when the pipeline has no depth attachment or already writes no depth.
    WGPURenderPipeline renderPipelineDepthReadOnly = nullptr; // owned (+1)
    WGPUComputePipeline computePipeline = nullptr; // owned (+1)
    WGPUPipelineLayout pipelineLayout = nullptr;   // owned (+1)
    PipelineType type = PipelineType::Graphics;
    PipelineFormatKey formatKey{};

    uint32_t pushConstantSize = 0;
    uint32_t pushConstantStagesMask = 0;
    // Number of engine descriptor sets in the pipeline layout. Groups between
    // this and the push-constant emulation group are padded with the shared
    // empty layout; the command list binds the shared empty group there.
    uint32_t descriptorSetCount = 0;
    // The engine vertex binding (VertexLayoutBuilder.h) each WebGPU vertex
    // buffer slot reads: slot i is binding vertexBindings[i]. The slots are
    // dense, the bindings need not be (no tangent: bindings 0 and 2), and a draw
    // names bindings, so the command list binds each buffer at its slot here.
    std::vector<uint32_t> vertexBindings;
    struct PushRange
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t stagesMask = 0;
    };
    std::vector<PushRange> pushRanges;
    std::string debugName;
};

class WebGpuDevice : public IDevice
{
    friend class WebGpuCommandList;
    friend class WebGpuSwapchain;

  public:
    WebGpuDevice();
    ~WebGpuDevice() override;

    // Device management
    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown() override;
    const RenderingDeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
    GraphicsAPI GetAPI() const override { return GraphicsAPI::WebGPU; }
    // Resolved by CreateInstance before any shader loads: wgpu-native takes
    // SPIR-V when the instance came up with ShaderSourceSPIRV, the browser
    // never does.
    ShaderSourceKind PreferredShaderSource() const override
    {
        return m_SupportsSpirv ? ShaderSourceKind::SpirV : ShaderSourceKind::Wgsl;
    }
    std::string GetHardwareDescription() const override { return m_HardwareDescription; }
    void PrintCapabilityReport() const override;

    // Push-constant introspection (backed by wgpu immediates when the adapter
    // exposes them; the ranges are tracked regardless so reflection answers).
    uint32_t GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const override;
    bool GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id, PushConstantRangeInfo& outInfo) const override;
    bool FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const override;
    bool GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const override;

    // WebGPU exposes a single queue, so every family index is 0 and queue
    // ownership transfers in barriers degrade to no-ops.
    uint32_t GetGraphicsQueueFamilyIndex() const override { return 0; }
    uint32_t GetComputeQueueFamilyIndex() const override { return 0; }
    uint32_t GetTransferQueueFamilyIndex() const override { return 0; }

    // Resources
    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    TextureViewHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc) override;
    void DestroyTextureView(TextureViewHandle handle) override;
    void DestroyBuffer(BufferHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    void DestroySampler(SamplerHandle handle) override;
    void DestroyPipeline(PipelineHandle handle) override;

    void* MapBuffer(BufferHandle handle) override;
    bool IsBufferMapBusy(BufferHandle handle) const override;
    void UnmapBuffer(BufferHandle handle) override;
    void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;
    void FlushMappedRange(BufferHandle handle, size_t offset, size_t size) override;

    // Pipelines
    PipelineHandle CreateConcreteGraphicsPipeline(const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk) override;
    PipelineHandle CreateConcreteComputePipeline(const ComputePipelineDesc& cd) override;

    // Descriptor sets (bind groups)
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override;
    void UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update) override;
    void DestroyDescriptorSet(DescriptorSetHandle descriptorSet) override;

    // Command lists & submission
    std::unique_ptr<CommandList> CreateCommandList(QueueType queue) override;
    void ExecuteCommandLists(const std::vector<CommandList*>& commandLists) override;
    void WaitForIdle() override;

    // Timeline semaphores, emulated CPU-side: WebGPU exposes one in-order
    // queue, so GPU-side waits are ordering no-ops; signals land when
    // wgpuQueueOnSubmittedWorkDone fires for the signaling submission.
    SemaphoreHandle CreateTimelineSemaphore(uint64_t initialValue) override;
    void DestroySemaphore(SemaphoreHandle handle) override;
    bool GetTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t& outValue) const override;
    bool WaitTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t value, uint64_t timeoutNs = ~0ull) override;
    bool QueueSubmit(QueueType queue,
                     const std::vector<CommandList*>& cmdLists,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores) override;

    // Frame loop
    bool BeginFrame() override;
    void Present() override;
    void FinalizeFrame() override;
    uint32_t GetFramesInFlight() const override { return kFramesInFlight; }
    uint32_t GetFrameIndex() const override { return m_FrameIndex; }
    void SetVsync(bool vsync) override;
    bool IsVsyncEnabled() const override { return m_Desc.vsync; }

    // Window targets / surfaces
    WindowTargetHandle CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height) override;
    bool DestroyWindowTarget(WindowTargetHandle target) override;
    bool SetActiveWindowTarget(WindowTargetHandle target) override;
    WindowTargetHandle GetActiveWindowTarget() const override { return m_ActiveWindowTarget; }
    bool RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height) override;
    bool GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const override;
    bool GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const override;
    bool AcquireNextImage(uint32_t& imageIndex) override;
    bool PresentImage(uint32_t imageIndex) override;
    uint32_t GetSwapchainImageCount() const override;
    TextureHandle GetSwapchainImage(uint32_t index) const override;
    TextureHandle GetCurrentSwapchainImageHandle() override;
    TextureFormat GetSwapchainTextureFormat() const override;

    // Managers: WebGPU tracks resource hazards itself and has no query-pool
    // wiring in this slice.
    ResourceManager* GetResourceManager() override { return nullptr; }
    IQueryPool* GetQueryPool() override { return m_QueryPool.get(); }

    // Texture introspection
    TextureFormat GetTextureFormat(TextureHandle texture) const override;
    uint32_t GetTextureSampleCount(TextureHandle texture) const override;
    uint32_t GetTextureArrayLayers(TextureHandle texture) const override;
    void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const override;
    bool IsTextureAlive(TextureHandle texture) const override;
    // VRAM panel: live registries and their byte footprint. WebGPU exposes no
    // allocation size, so texture bytes are the tightly packed mip chain.
    size_t DebugGetAllocationCount() const override;
    size_t DebugGetBufferRegistryCount() const override;
    size_t DebugGetImageRegistryCount() const override;
    size_t DebugGetAllocatedBytes() const override;
    size_t DebugGetBufferBytes() const override;
    size_t DebugGetTextureBytes() const override;
    void DebugEnumerateResources(const std::function<void(const DebugResourceInfo&)>& fn) const override;
    bool IsTextureHandleLive(TextureHandle texture) const override;
    bool IsPipelineAlive(PipelineHandle pipeline) const override;
    bool IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const override;

    // Internal accessors for the command list and swapchain.
    WGPUDevice GetWgpuDevice() const { return m_Device; }
    WGPUQueue GetWgpuQueue() const { return m_Queue; }
    WebGpuBuffer* GetBuffer(BufferHandle handle);
    WebGpuTexture* GetTexture(TextureHandle handle);
    WebGpuTextureView* GetTextureView(TextureViewHandle handle);
    WebGpuSampler* GetSampler(SamplerHandle handle);
    WebGpuPipeline* GetPipeline(PipelineHandle handle);
    WebGpuDescriptorSet* GetDescriptorSet(DescriptorSetHandle handle);
    // Resolves the set's cached bind group, rebuilding it if bindings changed
    // since the last bind. Null when the set is unknown or has no layout.
    WGPUBindGroup ResolveBindGroup(DescriptorSetHandle handle);
    // True when the adapter granted the wgpu immediates feature — the transport
    // this backend uses for engine push constants.
    bool SupportsImmediates() const { return m_SupportsImmediates; }

    // Creates (or returns) the bind-group layout for an interned engine layout
    // id. Pipelines and descriptor sets share the cache so a set created from
    // the same layout desc is bind-compatible with the pipeline.
    WGPUBindGroupLayout GetOrCreateBindGroupLayout(DescriptorSetLayoutId id);

  private:
    // WebGPU paces frames through the surface itself; three slots match the
    // engine's expectation of a small in-flight window without pretending to
    // control the browser's presentation queue.
    static constexpr uint32_t kFramesInFlight = 3;
    // Buffer-binding headroom the BonePalette pool needs (PerFrameWritePool
    // grows it to 256 MB); WebGPU's default maxStorageBufferBindingSize is
    // 128 MB, so the device asks the adapter for its full limit set.
    static constexpr uint64_t kDesiredStorageBufferBindingSize = 256ull * 1024ull * 1024ull;
    // WebGPU has no combined image/sampler binding; the WGSL cook splits the
    // pair and renumbers the sampler to binding + this offset (must match
    // kSamplerBindingOffset in Tools/ShaderCook/shadercook.py). The backend
    // applies the same rule when building layouts and bind groups from
    // engine descriptors that still say CombinedImageSampler.
    static constexpr uint32_t kCombinedSamplerBindingOffset = 64;
    // Browser WebGPU has no push constants/immediates. The WGSL cook rewrites
    // push-constant blocks to a UBO at this reserved bind group (sets 0-2 are
    // the engine's material layout). Draws suballocate from a per-frame ring
    // and bind it with a dynamic offset — a new GPU buffer per draw would
    // keep every UBO alive until queue-done, which on the browser path can
    // fail to fire and grow wasm linear memory without bound.
    static constexpr uint32_t kPushConstantEmulationGroup = 3;
    static constexpr uint32_t kPushConstantSlotsPerFrame = kWebGpuPushConstantSlotsPerFrame;
    // Bounded spin for PumpUntil — an escape hatch against a callback that
    // never resolves, not a latency budget. Sized for the browser path, where
    // each attempt is a ~1 ms event-loop yield and a cold adapter/device
    // request can take seconds of real time.
    bool m_SupportsDepthClipControl = false;
    bool m_SupportsFloat32Filterable = false;
    static constexpr uint32_t kFutureWaitAttempts = 16384;

    bool CreateInstance(const DeviceDesc& desc);
    bool SelectAdapter();
    bool CreateLogicalDevice(const DeviceDesc& desc);
    void PopulateCapabilities();
    // Drains wgpu's event loop until `future` resolves. wgpu-native runs
    // callbacks on the calling thread, so this is a bounded spin rather than a
    // real block; the browser build replaces it with an ASYNCIFY yield.
    bool PumpUntil(const bool& done);
    WebGpuSwapchain* GetSwapchain(WindowTargetHandle target);
    const WebGpuSwapchain* GetSwapchain(WindowTargetHandle target) const;
    WGPUBindGroupLayout CreateBindGroupLayout(const DescriptorSetLayoutDesc& desc);
    // Accepts a SPIR-V word stream (desktop dev loop) or UTF-8 WGSL text (the
    // cook's output; the only source browser WebGPU accepts), told apart by
    // the SPIR-V magic.
    WGPUShaderModule CreateShaderModule(const std::vector<uint8_t>& bytes, const char* debugName);
    WGPUPipelineLayout CreatePipelineLayout(const std::vector<DescriptorSetLayoutId>& layouts,
                                            uint32_t immediateSize, const char* debugName);
    // Push-constant emulation plumbing (browser path; see
    // kPushConstantEmulationGroup). Lazily created, owned by the device.
    WGPUBindGroupLayout GetOrCreateEmptyBindGroupLayout();
    WGPUBindGroup GetOrCreateEmptyBindGroup();
    WGPUBindGroupLayout GetOrCreatePushConstantBindGroupLayout();
    bool EnsurePushConstantRing();
    // Writes `data` into this frame's ring slice and binds group 3 with a
    // dynamic offset. Returns false when the block cannot be bound (the slice
    // is full, or the ring could not be created): the caller then refuses the
    // draw or dispatch, because recording it with group 3 unbound invalidates
    // the frame's whole command buffer. A full slice is counted against
    // `passLabel` and `pipelineName` and reported at the next BeginFrame.
    bool BindEmulatedPushConstants(WGPURenderPassEncoder renderPass, WGPUComputePassEncoder computePass,
                                   const void* data, uint32_t size, const std::string& passLabel,
                                   const std::string& pipelineName);
    // Logs the frame's ring refusals: once when a run of refusing frames
    // starts, and again whenever a frame refuses more than any before it in
    // that run, so a steady overflow does not flood the console.
    void ReportPushConstantRefusals();
    void ReleasePushConstantRing();

    // Writes every still-mapped shadow's dirty ranges to its GPU buffer. Queue
    // writes are ordered against the submit that follows, so this is what makes
    // a persistently mapped upload visible to the commands that read it.
    //
    // The ranges are what the frame declared through FlushMappedRange; a ring
    // therefore costs its frame's allocations, not its capacity. That cost is
    // invisible from the engine side — it accrues inside the browser's
    // writeBuffer with no engine frame on the stack — so ReportShadowFlushVolume
    // names it once.
    void FlushMappedShadows();
    // Uploads one buffer's pending ranges and returns the bytes written.
    uint64_t FlushBufferShadow(WebGpuBuffer& buffer);
    void ReportShadowFlushVolume(uint64_t totalBytes) const;
    void ReadFlushDiagnosticSettings();
    void ForgetMappedShadow(WebGpuBuffer& buffer, BufferHandle handle);
    // Names any shadow byte that changed since the last upload without a
    // covering FlushMappedRange — a missed range, which renders stale on this
    // backend and correctly everywhere else. Uploads what it finds, so an
    // audited run is visually correct and the log is the signal.
    void AuditUndeclaredWrites(WebGpuBuffer& buffer);
    void RefreshAuditHashes(WebGpuBuffer& buffer, uint64_t begin, uint64_t end);

    static void OnUncapturedError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message,
                                  void* userdata1, void* userdata2);
    static void OnDeviceLost(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message,
                             void* userdata1, void* userdata2);

    WGPUInstance m_Instance = nullptr; // owned (+1)
    WGPUAdapter m_Adapter = nullptr;   // owned (+1)
    WGPUDevice m_Device = nullptr;     // owned (+1)
    WGPUQueue m_Queue = nullptr;       // owned (+1)
    // Present only where the adapter exposes WGPUFeatureName_TimestampQuery.
    bool m_SupportsTimestampQuery = false;
    std::unique_ptr<WebGpuQueryPool> m_QueryPool;
    WGPULimits m_AdapterLimits{};
    WGPULimits m_DeviceLimits{};
    bool m_SupportsImmediates = false;
    bool m_SupportsSpirv = false;
    std::string m_HardwareDescription;

    DeviceDesc m_Desc{};
    RenderingDeviceCapabilities m_Capabilities{};
    bool m_Initialized = false;
    uint32_t m_FrameIndex = 0;

    mutable std::recursive_mutex m_ResourceMutex;
    GenerationalVector<WebGpuBuffer> m_Buffers;
    // Buffers whose shadow is handed out and not yet unmapped, in map order.
    std::vector<BufferHandle> m_MappedForWriteBuffers;
    mutable uint32_t m_ShadowFlushCount = 0;
    mutable bool m_ReportedShadowFlushVolume = false;
    // Differential oracle for the range migration: upload every mapped shadow
    // in full, ignoring declared ranges. Any visual difference against the
    // ranged path is a range a producer failed to declare.
    bool m_FlushWholeShadows = false;
    bool m_AuditUndeclaredWrites = false;
    GenerationalVector<WebGpuTexture> m_Textures;
    GenerationalVector<WebGpuTextureView> m_TextureViews;
    GenerationalVector<WebGpuSampler> m_Samplers;
    GenerationalVector<WebGpuPipeline> m_Pipelines;
    GenerationalVector<WebGpuDescriptorSet> m_DescriptorSets;

    // Transient descriptor sets, bucketed by the frame slot they were created
    // in. BeginFrame destroys the current slot's bucket — those sets are
    // kFramesInFlight frames old, so their GPU work has retired. Without this
    // the render path's per-frame sets leak forever (Dawn D3D12 descriptor-heap
    // exhaustion → device lost). Mirrors Metal's m_TransientDescriptorSets.
    std::array<std::vector<DescriptorSetHandle>, kFramesInFlight> m_TransientDescriptorSets;

    // Interned engine layout id -> bind-group layout (+1 each).
    std::unordered_map<uint32_t, WGPUBindGroupLayout> m_BindGroupLayouts;

    // Push-constant emulation objects (owned, +1 each; browser path).
    WGPUBindGroupLayout m_EmptyBindGroupLayout = nullptr;
    WGPUBindGroup m_EmptyBindGroup = nullptr;
    WGPUBindGroupLayout m_PushConstantBindGroupLayout = nullptr;
    WGPUBuffer m_PushConstantRing = nullptr;
    WGPUBindGroup m_PushConstantBindGroup = nullptr;
    uint32_t m_PushConstantSlotBytes = 256;
    uint64_t m_PushConstantSliceBytes = 0;
    uint64_t m_PushConstantOffset = 0;
    // Refusals in the frame being recorded, the first refused draw's pass and
    // pipeline, and the largest count reported in the current refusing run.
    uint32_t m_PushConstantRefusals = 0;
    std::string m_PushConstantFirstRefusalPass;
    std::string m_PushConstantFirstRefusalPipeline;
    uint32_t m_PushConstantRefusalsReported = 0;

    // CPU-side timeline semaphore state. shared_ptr: the OnSubmittedWorkDone
    // callback may outlive DestroySemaphore.
    struct WebGpuSemaphore
    {
        std::shared_ptr<std::atomic<uint64_t>> Value;
    };
    GenerationalVector<WebGpuSemaphore> m_Semaphores;

    GenerationalVector<std::unique_ptr<WebGpuSwapchain>> m_WindowTargets;
    WindowTargetHandle m_ActiveWindowTarget{};
};

} // namespace GameEngine::Rendering
