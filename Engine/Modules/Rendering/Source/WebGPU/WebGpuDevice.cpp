#include "WebGpuDevice.h"

#include "WebGpuCommandList.h"
#include "WebGpuConversions.h"
#include "WebGpuSurfaceBridge.h"
#include "WebGpuSwapchain.h"
#include "WebGpuUnsupported.h"

#include "Logger/Logger.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
// wgpu-native extras (immediates, DevicePoll); Dawn/emdawnwebgpu has no wgpu.h.
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

namespace
{
// Adapter rank: the first enumerated adapter frequently cannot present, so the
// backend picks deliberately. D3D12 first on Windows, then discrete over
// integrated, then anything that is not a software rasterizer.
int RankAdapter(const WGPUAdapterInfo& info)
{
    int score = 0;
#if defined(_WIN32)
    if (info.backendType == WGPUBackendType_D3D12) score += 1000;
#else
    if (info.backendType == WGPUBackendType_Metal) score += 1000;
    if (info.backendType == WGPUBackendType_Vulkan) score += 900;
#endif
    switch (info.adapterType)
    {
    case WGPUAdapterType_DiscreteGPU:   score += 100; break;
    case WGPUAdapterType_IntegratedGPU: score += 50;  break;
    case WGPUAdapterType_CPU:           score -= 500; break;
    default: break;
    }
    return score;
}

bool ReadAdapterIndexOverride(uint32_t& outIndex)
{
    const char* value = std::getenv("GE_WEBGPU_ADAPTER");
    if (value == nullptr || value[0] == '\0')
    {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 0)
    {
        Logger::Log::Warning("WebGpuDevice: GE_WEBGPU_ADAPTER='{}' is not a valid index; ignoring", value);
        return false;
    }
    outIndex = static_cast<uint32_t>(parsed);
    return true;
}

// Null when the platform does not report the type (browsers report Unknown).
const char* AdapterTypeName(WGPUAdapterType type)
{
    switch (type)
    {
    case WGPUAdapterType_DiscreteGPU:   return "discrete GPU";
    case WGPUAdapterType_IntegratedGPU: return "integrated GPU";
    case WGPUAdapterType_CPU:           return "CPU";
    default:                            return nullptr;
    }
}

const char* BackendTypeName(WGPUBackendType type)
{
    switch (type)
    {
    case WGPUBackendType_WebGPU:   return "browser WebGPU";
    case WGPUBackendType_D3D11:    return "D3D11";
    case WGPUBackendType_D3D12:    return "D3D12";
    case WGPUBackendType_Metal:    return "Metal";
    case WGPUBackendType_Vulkan:   return "Vulkan";
    case WGPUBackendType_OpenGL:   return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGL ES";
    default:                       return "unknown backend";
    }
}

void AppendDetail(std::string& text, const char* label, const std::string& value, const std::string& name)
{
    if (!value.empty() && value != name)
    {
        text += std::format(" {} {}", label, value);
    }
}

// The init log line a user reads to see which GPU was chosen, for example
// "NVIDIA GeForce RTX 5080 (discrete GPU, D3D12, PCI vendor 0x10de) driver 32.0.16.1714".
// Only what the platform reports is printed: browsers report no adapter type
// and no PCI vendor (both read as Unknown and 0) and often no device name, so
// the name falls back to the description, then the vendor string. wgpu-native
// fills the vendor string with the driver; browsers fill it with the vendor.
std::string DescribeAdapter(const WGPUAdapterInfo& info)
{
    const std::string device = WebGpu::ToString(info.device);
    const std::string vendor = WebGpu::ToString(info.vendor);
    const std::string architecture = WebGpu::ToString(info.architecture);
    const std::string description = WebGpu::ToString(info.description);
    const std::string& name = !device.empty() ? device : !description.empty() ? description : vendor;

    std::string text = name.empty() ? std::string("unnamed adapter") : name;
    text += " (";
    if (const char* type = AdapterTypeName(info.adapterType))
    {
        text += std::format("{}, ", type);
    }
    text += BackendTypeName(info.backendType);
    if (info.vendorID != 0)
    {
        text += std::format(", PCI vendor {:#06x}", info.vendorID);
    }
    text += ")";
    AppendDetail(text, info.backendType == WGPUBackendType_WebGPU ? "vendor" : "driver", vendor, name);
    AppendDetail(text, "architecture", architecture, name);
    AppendDetail(text, "description", description, name);
    return text;
}
} // namespace

WebGpuDevice::WebGpuDevice() = default;

WebGpuDevice::~WebGpuDevice()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool WebGpuDevice::Initialize(const DeviceDesc& desc)
{
    if (m_Initialized)
    {
        return true;
    }
    m_Desc = desc;

    if (!CreateInstance(desc) || !SelectAdapter() || !CreateLogicalDevice(desc))
    {
        Shutdown();
        return false;
    }

    PopulateCapabilities();
    m_Initialized = true;
    Logger::Log::Info("WebGpuDevice: initialized on {}", m_HardwareDescription);
    return true;
}

bool WebGpuDevice::CreateInstance(const DeviceDesc& desc)
{
#if defined(__EMSCRIPTEN__)
    // Browser WebGPU has neither SPIR-V ingestion nor TimedWaitAny; requesting
    // them fails instance creation (and Dawn logs console noise). WGSL-only.
    (void)desc;
    WGPUInstanceDescriptor instanceDesc{};
    m_Instance = wgpuCreateInstance(&instanceDesc);
    m_SupportsSpirv = false;
    if (m_Instance == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: wgpuCreateInstance failed (no WebGPU in this browser?)");
        return false;
    }
    Logger::Log::Info("WebGpuDevice: browser WebGPU instance (WGSL shader path)");
    return true;
#else
    // SPIR-V ingestion has no probe API and some entries panic when queried, so
    // the feature is requested optimistically and the fallback is a WGSL-only
    // shader path (which the cook produces anyway).
    const WGPUInstanceFeatureName instanceFeatures[] = {
        WGPUInstanceFeatureName_ShaderSourceSPIRV,
        WGPUInstanceFeatureName_TimedWaitAny,
    };

    // Primary backends only: the GL backend crashes on teardown under Windows
    // and is never the intended target of this backend.
    WGPUInstanceExtras extras{};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = WGPUInstanceBackend_Primary;
    extras.flags = desc.enableDebugLayer ? WGPUInstanceFlag_Validation : WGPUInstanceFlag_Default;

    WGPUInstanceDescriptor instanceDesc{};
    instanceDesc.nextInChain = &extras.chain;
    instanceDesc.requiredFeatureCount = std::size(instanceFeatures);
    instanceDesc.requiredFeatures = instanceFeatures;

    m_Instance = wgpuCreateInstance(&instanceDesc);
    m_SupportsSpirv = m_Instance != nullptr;
    if (m_Instance == nullptr)
    {
        // Retry without the optional instance features so a runtime that lacks
        // SPIR-V ingestion still comes up on the WGSL path.
        WGPUInstanceDescriptor fallbackDesc{};
        fallbackDesc.nextInChain = &extras.chain;
        m_Instance = wgpuCreateInstance(&fallbackDesc);
        m_SupportsSpirv = false;
    }

    if (m_Instance == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: wgpuCreateInstance failed");
        return false;
    }
    if (!m_SupportsSpirv)
    {
        Logger::Log::Warning("WebGpuDevice: SPIR-V shader ingestion unavailable; shaders must be cooked to WGSL");
    }
    return true;
#endif
}

bool WebGpuDevice::SelectAdapter()
{
#if defined(__EMSCRIPTEN__)
    // Browser WebGPU has no adapter enumeration (that is a wgpu-native
    // extension); request the high-performance adapter asynchronously and pump until
    // the callback lands.
    struct RequestState
    {
        WGPUAdapter adapter = nullptr;
        bool done = false;
    } state;
    WGPURequestAdapterOptions options{};
    // Dawn returns NO adapter for the zero-initialized (Undefined) feature
    // level; Core must be requested explicitly.
    options.featureLevel = WGPUFeatureLevel_Core;
    // On a machine with an integrated and a discrete GPU the browser may hand
    // out the integrated one unless asked; this becomes
    // requestAdapter({ powerPreference: 'high-performance' }).
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo callbackInfo{};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                               void* userdata1, void* /*userdata2*/) {
        auto* request = static_cast<RequestState*>(userdata1);
        if (status == WGPURequestAdapterStatus_Success)
        {
            request->adapter = adapter;
        }
        else
        {
            Logger::Log::Error("WebGpuDevice: wgpuInstanceRequestAdapter failed: {}", WebGpu::ToString(message));
        }
        request->done = true;
    };
    callbackInfo.userdata1 = &state;
    (void)wgpuInstanceRequestAdapter(m_Instance, &options, callbackInfo);
    if (!PumpUntil(state.done))
    {
        Logger::Log::Error("WebGpuDevice: adapter request did not complete");
        return false;
    }
    m_Adapter = state.adapter;
#else
    const size_t adapterCount = wgpuInstanceEnumerateAdapters(m_Instance, nullptr, nullptr);
    if (adapterCount == 0)
    {
        Logger::Log::Error("WebGpuDevice: no WebGPU adapters available");
        return false;
    }

    std::vector<WGPUAdapter> adapters(adapterCount, nullptr);
    wgpuInstanceEnumerateAdapters(m_Instance, nullptr, adapters.data());

    uint32_t chosen = 0;
    uint32_t forcedIndex = 0;
    if (ReadAdapterIndexOverride(forcedIndex) && forcedIndex < adapterCount)
    {
        chosen = forcedIndex;
    }
    else
    {
        int bestScore = -100000;
        for (size_t i = 0; i < adapterCount; ++i)
        {
            WGPUAdapterInfo info{};
            if (wgpuAdapterGetInfo(adapters[i], &info) != WGPUStatus_Success)
            {
                continue;
            }
            const int score = RankAdapter(info);
            wgpuAdapterInfoFreeMembers(info);
            if (score > bestScore)
            {
                bestScore = score;
                chosen = static_cast<uint32_t>(i);
            }
        }
    }

    for (size_t i = 0; i < adapterCount; ++i)
    {
        if (i == chosen)
        {
            m_Adapter = adapters[i];
        }
        else
        {
            wgpuAdapterRelease(adapters[i]);
        }
    }
#endif

    if (m_Adapter == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: adapter selection produced no adapter");
        return false;
    }

    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(m_Adapter, &info) == WGPUStatus_Success)
    {
        m_HardwareDescription = DescribeAdapter(info);
        wgpuAdapterInfoFreeMembers(info);
    }

    m_AdapterLimits = WGPULimits{};
    if (wgpuAdapterGetLimits(m_Adapter, &m_AdapterLimits) != WGPUStatus_Success)
    {
        Logger::Log::Error("WebGpuDevice: wgpuAdapterGetLimits failed");
        return false;
    }
    return true;
}

bool WebGpuDevice::CreateLogicalDevice(const DeviceDesc& desc)
{
    // Ask for the adapter's own limits verbatim: they are guaranteed grantable
    // and they lift maxStorageBufferBindingSize / maxBufferSize past the 128 MB
    // WebGPU default, which the BonePalette pool (256 MB) needs.
    WGPULimits requiredLimits = m_AdapterLimits;
    requiredLimits.nextInChain = nullptr;

    std::vector<WGPUFeatureName> features;
    // GPU-generated draws select slices of shared instance buffers with a
    // nonzero firstInstance (for example terrain grass's far LOD). WebGPU
    // silently treats those draws as no-ops unless this feature is enabled.
    // Refuse an unsupported adapter instead of rendering incomplete scenes.
    if (!wgpuAdapterHasFeature(m_Adapter, WGPUFeatureName_IndirectFirstInstance))
    {
        Logger::Log::Error("WebGpuDevice: indirect-first-instance is required for GPU-generated draws");
        return false;
    }
    features.push_back(WGPUFeatureName_IndirectFirstInstance);
    // Depth clamp is core Vulkan; WebGPU gates it behind depth-clip-control,
    // and a pipeline that asks for unclippedDepth without the feature is
    // invalid — which poisons every command buffer that binds it. Request it
    // where the adapter has it; where it does not, pipelines clip (shadow
    // casters beyond the far plane clip instead of clamping — visible only in
    // extreme light frusta).
    if (wgpuAdapterHasFeature(m_Adapter, WGPUFeatureName_DepthClipControl))
    {
        features.push_back(WGPUFeatureName_DepthClipControl);
        m_SupportsDepthClipControl = true;
    }
    // Linear filtering of 32-bit float textures is a WebGPU feature, not core
    // (core rgba32float samples as UnfilterableFloat, and a layout that pairs
    // it with a filtering sampler is invalid — which poisons every command
    // buffer that binds it, e.g. the LDR post stack's colour LUTs). Request it
    // where the adapter has it; where it does not, the capability flag steers
    // float32 consumers (LUT uploads) onto float16.
    if (wgpuAdapterHasFeature(m_Adapter, WGPUFeatureName_Float32Filterable))
    {
        features.push_back(WGPUFeatureName_Float32Filterable);
        m_SupportsFloat32Filterable = true;
    }
    // GPU timings. Optional, and absent unless the browser is told to expose it
    // (Chrome hides timestamps without --enable-webgpu-developer-features);
    // without the feature the query pool simply never exists and the profiler
    // shows CPU timings alone.
    if (wgpuAdapterHasFeature(m_Adapter, WGPUFeatureName_TimestampQuery))
    {
        features.push_back(WGPUFeatureName_TimestampQuery);
        m_SupportsTimestampQuery = true;
    }
#if !defined(__EMSCRIPTEN__)
    // Engine push constants ride wgpu's immediates when the adapter has them.
    // Browser WebGPU has no immediates feature; m_SupportsImmediates stays
    // false and SetConstants routes through the emulation ring.
    // GE_WEBGPU_EMULATE_PUSH_CONSTANTS=1 takes the browser's ring on the
    // desktop too, so its limits reproduce outside a browser; the shaders must
    // then be WGSL that reads the block at kPushConstantEmulationGroup, as the
    // shader cook writes it.
    const char* emulate = std::getenv("GE_WEBGPU_EMULATE_PUSH_CONSTANTS");
    const bool emulatePushConstants = emulate != nullptr && emulate[0] != '\0' && emulate[0] != '0';
    if (!emulatePushConstants &&
        wgpuAdapterHasFeature(m_Adapter, static_cast<WGPUFeatureName>(WGPUNativeFeature_Immediates)))
    {
        features.push_back(static_cast<WGPUFeatureName>(WGPUNativeFeature_Immediates));
        m_SupportsImmediates = true;
    }
#endif

    WGPUDeviceDescriptor deviceDesc{};
    deviceDesc.label = WebGpu::MakeStringView(desc.applicationName.c_str());
    deviceDesc.requiredLimits = &requiredLimits;
    deviceDesc.requiredFeatureCount = features.size();
    deviceDesc.requiredFeatures = features.empty() ? nullptr : features.data();
    deviceDesc.uncapturedErrorCallbackInfo.callback = &WebGpuDevice::OnUncapturedError;
    deviceDesc.uncapturedErrorCallbackInfo.userdata1 = this;
    deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceDesc.deviceLostCallbackInfo.callback = &WebGpuDevice::OnDeviceLost;
    deviceDesc.deviceLostCallbackInfo.userdata1 = this;

    struct DeviceRequest
    {
        WGPUDevice device = nullptr;
        bool done = false;
    } request;

    WGPURequestDeviceCallbackInfo callbackInfo{};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.userdata1 = &request;
    callbackInfo.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                               void* userdata1, void*) {
        auto* result = static_cast<DeviceRequest*>(userdata1);
        if (status == WGPURequestDeviceStatus_Success)
        {
            result->device = device;
        }
        else
        {
            Logger::Log::Error("WebGpuDevice: wgpuAdapterRequestDevice failed: {}", WebGpu::ToString(message));
        }
        // Written last: the pump reads it unsynchronized on this thread.
        result->done = true;
    };

    (void)wgpuAdapterRequestDevice(m_Adapter, &deviceDesc, callbackInfo);
    if (!PumpUntil(request.done) || request.device == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: device request did not complete");
        return false;
    }

    m_Device = request.device;
    m_Queue = wgpuDeviceGetQueue(m_Device);
    if (m_Queue == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: wgpuDeviceGetQueue returned null");
        return false;
    }

    // Browser WebGPU has no encoder-level timestamp write (writeTimestamp left
    // the spec and survives only as a Dawn-native extension), so there the
    // pool maps each span onto pass-boundary timestampWrites: the begin rides
    // the next pass, the end is the last pass recorded before the span closes
    // (WebGpuCommandList::NoteTimestamp). The native wgpu backend writes on the
    // encoder directly.
    if (m_SupportsTimestampQuery)
    {
        IQueryPool::Config qc{};
        qc.FramesInFlight = std::max(2u, GetFramesInFlight());
#if defined(__EMSCRIPTEN__)
        // Two queries per pass, every pass, per frame slot.
        qc.MaxTimestampQueries = 3072;
#endif
        auto pool = std::make_unique<WebGpuQueryPool>(*this, m_Device, qc);
        if (pool->Initialize())
            m_QueryPool = std::move(pool);
#if defined(__EMSCRIPTEN__)
        Logger::Log::Info("WebGpuDevice: GPU pass timings through pass-boundary timestamp writes");
#endif
    }

    m_DeviceLimits = WGPULimits{};
    if (wgpuDeviceGetLimits(m_Device, &m_DeviceLimits) != WGPUStatus_Success)
    {
        Logger::Log::Error("WebGpuDevice: wgpuDeviceGetLimits failed");
        return false;
    }

    if (m_DeviceLimits.maxStorageBufferBindingSize < kDesiredStorageBufferBindingSize)
    {
        Logger::Log::Warning(
            "WebGpuDevice: granted maxStorageBufferBindingSize is {} MB (wanted {} MB); "
            "storage pools must clamp to the granted value",
            m_DeviceLimits.maxStorageBufferBindingSize / (1024ull * 1024ull),
            kDesiredStorageBufferBindingSize / (1024ull * 1024ull));
    }

    ReadFlushDiagnosticSettings();
    return true;
}

void WebGpuDevice::PopulateCapabilities()
{
    RenderingDeviceCapabilities caps{};

    // WebGPU has none of the GPU-driven desktop machinery; the compatibility
    // renderer profile reads these and takes the classic paths instead.
    caps.supportsBindlessResources = false;
    caps.supportsDescriptorBuffer = false;
    caps.supportsBufferDeviceAddress = false;
    caps.supportsDrawIndirectCountNative = false;
    caps.supportsShaderInt64 = false;
    caps.supportsMeshShaders = false;
    caps.supportsRayTracing = false;
    caps.supportsRayQuery = false;
    caps.supportsWorkGraphs = false;
    caps.supportsVariableRateShading = false;
    // WGPUBufferUsage_MapRead composes only with CopyDst, so no buffer can be
    // both a dispatch's storage target and CPU-mapped.
    caps.supportsMappableStorageBuffers = false;
    // WebGPU's storage-capable texture formats are a fixed list that includes
    // rgba16float and excludes rg16float.
    caps.supportsRG16FloatStorage = false;
    caps.supportsNarrowStorageFormats = false;
    caps.supportsAliasedStorageTextureBindings = false;
    caps.supportsFilterableFloat32 = m_SupportsFloat32Filterable;
#if defined(__EMSCRIPTEN__)
    // emdawnwebgpu resolves a handle through a per-thread JS object table, so a
    // device created on the main thread is absent in a worker and the first
    // call from one aborts that worker. wgpu-native has no such confinement.
    caps.supportsMultithreadedResourceCreation = false;
#endif
    caps.supportsDualSourceBlending =
        wgpuDeviceHasFeature(m_Device, WGPUFeatureName_DualSourceBlending) != 0;
    caps.supportsTextureCompressionBC =
        wgpuDeviceHasFeature(m_Device, WGPUFeatureName_TextureCompressionBC) != 0;

    caps.maxBindlessTextures = 0;
    caps.maxBindlessBuffers = 0;
    caps.maxPerStageSamplers = m_DeviceLimits.maxSamplersPerShaderStage;
    caps.maxPerStageSampledImages = m_DeviceLimits.maxSampledTexturesPerShaderStage;
    caps.maxPerStageResources = m_DeviceLimits.maxBindingsPerBindGroup;
    caps.maxPerStageStorageBuffers = m_DeviceLimits.maxStorageBuffersPerShaderStage;
    caps.maxComputeWorkGroupSize = m_DeviceLimits.maxComputeWorkgroupSizeX;
    caps.maxComputeSharedMemorySize = m_DeviceLimits.maxComputeWorkgroupStorageSize;
    // WebGPU core guarantees 4x MSAA only.
    caps.maxMSAASamples = 4;
    // WebGPU exposes no anisotropy limit: a trilinear sampler may request up to
    // the documented 16 and the implementation clamps to what the platform
    // supports.
    caps.maxSamplerAnisotropy = 16.0f;
    caps.minStorageBufferOffsetAlignment = m_DeviceLimits.minStorageBufferOffsetAlignment;
    // The granted value, not the requested one: growable storage pools clamp
    // their ceilings to this, which is what turns a short grant into a crowd
    // cap instead of an unbindable buffer.
    caps.maxStorageBufferBindingSize =
        static_cast<size_t>(m_DeviceLimits.maxStorageBufferBindingSize);
    // Multi-row buffer<->texture copies must use a 256-aligned row pitch.
    // Upload callers that cannot are repacked by WebGpuCommandList; readback
    // callers own the pitch of the buffer they are about to map.
    caps.textureCopyRowPitchAlignment = kWebGpuCopyBytesPerRowAlignment;
    // Core WebGPU has no 16-bit UNORM color formats (texture-formats-tier1).
    caps.supportsUnorm16TextureFormats = false;

    // No render-pass resolve-mode selection exists in WebGPU: colour resolves
    // are averaged and depth/stencil resolves are unavailable.
    caps.supportedDepthResolveModes = 0;
    caps.supportedStencilResolveModes = 0;
    caps.supportsIndependentStencilResolve = false;

    caps.preferredDepthAndStencilFormat =
        wgpuDeviceHasFeature(m_Device, WGPUFeatureName_Depth32FloatStencil8) != 0
            ? TextureFormat::D32_SFLOAT_S8_UINT
            : TextureFormat::D32_FLOAT;

    m_Capabilities = caps;
}

void WebGpuDevice::PrintCapabilityReport() const
{
    Logger::Log::Info("WebGPU capability report");
    Logger::Log::Info("  adapter                      : {}", m_HardwareDescription);
    Logger::Log::Info("  SPIR-V ingestion             : {}", m_SupportsSpirv ? "yes" : "no (WGSL only)");
    Logger::Log::Info("  immediates (push constants)  : {}", m_SupportsImmediates ? "yes" : "no");
    Logger::Log::Info("  maxBindGroups                : {}", m_DeviceLimits.maxBindGroups);
    Logger::Log::Info("  maxBindingsPerBindGroup      : {}", m_DeviceLimits.maxBindingsPerBindGroup);
    Logger::Log::Info("  maxStorageBufferBindingSize  : {} MB",
                      m_DeviceLimits.maxStorageBufferBindingSize / (1024ull * 1024ull));
    Logger::Log::Info("  maxUniformBufferBindingSize  : {} KB",
                      m_DeviceLimits.maxUniformBufferBindingSize / 1024ull);
    Logger::Log::Info("  maxBufferSize                : {} MB",
                      m_DeviceLimits.maxBufferSize / (1024ull * 1024ull));
    Logger::Log::Info("  maxVertexBuffers/attributes  : {} / {}", m_DeviceLimits.maxVertexBuffers,
                      m_DeviceLimits.maxVertexAttributes);
    Logger::Log::Info("  bindless / BDA / indirect-cnt: no / no / no");
}

void WebGpuDevice::OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message,
                                     void* /*userdata1*/, void* /*userdata2*/)
{
    Logger::Log::Error("WebGPU error ({}): {}", static_cast<uint32_t>(type), WebGpu::ToString(message));
}

void WebGpuDevice::OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                                void* /*userdata1*/, void* /*userdata2*/)
{
    // Destroyed is our own Shutdown; CallbackCancelled is the instance released before the
    // device-lost callback could fire, which only a shutdown does. Neither is a loss.
    if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
    {
        return;
    }
    Logger::Log::Error("WebGPU device lost ({}): {}", static_cast<uint32_t>(reason), WebGpu::ToString(message));
}

void WebGpuDevice::Shutdown()
{
    if (m_Instance == nullptr)
    {
        return;
    }

    InvokePerDeviceCacheCleanups();

    if (m_Device != nullptr)
    {
        WaitForIdle();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        m_WindowTargets.ForEach([this](Handle, std::unique_ptr<WebGpuSwapchain>& swapchain) {
            if (swapchain)
            {
                swapchain->Shutdown(*this);
            }
        });
        m_WindowTargets.Clear();
        m_ActiveWindowTarget = WindowTargetHandle{};

        m_DescriptorSets.ForEach([](Handle, WebGpuDescriptorSet& set) {
            if (set.bindGroup != nullptr)
            {
                wgpuBindGroupRelease(set.bindGroup);
                set.bindGroup = nullptr;
            }
        });
        m_DescriptorSets.Clear();

        m_Pipelines.ForEach([](Handle, WebGpuPipeline& pipeline) {
            if (pipeline.renderPipeline != nullptr)  wgpuRenderPipelineRelease(pipeline.renderPipeline);
            if (pipeline.renderPipelineDepthReadOnly != nullptr)
                wgpuRenderPipelineRelease(pipeline.renderPipelineDepthReadOnly);
            if (pipeline.computePipeline != nullptr) wgpuComputePipelineRelease(pipeline.computePipeline);
            if (pipeline.pipelineLayout != nullptr)  wgpuPipelineLayoutRelease(pipeline.pipelineLayout);
            pipeline.renderPipeline = nullptr;
            pipeline.renderPipelineDepthReadOnly = nullptr;
            pipeline.computePipeline = nullptr;
            pipeline.pipelineLayout = nullptr;
        });
        m_Pipelines.Clear();

        m_TextureViews.ForEach([](Handle, WebGpuTextureView& view) {
            if (view.view != nullptr) wgpuTextureViewRelease(view.view);
            view.view = nullptr;
        });
        m_TextureViews.Clear();

        m_Samplers.ForEach([](Handle, WebGpuSampler& sampler) {
            if (sampler.sampler != nullptr) wgpuSamplerRelease(sampler.sampler);
            sampler.sampler = nullptr;
        });
        m_Samplers.Clear();

        m_Textures.ForEach([](Handle, WebGpuTexture& texture) {
            if (texture.isSurfaceSlot)
            {
                return;
            }
            if (texture.defaultView != nullptr) wgpuTextureViewRelease(texture.defaultView);
            if (texture.texture != nullptr)     wgpuTextureRelease(texture.texture);
            texture.defaultView = nullptr;
            texture.texture = nullptr;
        });
        m_Textures.Clear();

        m_Buffers.ForEach([](Handle, WebGpuBuffer& buffer) {
            if (buffer.buffer != nullptr)
            {
                wgpuBufferDestroy(buffer.buffer);
                wgpuBufferRelease(buffer.buffer);
            }
            buffer.buffer = nullptr;
        });
        m_Buffers.Clear();

        ReleasePushConstantRing();
        if (m_PushConstantBindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(m_PushConstantBindGroupLayout);
            m_PushConstantBindGroupLayout = nullptr;
        }
        if (m_EmptyBindGroup != nullptr)
        {
            wgpuBindGroupRelease(m_EmptyBindGroup);
            m_EmptyBindGroup = nullptr;
        }
        if (m_EmptyBindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(m_EmptyBindGroupLayout);
            m_EmptyBindGroupLayout = nullptr;
        }

        for (auto& [id, layout] : m_BindGroupLayouts)
        {
            wgpuBindGroupLayoutRelease(layout);
        }
        m_BindGroupLayouts.clear();
    }

    if (m_Queue != nullptr)   { wgpuQueueRelease(m_Queue);     m_Queue = nullptr; }
    if (m_Device != nullptr)  { wgpuDeviceRelease(m_Device);   m_Device = nullptr; }
    if (m_Adapter != nullptr) { wgpuAdapterRelease(m_Adapter); m_Adapter = nullptr; }
    wgpuInstanceRelease(m_Instance);
    m_Instance = nullptr;
    m_Initialized = false;
}

// ---------------------------------------------------------------------------
// Submission and synchronization
// ---------------------------------------------------------------------------

bool WebGpuDevice::PumpUntil(const bool& done)
{
    // wgpuInstanceWaitAny is unimplemented in wgpu-native v29 — it PANICS
    // ("not implemented") rather than returning an error — so completion is
    // observed through a flag the callback writes while this pump drives the
    // event queue. Callbacks therefore use AllowProcessEvents, never
    // WaitAnyOnly. Once a device exists, DevicePoll(wait) also retires
    // submissions, which is what mapAsync completions ride on.
    for (uint32_t attempt = 0; attempt < kFutureWaitAttempts && !done; ++attempt)
    {
        wgpuInstanceProcessEvents(m_Instance);
#if defined(__EMSCRIPTEN__)
        // Browser promises resolve only when the JS event loop runs; an
        // ASYNCIFY sleep yields to it, then ProcessEvents delivers the
        // callback. Without the yield this loop spins forever.
        emscripten_sleep(1);
#else
        // wgpu-native only: DevicePoll(wait) retires submissions so mapAsync
        // completions land. The browser has no blocking poll; the yield above
        // covers it.
        if (m_Device != nullptr)
        {
            wgpuDevicePoll(m_Device, /*wait*/ true, nullptr);
        }
#endif
    }
    return done;
}

std::unique_ptr<CommandList> WebGpuDevice::CreateCommandList(QueueType queue)
{
    if (m_Device == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<WebGpuCommandList>(*this, queue);
}

void WebGpuDevice::ExecuteCommandLists(const std::vector<CommandList*>& commandLists)
{
    if (m_Queue == nullptr || commandLists.empty())
    {
        return;
    }

    std::vector<WGPUCommandBuffer> buffers;
    buffers.reserve(commandLists.size());
    for (CommandList* list : commandLists)
    {
        auto* webgpuList = static_cast<WebGpuCommandList*>(list);
        if (webgpuList == nullptr)
        {
            continue;
        }
        if (WGPUCommandBuffer buffer = webgpuList->Detach())
        {
            buffers.push_back(buffer);
        }
    }

    if (buffers.empty())
    {
        return;
    }

    // Before the submit, never after: a queue write is ordered against the
    // commands submitted after it, and these commands read what the frame just
    // wrote through its persistent mappings.
    FlushMappedShadows();
    wgpuQueueSubmit(m_Queue, buffers.size(), buffers.data());
    for (WGPUCommandBuffer buffer : buffers)
    {
        wgpuCommandBufferRelease(buffer);
    }
}

SemaphoreHandle WebGpuDevice::CreateTimelineSemaphore(uint64_t initialValue)
{
    WebGpuSemaphore sem{};
    sem.Value = std::make_shared<std::atomic<uint64_t>>(initialValue);
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<SemaphoreTag>(m_Semaphores.Create(std::move(sem)));
}

void WebGpuDevice::DestroySemaphore(SemaphoreHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (m_Semaphores.Get(ToGeneric(handle)) != nullptr)
    {
        m_Semaphores.Destroy(ToGeneric(handle));
    }
}

bool WebGpuDevice::GetTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t& outValue) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
    if (sem == nullptr || sem->Value == nullptr)
    {
        return false;
    }
    outValue = sem->Value->load(std::memory_order_acquire);
    return true;
}

bool WebGpuDevice::WaitTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t value, uint64_t /*timeoutNs*/)
{
    std::shared_ptr<std::atomic<uint64_t>> counter;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        const WebGpuSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
        if (sem == nullptr || sem->Value == nullptr)
        {
            return false;
        }
        counter = sem->Value;
    }
    // Signals land from wgpuQueueOnSubmittedWorkDone callbacks, which need the
    // event loop; drive it with the same bounded pump the futures use.
    bool done = counter->load(std::memory_order_acquire) >= value;
    for (uint32_t attempt = 0; attempt < kFutureWaitAttempts && !done; ++attempt)
    {
        wgpuInstanceProcessEvents(m_Instance);
#if defined(__EMSCRIPTEN__)
        emscripten_sleep(1);
#else
        if (m_Device != nullptr)
        {
            wgpuDevicePoll(m_Device, /*wait*/ true, nullptr);
        }
#endif
        done = counter->load(std::memory_order_acquire) >= value;
    }
    return done;
}

namespace
{
// One pending signal registration: bump the timeline to at least Target when
// the submission the callback was registered after completes.
struct WebGpuTimelineSignal
{
    std::shared_ptr<std::atomic<uint64_t>> Value;
    uint64_t Target = 0;
};

void OnTimelineSubmitDone(WGPUQueueWorkDoneStatus /*status*/, WGPUStringView /*message*/, void* userdata1,
                          void* /*userdata2*/)
{
    auto* signal = static_cast<WebGpuTimelineSignal*>(userdata1);
    if (signal != nullptr)
    {
        uint64_t current = signal->Value->load(std::memory_order_relaxed);
        while (current < signal->Target &&
               !signal->Value->compare_exchange_weak(current, signal->Target, std::memory_order_release))
        {
        }
        delete signal;
    }
}
} // namespace

bool WebGpuDevice::QueueSubmit(QueueType /*queue*/,
                               const std::vector<CommandList*>& cmdLists,
                               const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                               const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores)
{
    if (m_Queue == nullptr)
    {
        return false;
    }
    // Waits are ordering no-ops here: WebGPU exposes one in-order queue, and
    // the render graph submits in dependency order, so anything a wait would
    // target was already enqueued ahead of this submission.
    (void)waitSemaphores;

    ExecuteCommandLists(cmdLists);

    for (const auto& [handle, value] : signalSemaphores)
    {
        std::shared_ptr<std::atomic<uint64_t>> counter;
        {
            std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
            const WebGpuSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
            if (sem == nullptr || sem->Value == nullptr)
            {
                continue;
            }
            counter = sem->Value;
        }
        auto* signal = new WebGpuTimelineSignal{std::move(counter), value};
        WGPUQueueWorkDoneCallbackInfo callbackInfo{};
        callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        callbackInfo.callback = &OnTimelineSubmitDone;
        callbackInfo.userdata1 = signal;
        wgpuQueueOnSubmittedWorkDone(m_Queue, callbackInfo);
    }
    return true;
}

void WebGpuDevice::WaitForIdle()
{
    if (m_Device == nullptr)
    {
        return;
    }
#if !defined(__EMSCRIPTEN__)
    // wgpuDevicePoll(wait) blocks until every submission has retired and runs
    // the pending callbacks, which is exactly the drain semantic callers want.
    wgpuDevicePoll(m_Device, /*wait*/ true, nullptr);
#endif
    // Browser WebGPU cannot block on the GPU; ProcessEvents runs whatever has
    // already completed and the rest lands via the event loop.
    wgpuInstanceProcessEvents(m_Instance);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

bool WebGpuDevice::BeginFrame()
{
    if (!m_Initialized)
    {
        return false;
    }

    // Drives wgpu's callback queue: surface-texture releases and mapping
    // completions land here.
    wgpuInstanceProcessEvents(m_Instance);

    // Sync the timestamp pool to this frame slot. ProcessEvents above is what
    // delivers the map completion for the slot's PREVIOUS turn, so the results
    // this reads are already in hand — nothing here waits.
    if (m_QueryPool)
        m_QueryPool->BeginFrame(m_FrameIndex);

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);

    // Recycle transient descriptor sets created the last time this slot was
    // active — kFramesInFlight frames ago, so their GPU work has retired. The
    // recursive mutex lets DestroyDescriptorSet re-enter; swap the bucket out
    // first so a set's destroy can't perturb the vector being iterated.
    {
        std::vector<DescriptorSetHandle> pending;
        pending.swap(m_TransientDescriptorSets[m_FrameIndex]);
        for (DescriptorSetHandle handle : pending)
        {
            DestroyDescriptorSet(handle);
        }
    }

    ReportPushConstantRefusals();
    if (m_PushConstantSliceBytes != 0)
    {
        m_PushConstantOffset = static_cast<uint64_t>(m_FrameIndex) * m_PushConstantSliceBytes;
    }
    m_WindowTargets.ForEach([](Handle, std::unique_ptr<WebGpuSwapchain>& swapchain) {
        if (swapchain)
        {
            swapchain->DrainCompletedReleases();
        }
    });

    if (WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget))
    {
        return swapchain->Acquire(*this);
    }
    return true;
}

void WebGpuDevice::Present()
{
    // Resolve + map this frame's timestamps AFTER its work is queued, so queue
    // order puts the resolve behind the writes. Without this the writes pile up
    // against a set that is never resolved and buffers that are never mapped.
    if (m_QueryPool)
        m_QueryPool->EndFrame();

    if (WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget))
    {
        swapchain->Present(*this);
    }
    FinalizeFrame();
}

void WebGpuDevice::FinalizeFrame()
{
    m_FrameIndex = (m_FrameIndex + 1) % kFramesInFlight;
}

// ---------------------------------------------------------------------------
// Window targets
// ---------------------------------------------------------------------------

WindowTargetHandle WebGpuDevice::CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height)
{
    if (!m_Initialized || windowHandle == nullptr)
    {
        return WindowTargetHandle{};
    }

    uint32_t pixelWidth = width;
    uint32_t pixelHeight = height;
    WebGpuSurfaceBridge::GetWindowPixelSize(windowHandle, pixelWidth, pixelHeight);

    auto swapchain = std::make_unique<WebGpuSwapchain>();
    if (!swapchain->Initialize(*this, windowHandle, pixelWidth, pixelHeight, m_Desc.vsync))
    {
        return WindowTargetHandle{};
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<WindowTargetTag>(m_WindowTargets.Create(std::move(swapchain)));
}

bool WebGpuDevice::DestroyWindowTarget(WindowTargetHandle target)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    auto* slot = m_WindowTargets.Get(ToGeneric(target));
    if (slot == nullptr || !*slot)
    {
        return false;
    }
    WaitForIdle();
    (*slot)->Shutdown(*this);
    m_WindowTargets.Destroy(ToGeneric(target));
    if (m_ActiveWindowTarget == target)
    {
        m_ActiveWindowTarget = WindowTargetHandle{};
    }
    return true;
}

bool WebGpuDevice::SetActiveWindowTarget(WindowTargetHandle target)
{
    if (GetSwapchain(target) == nullptr)
    {
        return false;
    }
    m_ActiveWindowTarget = target;
    return true;
}

bool WebGpuDevice::RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height)
{
    WebGpuSwapchain* swapchain = GetSwapchain(target);
    if (swapchain == nullptr)
    {
        return false;
    }
    WaitForIdle();
    return swapchain->Resize(*this, width, height);
}

bool WebGpuDevice::GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const
{
    const WebGpuSwapchain* swapchain = GetSwapchain(target);
    if (swapchain == nullptr)
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    outWidth = swapchain->GetWidth();
    outHeight = swapchain->GetHeight();
    return true;
}

bool WebGpuDevice::GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    return GetWindowTargetSize(m_ActiveWindowTarget, outWidth, outHeight);
}

bool WebGpuDevice::AcquireNextImage(uint32_t& imageIndex)
{
    imageIndex = 0;
    WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr && swapchain->Acquire(*this);
}

bool WebGpuDevice::PresentImage(uint32_t /*imageIndex*/)
{
    WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr && swapchain->Present(*this);
}

uint32_t WebGpuDevice::GetSwapchainImageCount() const
{
    // WebGPU hands out one surface texture at a time and keeps its buffering
    // internal, so exactly one addressable slot exists.
    return GetSwapchain(m_ActiveWindowTarget) != nullptr ? 1u : 0u;
}

TextureHandle WebGpuDevice::GetSwapchainImage(uint32_t index) const
{
    const WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    if (swapchain == nullptr || index != 0)
    {
        return TextureHandle{};
    }
    return swapchain->GetSlotTexture();
}

TextureHandle WebGpuDevice::GetCurrentSwapchainImageHandle()
{
    return GetSwapchainImage(0);
}

TextureFormat WebGpuDevice::GetSwapchainTextureFormat() const
{
    const WebGpuSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr ? swapchain->GetFormat() : TextureFormat::Unknown;
}

void WebGpuDevice::SetVsync(bool vsync)
{
    m_Desc.vsync = vsync;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_WindowTargets.ForEach([this, vsync](Handle, std::unique_ptr<WebGpuSwapchain>& swapchain) {
        if (swapchain)
        {
            swapchain->SetVsync(*this, vsync);
        }
    });
}

WebGpuSwapchain* WebGpuDevice::GetSwapchain(WindowTargetHandle target)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    auto* slot = m_WindowTargets.Get(ToGeneric(target));
    return (slot != nullptr && *slot) ? slot->get() : nullptr;
}

const WebGpuSwapchain* WebGpuDevice::GetSwapchain(WindowTargetHandle target) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const auto* slot = m_WindowTargets.Get(ToGeneric(target));
    return (slot != nullptr && *slot) ? slot->get() : nullptr;
}

} // namespace GameEngine::Rendering
