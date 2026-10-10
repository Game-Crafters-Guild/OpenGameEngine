#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include <functional>

namespace GameEngine
{
namespace Rendering
{

// Forward declarations
class CommandList;
class IAccelerationStructureBackend;

class SpecializationConstants;
class ResourceManager;
class MemoryAllocator;
class IQueryPool;
class PipelineCache;
class PipelineBuildTable;
enum class TextureFormat : uint32_t;
// New pipeline desc types — defined in PipelineTypes.h. Forward-declared
// here so IDevice can take them by const-ref without pulling in the
// full PipelineTypes.h definitions (which would create an include cycle
// because the desc types reference state structs defined below).
struct GraphicsPipelineDesc;
struct ComputePipelineDesc;

// CPU source bytes are consumed before UpdateBufferRanges returns. Ranges may
// be disjoint; callers retain the same frame/fence ownership as UpdateBuffer.
// A batch is all or nothing: when any non-empty range runs past the buffer or
// has no data, no range of the batch is written. Empty ranges are skipped.
struct BufferUpdateRange
{
    size_t offset = 0;
    size_t size = 0;
    const void* data = nullptr;
};

// Resource handles are now generational handles for memory safety and performance
// (Handle types are defined in Handle.h)

// Graphics API enumeration
enum class GraphicsAPI
{
    Vulkan,
    DirectX12,
    Metal,
    WebGPU, // wgpu-native on desktop, the browser's WebGPU under Emscripten
    Auto    // Automatically select best available
};

// Which cooked shader form a device ingests. A .shaderpkg can carry a SPIR-V
// and a WGSL chunk for the same stage; this says which of the two the shader
// loaders must serve so pipeline creation gets bytes the backend can read.
enum class ShaderSourceKind : uint8_t
{
    SpirV,
    Wgsl
};

// Device-loss health state. Healthy is the steady
// state and carries no cost. Hung means a fence wait timed out on a finite-wait
// platform (macOS/Linux): the GPU is alive but behind and the state is recovered
// by polling. Lost means the backend reported a real device loss on some op;
// rendering is suppressed until the recovery machine rebuilds.
//
// The remaining states drive the Tier-2 in-place rebuild (slice 2):
//   Rebuilding          — a rebuild is running on the render thread; device-scoped
//                         Vulkan objects are being replaced. No work is issued.
//   AwaitingReprovision — the device was rebuilt and is fully functional, but the
//                         upper layers (RenderServices caches, ECS component-
//                         resident handles) still hold dead handles from the old
//                         device. Rendering stays suppressed until a re-provision
//                         pass completes and calls NotifyReprovisionComplete()
//                         (slice 3a owns that consumer). Direct device GPU ops
//                         (buffer create/upload, submit, fence) work here.
//   Failed              — the rebuild exhausted its bounded retries (a persistently
//                         dying GPU). No healthy device; only save-and-restart is
//                         viable (slice 6 owns the UX).
// The backend reads this once per window per frame plus on error paths, so a
// healthy steady state pays nothing.
enum class DeviceHealth : uint8_t
{
    Healthy,
    Hung,
    Lost,
    Rebuilding,
    AwaitingReprovision,
    Failed
};

inline const char* DeviceHealthToString(DeviceHealth health)
{
    switch (health)
    {
    case DeviceHealth::Healthy:             return "Healthy";
    case DeviceHealth::Hung:                return "Hung";
    case DeviceHealth::Lost:                return "Lost";
    case DeviceHealth::Rebuilding:          return "Rebuilding";
    case DeviceHealth::AwaitingReprovision: return "AwaitingReprovision";
    case DeviceHealth::Failed:              return "Failed";
    default:                                return "Healthy";
    }
}

// Descriptor-buffer binding mode. The legacy descriptor-pool path remains
// available for devices without descriptor-buffer support; production
// callers should leave this at Auto so a capable device picks the DB path
// automatically. Disabled forces the pool path on every layout, primarily
// for unit tests and bisection.
enum class DescriptorBufferMode
{
    Auto,
    Disabled
};

enum class HdrOutputMode : uint8_t
{
    Off,
    Auto,
    HDR10_PQ,
    HLG,
    ScRGB,
    HDR10Plus
};

enum class HdrSwapchainBitDepth : uint8_t
{
    Bit10,
    Float16
};

struct HdrStaticMetadata
{
    std::array<float, 2> redPrimary = {0.708f, 0.292f};
    std::array<float, 2> greenPrimary = {0.170f, 0.797f};
    std::array<float, 2> bluePrimary = {0.131f, 0.046f};
    std::array<float, 2> whitePoint = {0.3127f, 0.3290f};
    float maxMasteringLuminance = 1000.0f;
    float minMasteringLuminance = 0.001f;
    float maxContentLightLevel = 1000.0f;
    float maxFrameAverageLightLevel = 400.0f;
    float paperWhiteNits = 203.0f;

    /// Gates whether an HDR apply reaches the device, so exact float comparison
    /// is intended: these values are handed to the driver unchanged, and any
    /// difference is a real metadata change rather than rounding noise.
    /// Defaulted so a field added above is compared without touching the
    /// callers that decide "did the request change".
    friend bool operator==(const HdrStaticMetadata&, const HdrStaticMetadata&) = default;
};

struct HdrDisplayInfo
{
    std::string id;
    std::string name;
    bool activeMonitor = false;
    bool hdrAvailable = false;
    bool hdrActive = false;
    bool supportsHDR10_PQ = false;
    bool supportsHLG = false;
    bool supportsScRGB = false;
    bool supportsHDR10Plus = false;
    TextureFormat swapchainFormat = static_cast<TextureFormat>(0);
    // Raw negotiated swapchain pixel format / color space as reported by the
    // backend API in its own format and color-space enumerations (0 before a
    // swapchain exists). Diagnostic facts for the output-chain audit surface —
    // engine logic keys off swapchainFormat / resolvedMode instead.
    uint32_t nativeFormat = 0;
    uint32_t nativeColorSpace = 0;
    HdrSwapchainBitDepth swapchainBitDepth = HdrSwapchainBitDepth::Bit10;
    float maxLuminance = 0.0f;
    float minLuminance = 0.0f;
    float maxFullFrameLuminance = 0.0f;
    float paperWhiteNits = 203.0f;
    float outputMaxLinearValue = 1.0f;
    double refreshRate = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    HdrOutputMode requestedMode = HdrOutputMode::Off;
    HdrOutputMode resolvedMode = HdrOutputMode::Off;
    std::vector<std::string> colorSpaces;
    std::vector<std::string> diagnosticHints;
};

struct HdrOutputState
{
    bool enabled = false;
    HdrOutputMode requestedMode = HdrOutputMode::HDR10_PQ;
    HdrOutputMode activeMode = HdrOutputMode::Off;
    HdrSwapchainBitDepth swapchainBitDepth = HdrSwapchainBitDepth::Bit10;
    int targetDisplay = -1;
    HdrStaticMetadata staticMetadata{};
    HdrDisplayInfo display{};
};

inline const char* HdrOutputModeToString(HdrOutputMode mode)
{
    switch (mode)
    {
    case HdrOutputMode::Off:       return "Off";
    case HdrOutputMode::Auto:      return "Auto";
    case HdrOutputMode::HDR10_PQ:  return "HDR10_PQ";
    case HdrOutputMode::HLG:       return "HLG";
    case HdrOutputMode::ScRGB:     return "scRGB";
    case HdrOutputMode::HDR10Plus: return "HDR10Plus";
    default:                       return "Off";
    }
}

// Parses a mode name, distinguishing "spelled Off" from "not a mode at all".
// Callers that treat an unrecognized value as an operator error (a typo in a
// launch flag or env var silently disabling HDR) need that distinction;
// HdrOutputModeFromString below collapses both to Off for callers that don't.
inline std::optional<HdrOutputMode> TryParseHdrOutputMode(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Strip separators and ALL whitespace, not just spaces: env values routinely
    // carry a trailing CR from CRLF files, shell pipelines and CI scripts, and a
    // value refused over invisible whitespace is worse than one refused outright
    // — the caller falls back to its default and the operator sees no reason why.
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char c) {
                                        return c == '-' || c == '_' || std::isspace(c) != 0;
                                    }),
                     normalized.end());

    // The two boolean sets are kept symmetric on purpose: an operator who learns
    // "Off" works will reach for "On", and a refusal here does not mean "no" — it
    // means "whatever the project said", which is the opposite of an override.
    if (normalized == "off" || normalized == "0" || normalized == "false" ||
        normalized == "no" || normalized == "none" || normalized == "disabled" ||
        normalized == "sdr")
        return HdrOutputMode::Off;
    if (normalized == "auto" || normalized == "on" || normalized == "1" ||
        normalized == "true" || normalized == "yes" || normalized == "enabled")
        return HdrOutputMode::Auto;
    if (normalized == "hdr10pq" || normalized == "pq" || normalized == "st2084" || normalized == "hdr10")
        return HdrOutputMode::HDR10_PQ;
    if (normalized == "hlg") return HdrOutputMode::HLG;
    if (normalized == "scrgb" || normalized == "extendedsrgb") return HdrOutputMode::ScRGB;
    if (normalized == "hdr10plus" || normalized == "hdr10+") return HdrOutputMode::HDR10Plus;
    return std::nullopt;
}

inline HdrOutputMode HdrOutputModeFromString(std::string_view value)
{
    return TryParseHdrOutputMode(value).value_or(HdrOutputMode::Off);
}

inline bool IsHdrOutputModeActive(HdrOutputMode mode)
{
    return mode == HdrOutputMode::HDR10_PQ || mode == HdrOutputMode::HLG ||
           mode == HdrOutputMode::ScRGB || mode == HdrOutputMode::HDR10Plus;
}

// Canonical HDR paper-white floor: the Windows scRGB anchor where framebuffer
// 1.0 == 80 nits (see GetScRGBFramebufferWhiteNits). The single source for the "80 nit" floor shared by the output-encode and
// UI shaders (encode_srgb.frag / tonemap.frag / ui_sdf_common.glsl mirror this in
// GLSL since the two languages can't share a constant).
inline constexpr float kHdrPaperWhiteFloorNits = 80.0f;

// Sane bounds when mapping the OS "SDR content brightness" (SDR white level) onto paper-white nits,
// shared by the HDR metadata tuning (EditorApplication) and the HDR settings panel's detected value.
inline constexpr float kSdrWhiteToPaperWhiteMinNits = 80.0f;
inline constexpr float kSdrWhiteToPaperWhiteMaxNits = 480.0f;

/// Luminance, in nits, of framebuffer value 1.0 on an scRGB (extended-linear sRGB) swapchain.
/// The final encode scales scene-linear output by paperWhite / this value.
///
/// The anchor belongs to the platform compositor, not to the graphics backend. Windows scRGB
/// anchors 1.0 at 80 nits (kHdrPaperWhiteFloorNits), so the encode lifts SDR content by
/// paperWhite / 80. The macOS compositor anchors extended-linear sRGB 1.0 at the display's SDR
/// reference white and applies the lift itself, so there the anchor is the paper white and the
/// encode scale is 1. Metal and Vulkan (MoltenVK) present through the same CAMetalLayer
/// configuration (RGBA16Float, kCGColorSpaceExtendedLinearSRGB, wantsExtendedDynamicRangeContent),
/// so both backends take the same anchor.
inline float GetScRGBFramebufferWhiteNits([[maybe_unused]] const HdrOutputState& state)
{
#if defined(__APPLE__)
    return state.staticMetadata.paperWhiteNits;
#else
    return kHdrPaperWhiteFloorNits;
#endif
}

inline float GetHdrOutputPeakLuminanceNits(const HdrOutputState& state)
{
    const float paperWhite = std::clamp(state.staticMetadata.paperWhiteNits, 40.0f, 1000.0f);
    float peakNits = std::max(state.display.maxLuminance, state.staticMetadata.maxContentLightLevel);
    peakNits = std::max(peakNits, state.staticMetadata.maxMasteringLuminance);
    return std::max(peakNits, paperWhite);
}

inline float GetHdrOutputMaxLinearValue(const HdrOutputState& state)
{
    if (!IsHdrOutputModeActive(state.activeMode))
        return 1.0f;

    const float paperWhite = std::clamp(state.staticMetadata.paperWhiteNits, 40.0f, 1000.0f);
    const float peakNits = GetHdrOutputPeakLuminanceNits(state);
    if (peakNits <= paperWhite)
        return 1.0f;
    return std::clamp(peakNits / paperWhite, 1.0f, 20.0f);
}

inline bool IsKnownLGOledC7DisplayName(std::string_view displayName)
{
    std::string name(displayName);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find("lg oled") != std::string::npos ||
           name == "lg tv" ||
           name.find("lg electronics tv") != std::string::npos ||
           name.find("oled55c7") != std::string::npos ||
           name.find("oled65c7") != std::string::npos ||
           name.find("c7p") != std::string::npos ||
           name.find("c7v") != std::string::npos ||
           name.find("c7t") != std::string::npos;
}

inline HdrOutputMode ResolveHdrOutputMode(HdrOutputMode requested, const HdrDisplayInfo& display)
{
    if (requested == HdrOutputMode::Off || !display.hdrAvailable)
        return HdrOutputMode::Off;

    if (requested == HdrOutputMode::Auto)
    {
        if (display.supportsHDR10_PQ) return HdrOutputMode::HDR10_PQ;
        if (display.supportsHLG) return HdrOutputMode::HLG;
        if (display.supportsScRGB) return HdrOutputMode::ScRGB;
        return HdrOutputMode::Off;
    }

    if (requested == HdrOutputMode::HDR10Plus)
    {
        if (display.supportsHDR10Plus) return HdrOutputMode::HDR10Plus;
        if (display.supportsHDR10_PQ) return HdrOutputMode::HDR10_PQ;
        return HdrOutputMode::Off;
    }

    if (requested == HdrOutputMode::HDR10_PQ && display.supportsHDR10_PQ) return HdrOutputMode::HDR10_PQ;
    if (requested == HdrOutputMode::HLG && display.supportsHLG) return HdrOutputMode::HLG;
    if (requested == HdrOutputMode::ScRGB && display.supportsScRGB) return HdrOutputMode::ScRGB;
    return HdrOutputMode::Off;
}

// Device creation parameters
struct DeviceDesc
{
    GraphicsAPI preferredAPI = GraphicsAPI::Auto;
    bool enableDebugLayer = false;
    // Validate descriptor updates against the declared set layout (type, array
    // bounds, binding existence). Assert-enabled builds (Debug/DebugFast) always
    // validate regardless of this flag — unchecked misuse on the descriptor-buffer
    // path corrupts adjacent sets' descriptor bytes. Setting this false takes
    // effect in Release only, where it removes all tracking and per-update lookups.
    bool enableDescriptorValidation = true;
    // When true, the device will create and manage a presentation swapchain if supported by the active backend.
    bool enableSwapchain = true;
    std::string applicationName = "GameEngine Application";
    uint32_t applicationVersion = 1;
    // Policy & features
    uint32_t maxPushConstantBytes = 128;        // Policy limit for push constants (<= device limit)
    bool enableDynamicRendering = true;         // Enable dynamic rendering when supported by the device
    bool forceDisableBindlessResources = false; // When true, gate off bindless even if supported
    // When true, request the backend's FIFO present mode (hard vsync, capped to
    // display refresh rate). When false, prefer mailbox (low-latency, uncapped).
    bool vsync = false;
    // Per-device descriptor-buffer policy. Auto routes through DB on hardware
    // that supports descriptor buffers. Disabled forces the legacy pool path.
    DescriptorBufferMode descriptorBuffers = DescriptorBufferMode::Auto;
    bool hdrEnabled = false;
    HdrOutputMode hdrMode = HdrOutputMode::HDR10_PQ;
    HdrSwapchainBitDepth hdrSwapchainBitDepth = HdrSwapchainBitDepth::Bit10;
    int hdrTargetDisplay = -1;
    HdrStaticMetadata hdrStaticMetadata{};
};

// Buffer usage flags
enum class BufferUsage : uint32_t
{
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Uniform = 1 << 2,
    Storage = 1 << 3,
    Indirect = 1 << 4,
    TransferSrc = 1 << 5,
    TransferDst = 1 << 6,

    // Enables taking the buffer's device address; the backend adds its own
    // device-address usage bit. Required for shaders that consume the buffer via
    // GL_EXT_buffer_reference instead of an SSBO/UBO descriptor. Independent
    // from Storage — a buffer can carry one, the other, or both.
    ShaderDeviceAddress = 1 << 7,

    // The buffer's contents (vertex/index data) may be read as geometry input
    // by acceleration-structure builds. Builds consume the data via
    // device address, so pair with ShaderDeviceAddress. Request only when
    // RenderingDeviceCapabilities::supportsRayQuery is true — the underlying
    // extension is not enabled otherwise.
    AccelerationStructureBuildInput = 1 << 8,

    // The buffer backs an acceleration structure. Backend-internal — the AS
    // backend allocates BLAS/TLAS storage with it; same supportsRayQuery
    // contract as AccelerationStructureBuildInput.
    AccelerationStructureStorage = 1 << 9
};

// Bitwise operators for BufferUsage
inline BufferUsage operator|(BufferUsage a, BufferUsage b)
{
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline BufferUsage operator&(BufferUsage a, BufferUsage b)
{
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// Texture format enumeration
enum class TextureFormat : uint32_t
{
    Unknown = 0,
    // 8-bit normalized and SRGB
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    // Integer color formats (8/16/32-bit; scalar/vec)
    R8_UINT,
    R8_SINT,
    R8G8_UINT,
    R8G8_SINT,
    RGBA8_UINT,
    RGBA8_SINT,
    R16_UINT,
    R16_SINT,
    R16G16_UINT,
    R16G16_SINT,
    RGBA16_UINT,
    RGBA16_SINT,
    R32_UINT,
    R32_SINT,
    R32G32_UINT,
    R32G32_SINT,
    R32G32B32_UINT,
    R32G32B32_SINT,
    RGBA32_UINT,
    RGBA32_SINT,
    // Float color formats
    R32G32B32A32_FLOAT,
    R16G16B16A16_FLOAT,
    R16G16B16A16_UNORM,
    R11G11B10_FLOAT,
    RGB10A2_UNORM,
    R16_FLOAT,
    R16G16_FLOAT,
    R32_FLOAT,
    R8_UNORM,
    R8G8_UNORM,
    // Depth/stencil
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_SFLOAT_S8_UINT,
    // Block compressed
    BC1_UNORM,
    BC3_UNORM,
    BC5_UNORM,
    BC7_UNORM,
    // Additional depth/stencil variants
    D16_UNORM,
    S8_UINT,
    X8_D24_UNORM_PACK32,
    R32G32_FLOAT,
    // Block compressed, sRGB view (appended: TextureDesc.format carries raw enum values)
    BC7_SRGB,
    // Block compressed, appended for the texture import cook (BCn + mips):
    // BC1 sRGB view, single-channel BC4, and HDR BC6H (unsigned half).
    BC1_SRGB,
    BC4_UNORM,
    BC6H_UF16
};

// Texture usage flags
enum class TextureUsage : uint32_t
{
    None = 0,
    ShaderResource = 1 << 0,
    RenderTarget = 1 << 1,
    DepthStencil = 1 << 2,
    UnorderedAccess = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5
};

// Bitwise operators for TextureUsage
inline TextureUsage operator|(TextureUsage a, TextureUsage b)
{
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline TextureUsage operator&(TextureUsage a, TextureUsage b)
{
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// Format enumeration (backend-agnostic; used for vertex attributes and generic format selection)
enum class Format : uint32_t
{
    Unknown = 0,
    // Unsigned normalized
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    // Float formats
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    R32G32B32_FLOAT,
    R32G32_FLOAT,
    // Unsigned integer (vertex attributes)
    R16G16B16A16_UINT,
    // Depth/stencil
    D32_FLOAT,
    D24_UNORM_S8_UINT
};

// Memory usage (cross-API heap selection)
enum class BufferMemoryUsage : uint32_t
{
    Auto = 0,
    DeviceLocal,
    Upload,
    Readback,

    // Host-writable like Upload, but asking to live in device-local memory.
    //
    // The caller states a shape, not a heap: bytes the CPU writes once (or
    // rarely) and the GPU then re-reads for many frames, large enough that the
    // re-read bandwidth dominates the write. Mesh vertex/index pools are the
    // motivating class. Upload stays the right value for anything the CPU
    // rewrites every frame, anything whose bytes are copied out once, and
    // anything small enough for the difference not to matter.
    //
    // A preference, never a guarantee, and deliberately so: whether device-local
    // memory is host-writable *at scale* is a property of the device, and each
    // backend answers it from its own memory model. What the preference must
    // never cost is host-visibility — a buffer created with this value is
    // exactly as mappable as one created with Upload, so PersistentlyMapped
    // means the same thing here as there. That invariant is what lets a caller
    // choose this value without also writing a fallback path.
    //
    // A backend that cannot express the preference must behave exactly as it
    // does for Upload rather than approximate it: a silently non-mappable
    // buffer faults on first write, which is strictly worse than the slower
    // residency this value exists to avoid.
    UploadDeviceLocalPreferred
};

// Buffer creation flags (extensible)
enum class BufferCreateFlags : uint32_t
{
    None = 0,
    PersistentlyMapped = 1 << 0,
    Transient = 1 << 1,
    AllowAlias = 1 << 2,
    DedicatedAllocation = 1 << 3,
    ShareAcrossQueues = 1 << 4,
    // One slot of a per-frame ring whose owner selects it with GetFrameIndex().
    // Only BeginFrame waits the fence proving the frame that last used the slot
    // has finished reading it, so a host write is valid only inside an acquired
    // frame — between BeginFrame and the Present/FinalizeFrame that rotates on.
    // Developer builds diagnose a write outside that window; nothing else tells
    // the backend a ring slot apart from an ordinary upload buffer. The diagnosis
    // also covers MapBuffer, so a read-back ring must not carry this flag.
    FrameSlotted = 1 << 5
};

// Helper: true for formats with a depth aspect. Sampled views of these images
// are bound by the descriptor writer in the depth-read-only layout, never the
// color SHADER_READ_ONLY layout — transition producers must agree (route
// through ResourceState::DepthSampled). S8_UINT is stencil-only and excluded:
// nothing samples it and the descriptor writer's format list excludes it too.
inline bool IsDepthFormat(TextureFormat fmt)
{
    switch (fmt)
    {
    case TextureFormat::D32_FLOAT:
    case TextureFormat::D24_UNORM_S8_UINT:
    case TextureFormat::D32_SFLOAT_S8_UINT:
    case TextureFormat::D16_UNORM:
    case TextureFormat::X8_D24_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

// Helper: bytes per pixel for TextureFormat (for row pitch calculations)
inline uint32_t BytesPerPixel(TextureFormat fmt)
{
    switch (fmt)
    {
    // 8-bit UNORM/SRGB
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
        return 4;
    // 8-bit integer
    case TextureFormat::R8_UINT:
    case TextureFormat::R8_SINT:
        return 1;
    case TextureFormat::R8G8_UINT:
    case TextureFormat::R8G8_SINT:
        return 2;
    case TextureFormat::RGBA8_UINT:
    case TextureFormat::RGBA8_SINT:
        return 4;
    // 16-bit float/integer
    case TextureFormat::R16_FLOAT:
    case TextureFormat::R16_UINT:
    case TextureFormat::R16_SINT:
        return 2;
    case TextureFormat::R16G16_FLOAT:
    case TextureFormat::R16G16_UINT:
    case TextureFormat::R16G16_SINT:
        return 4;
    case TextureFormat::R16G16B16A16_FLOAT:
    case TextureFormat::R16G16B16A16_UNORM:
    case TextureFormat::RGBA16_UINT:
    case TextureFormat::RGBA16_SINT:
        return 8;
    // Packed
    case TextureFormat::R11G11B10_FLOAT:
    case TextureFormat::RGB10A2_UNORM:
        return 4;
    // 32-bit float/integer
    case TextureFormat::R32_FLOAT:
    case TextureFormat::R32_UINT:
    case TextureFormat::R32_SINT:
        return 4;
    case TextureFormat::R32G32_UINT:
    case TextureFormat::R32G32_SINT:
    case TextureFormat::R32G32_FLOAT:
        return 8;
    case TextureFormat::R32G32B32_UINT:
    case TextureFormat::R32G32B32_SINT:
        return 12;
    case TextureFormat::R32G32B32A32_FLOAT:
    case TextureFormat::RGBA32_UINT:
    case TextureFormat::RGBA32_SINT:
        return 16;
    // Lower-bit float/unorm
    case TextureFormat::R8_UNORM:
        return 1;
    case TextureFormat::R8G8_UNORM:
        return 2;
    // Depth/stencil
    case TextureFormat::D32_FLOAT:
        return 4;
    case TextureFormat::D24_UNORM_S8_UINT:
        return 4;
    case TextureFormat::D32_SFLOAT_S8_UINT:
        return 8; // 64-bit depth-stencil (32-bit float depth + 8-bit stencil + padding)
    case TextureFormat::D16_UNORM:
        return 2;
    case TextureFormat::S8_UINT:
        return 1;
    case TextureFormat::X8_D24_UNORM_PACK32:
        return 4;
    // Block compressed
    case TextureFormat::BC1_UNORM:
    case TextureFormat::BC1_SRGB:
    case TextureFormat::BC3_UNORM:
    case TextureFormat::BC4_UNORM:
    case TextureFormat::BC5_UNORM:
    case TextureFormat::BC6H_UF16:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::BC7_SRGB:
        return 0; // block compressed; use block rules (BytesPerBlock below)
    default:
        return 0;
    }
}

// Block-compression rules. All supported BC formats use 4x4 texel blocks.
inline constexpr uint32_t kBlockCompressedBlockDim = 4;

inline bool IsBlockCompressedFormat(TextureFormat fmt)
{
    switch (fmt)
    {
    case TextureFormat::BC1_UNORM:
    case TextureFormat::BC1_SRGB:
    case TextureFormat::BC3_UNORM:
    case TextureFormat::BC4_UNORM:
    case TextureFormat::BC5_UNORM:
    case TextureFormat::BC6H_UF16:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::BC7_SRGB:
        return true;
    default:
        return false;
    }
}

// Bytes per 4x4 block; 0 for non-block formats.
inline uint32_t BytesPerBlock(TextureFormat fmt)
{
    switch (fmt)
    {
    case TextureFormat::BC1_UNORM:
    case TextureFormat::BC1_SRGB:
    case TextureFormat::BC4_UNORM:
        return 8;
    case TextureFormat::BC3_UNORM:
    case TextureFormat::BC5_UNORM:
    case TextureFormat::BC6H_UF16:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::BC7_SRGB:
        return 16;
    default:
        return 0;
    }
}

// The rows of a tightly packed buffer copy of a `width` x `height` texel region,
// which is what a zero row pitch in a buffer-texture copy means. An uncompressed
// row is one row of texels. A block-compressed row is one row of 4x4 blocks:
// ceil(width / 4) blocks long, covering four texel rows, so a slice holds
// ceil(height / 4) of them.
struct TightCopyRows
{
    size_t RowBytes;
    uint32_t RowCount;
};

inline TightCopyRows ComputeTightCopyRows(TextureFormat fmt, uint32_t width, uint32_t height)
{
    if (!IsBlockCompressedFormat(fmt))
    {
        return {static_cast<size_t>(width) * BytesPerPixel(fmt), height};
    }
    const uint32_t blocksWide = (width + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim;
    const uint32_t blocksHigh = (height + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim;
    return {static_cast<size_t>(blocksWide) * BytesPerBlock(fmt), blocksHigh};
}

// How a device's memory is physically arranged, as far as its backend can see.
//
// Reported as a unit deliberately: a backend either walks its memory heaps or
// it does not, and it cannot honestly know the CPU-writable VRAM window without
// also knowing the device-local total. Grouping the three behind one
// std::optional (RenderingDeviceCapabilities::memoryTopology) is what keeps
// "this backend does not answer" distinguishable from "this device has no such
// heap" — a residency gate that read a silent zero as fact would classify an
// unimplemented backend as having no VRAM at all.
struct DeviceMemoryTopology
{
    // True when device memory is physically shared with the host, so a staged
    // copy into "device-local" memory moves bytes that were already as close to
    // the GPU as they can get. Broader than integrated-GPU: Vulkan's CPU and
    // VIRTUAL_GPU device types are unified too.
    bool isUnifiedMemory = false;

    // Size of the LARGEST single heap that is both device-local and
    // host-visible: the widest window one CPU write can land in without a
    // transfer. Roughly 256 MB is a classic BAR; a value comparable to
    // deviceLocalHeapBytesTotal means resizable BAR / Smart Access Memory.
    //
    // A max and not a sum, deliberately: one allocation cannot span two heaps,
    // so adding separate windows would describe a capability no caller has.
    //
    // A large window means such a write CAN land in VRAM, never that any given
    // allocation DID — that is decided by the residency policy the allocation
    // was created under, and can only be established by observing the
    // allocation itself (IDevice::GetBufferMemoryResidency).
    uint64_t largestHostVisibleDeviceLocalHeapBytes = 0;

    // Total bytes across every device-local heap — the device's VRAM, however
    // many heaps the driver splits it into.
    //
    // A sum and not a max, deliberately: drivers disagree on whether the
    // CPU-visible aperture is carved OUT of the VRAM total (declared as two
    // heaps that partition it) or declared as an extra heap OVERLAPPING it.
    // Against a max, "the window covers the card" would silently mean "the
    // window is at least half the card" on the first kind.
    uint64_t deviceLocalHeapBytesTotal = 0;
};

// GPU hardware vendor, classified from the raw vendor ID the backend reports.
//
// Unknown covers both "this backend reports no vendor" and a vendor outside this
// list, so no caller may read Unknown as a particular one. The raw id is kept
// beside it in RenderingDeviceCapabilities::vendorId, which is what leaves an
// unclassified device identifiable.
enum class GpuVendor : uint32_t
{
    Unknown,
    Nvidia,
    Amd,
    Intel
};

// Display spelling for a vendor, as logs and the debug server report it.
constexpr const char* GpuVendorName(GpuVendor vendor) noexcept
{
    switch (vendor)
    {
    case GpuVendor::Nvidia:  return "NVIDIA";
    case GpuVendor::Amd:     return "AMD";
    case GpuVendor::Intel:   return "Intel";
    case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

// Classify a raw PCI vendor ID. Anything unrecognized stays Unknown rather than
// being forced into the nearest enumerator — a vendor gate that guessed would
// enable a vendor-specific path on hardware that cannot run it.
constexpr GpuVendor ClassifyGpuVendor(uint32_t vendorId) noexcept
{
    // PCI-SIG vendor IDs, identical across graphics APIs.
    constexpr uint32_t kNvidiaVendorId = 0x10DEu;
    constexpr uint32_t kAmdVendorId    = 0x1002u;
    constexpr uint32_t kIntelVendorId  = 0x8086u;
    switch (vendorId)
    {
    case kNvidiaVendorId: return GpuVendor::Nvidia;
    case kAmdVendorId:    return GpuVendor::Amd;
    case kIntelVendorId:  return GpuVendor::Intel;
    default:              return GpuVendor::Unknown;
    }
}

// Device capabilities
struct RenderingDeviceCapabilities
{
    bool supportsMeshShaders = false;
    bool supportsRayTracing = false;
    // True when the backend's acceleration-structure and ray-query extensions
    // (plus the deferred-host-operations support they need) are ENABLED on the
    // logical device with their core feature bits — not mere physical-device detection like
    // supportsRayTracing above. Requires bufferDeviceAddress (AS builds consume
    // device addresses). Gates the RT shadow-mask lane (DirectionalShadowMode::RayTraced);
    // when false the flag is silently inert.
    bool supportsRayQuery = false;
    bool supportsWorkGraphs = false;
    bool supportsVariableRateShading = false;
    bool supportsBindlessResources = false;
    // True when the backend advertises a descriptor-buffer extension (or a D3D12 equivalent)
    // and the device reports the feature bit. Used to gate the descriptor-buffer code
    // paths added during the descriptor-buffer migration.
    bool supportsDescriptorBuffer = false;
    // True when the device supports shader-side buffer device addresses
    // (Vulkan 1.2 bufferDeviceAddress feature). Gates VMA's
    // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT and any caller wanting
    // to take BufferHandle addresses via IDevice::GetBufferDeviceAddress.
    bool supportsBufferDeviceAddress = false;
    // True when DrawIndexedIndirectCount honors the GPU-side count buffer
    // (Vulkan 1.2 drawIndirectCount; Metal through an indirect command buffer
    // run over a GPU-written execution range). False when the backend emulates
    // it by iterating all maxDrawCount records (Vulkan without drawIndirectCount,
    // WebGPU; D3D12 stub) — under emulation, producers must zero-fill unused
    // trailing records so stale commands aren't replayed as ghost draws. Metal
    // without MTLResidencySet (macOS before 15) also reports false, but it
    // refuses the call and draws nothing rather than emulating it.
    bool supportsDrawIndirectCountNative = false;
    // True when the device supports 64-bit integers in shaders (Vulkan
    // shaderInt64). The engine enables it whenever supported — buffer_reference /
    // instanced shaders declare uint64_t scalars, and CBT terrain stores its
    // per-bisector HeapID as a plain u64. This is NON-ATOMIC u64 support only;
    // int64 *atomics* (shaderBufferInt64Atomics) are deliberately not required
    // (CBT uses a u32-word bitfield so it stays portable to Metal/MoltenVK).
    bool supportsShaderInt64 = false;
    // True when the device supports BC (DXT/BPTC) block-compressed sampled
    // textures (Vulkan textureCompressionBC, enabled at device creation when
    // available). Gates BC1/3/5/7 texture creation; loaders fall back to
    // uncompressed RGBA when false (MoltenVK on some Apple GPUs).
    bool supportsTextureCompressionBC = false;
    // True when dual-source blending is enabled on the logical device (Vulkan
    // dualSrcBlend + maxFragmentDualSrcAttachments >= 1; same support==enabled
    // contract as shaderInt64). Gates pipelines whose blend factors reference
    // the second fragment output (BlendFactor::Src1* / OneMinusSrc1*), e.g.
    // subpixel RGB text AA. Callers must fall back to single-source blending
    // when false.
    bool supportsDualSourceBlending = false;
    // True when sampleRateShading is enabled on the logical device (same
    // support == enabled contract as shaderInt64). It is what makes the SPIR-V
    // InterpolationFunction capability (interpolateAtOffset) legal; enabling it
    // turns no pipeline to per-sample shading. RendererProfile maps it to
    // UseInterpolationFunctions, which the shader defines follow.
    bool supportsSampleRateShading = false;

    uint32_t maxBindlessTextures = 0;
    uint32_t maxBindlessBuffers = 0;
    uint32_t maxPerStageSamplers = 0;
    uint32_t maxPerStageSampledImages = 0;
    uint32_t maxPerStageResources = 0;
    // Storage buffers one shader stage may bind at once (Vulkan
    // maxPerStageDescriptorStorageBuffers, WebGPU maxStorageBuffersPerShaderStage).
    // 0 = the backend did not report it; callers treat 0 as unlimited.
    uint32_t maxPerStageStorageBuffers = 0;
    uint32_t maxComputeWorkGroupSize = 0;
    // Max shared/threadgroup memory per workgroup in bytes (Vulkan
    // maxComputeSharedMemorySize). 0 = the backend did not report it; callers
    // gating LDS-heavy dispatches must treat 0 as unsupported (fail safe).
    uint32_t maxComputeSharedMemorySize = 0;
    // Largest MSAA sample count supported for color+depth render targets.
    // Engine MSAA settings clamp to this (Apple GPUs cap at 4; requesting 8
    // there fails texture creation outright).
    uint32_t maxMSAASamples = 8;
    // Largest SamplerDesc::maxAnisotropy a sampler may request on this device
    // (Vulkan maxSamplerAnisotropy, when the samplerAnisotropy feature is
    // enabled). 1 = anisotropic filtering unavailable, or not reported by the
    // backend; every sampler then filters isotropically.
    float maxSamplerAnisotropy = 1.0f;
    // GPU vendor, classified from vendorId below. The two driver-policy hints
    // beneath are derived from it together with the device name.
    GpuVendor vendor = GpuVendor::Unknown;
    // Raw PCI vendor ID exactly as the backend reports it; 0 when it reports
    // none. Kept beside the classification so a device the enum calls Unknown is
    // still identifiable from a log or a bug report.
    uint32_t vendorId = 0;

    // Device/driver policy hint for the engine's capability-derived AA default
    // (ResolveDefaultAntiAliasing, Engine/Rendering/AntiAliasing.h). Legacy
    // Radeon Vulkan drivers can have fragile MSAA paths; callers should default
    // to 1x unless the user explicitly requests MSAA.
    bool prefersNoDefaultMSAA = false;
    // Device/driver policy hint for PCSS shadow filtering. Some legacy Radeon
    // Vulkan drivers handle the engine's bindless raw-depth PCSS reads
    // unreliably, while the comparison-sampler PCF path remains stable.
    bool prefersStableShadowFiltering = false;
    // Whether one buffer may be both a shader storage target and CPU-mappable.
    // Vulkan and Metal allow it (a storage buffer in host-visible memory);
    // WebGPU forbids it outright — MapRead composes with CopyDst and nothing
    // else — so a readback slot written directly by a dispatch has no legal
    // translation there. Features that rely on it must offer a fallback.
    bool supportsMappableStorageBuffers = true;
    // Whether a two-channel 16-bit float texture may be a storage image.
    // Vulkan and Metal allow it. WebGPU's storage-capable format list is fixed
    // and omits rg16float while including rgba16float, so a compute bake that
    // writes one is rejected at CreateTexture — the texture never exists, and
    // every sampler downstream reads nothing rather than reading something
    // wrong. Callers that bake into rg16float widen the format instead.
    bool supportsRG16FloatStorage = true;
    // Whether storage images may use narrow (sub-RGBA, non-32-bit) formats such
    // as r8unorm, rg8unorm or r16float. Vulkan and Metal allow them. WebGPU's
    // storage-capable list is RGBA-or-32-bit only, so a sim that bakes into an
    // r8unorm cascade has no legal translation and its textures are refused at
    // creation. Features built on them decline instead of emitting a frame of
    // invalid resources, which would invalidate the whole command buffer.
    bool supportsNarrowStorageFormats = true;
    // Whether multiple writable storage bindings may refer to the same
    // texture when the shader leaves the duplicate bindings unused. WebGPU
    // rejects this aliasing at dispatch even when control flow skips writes.
    bool supportsAliasedStorageTextureBindings = true;
    // Whether 32-bit float color formats sample as filterable floats (linear
    // filtering). Native backends on desktop GPUs support it; WebGPU gates it
    // behind the float32-filterable feature. When absent, consumers that
    // filter float textures (colour LUTs) must upload float16 instead.
    bool supportsFilterableFloat32 = true;
    // Whether GPU objects may be created and used from any thread. Native
    // backends say yes. emdawnwebgpu keeps its handle table in per-thread
    // JavaScript state, so a device made on one thread simply does not exist on
    // another: the first call from a worker fails an assertion and kills that
    // worker. Callers that would offload resource creation to a job must run it
    // on the thread that owns the device instead.
    bool supportsMultithreadedResourceCreation = true;

    size_t dedicatedVideoMemory = 0;
    size_t sharedSystemMemory = 0;
    // Minimum alignment in bytes required for storage buffer offsets used in descriptor bindings.
    // For Vulkan this is populated from the device limits field that reports
    // storage-buffer offset alignment requirements.
    // Backends that do not expose a specific requirement leave this as 0, which callers treat as
    // "no additional alignment beyond element size".
    size_t minStorageBufferOffsetAlignment = 0;
    // Largest byte range a single storage-buffer descriptor may cover (Vulkan
    // maxStorageBufferRange, WebGPU maxStorageBufferBindingSize). Growable
    // pools clamp their ceilings to this. 0 = the backend did not report it,
    // which callers treat as "no ceiling beyond their own budget".
    size_t maxStorageBufferBindingSize = 0;
    // True when 16-bit UNORM color texture formats (R16G16B16A16_UNORM) are
    // creatable. Core WebGPU has none (texture-formats-tier1 feature);
    // consumers fall back to the FLOAT16 variants.
    bool supportsUnorm16TextureFormats = true;
    // Required row-pitch alignment in bytes for multi-row buffer<->texture
    // copies (WebGPU COPY_BYTES_PER_ROW_ALIGNMENT = 256). Uploaders stage rows
    // padded to this and pass the padded pitch. 1 = no requirement beyond
    // texel size. Single-row copies are exempt on every backend.
    uint32_t textureCopyRowPitchAlignment = 1;

    // Engaged only by backends that actually walk their memory heaps.
    // Disengaged means "this backend does not report memory topology" — never
    // "the device has no device-local memory". Consumers must handle the
    // disengaged case explicitly; there is no safe default to read through.
    std::optional<DeviceMemoryTopology> memoryTopology;

    // Resolve mode capabilities (bitmask of supported depth/stencil resolve modes in engine-defined bit positions).
    // Backends map these engine-level bits to their native resolve flags.
    uint32_t supportedDepthResolveModes = 0;
    uint32_t supportedStencilResolveModes = 0;
    // True when the device can resolve depth and stencil independently according to its native API.
    bool supportsIndependentStencilResolve = false;

    // Preferred default depth (+optional stencil) attachment format for this device.
    // Backends are responsible for populating this based on native format support.
    // Reverse-Z requires float depth; D32_SFLOAT_S8_UINT is the only suitable
    // combined format. Backends that can't support it should set D32_FLOAT
    // (depth-only) explicitly.
    TextureFormat preferredDepthAndStencilFormat = TextureFormat::D32_SFLOAT_S8_UINT;
};

inline BufferCreateFlags operator|(BufferCreateFlags a, BufferCreateFlags b)
{
    return static_cast<BufferCreateFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline BufferCreateFlags operator&(BufferCreateFlags a, BufferCreateFlags b)
{
    return static_cast<BufferCreateFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

enum class TextureCreateFlags : uint32_t;

inline TextureCreateFlags operator|(TextureCreateFlags a, TextureCreateFlags b)
{
    return static_cast<TextureCreateFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline TextureCreateFlags operator&(TextureCreateFlags a, TextureCreateFlags b)
{
    return static_cast<TextureCreateFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// Resource state enumeration (no Present; final PRESENT layout is handled at device Present())
enum class ResourceState : uint32_t
{
    Undefined,
    Common,
    VertexBuffer,
    IndexBuffer,
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    DepthRead,
    DepthSampled, // Read-only depth/stencil with shader sampling synchronization
    CopySource,
    CopyDest,
    IndirectArgs // Buffer read as an indirect draw/dispatch argument source (DRAW_INDIRECT stage)
};

// Legacy compatibility: keep MemoryType for older modules (VulkanResources/D3D12Resources)
enum class MemoryType : uint32_t
{
    DeviceLocal,
    HostVisible,
    HostCached
};

// Buffer description
struct BufferDesc
{
    size_t size = 0;
    uint32_t usage = 0; // Usage flags (platform-specific)
    BufferMemoryUsage memoryUsage = BufferMemoryUsage::Auto;
    BufferCreateFlags flags = BufferCreateFlags::None;
    bool persistent = false; // Survives across frames
    size_t stride = 0;       // For structured buffers
    const char* debugName = nullptr;
};

// Texture creation flags (extensible)
enum class TextureCreateFlags : uint32_t
{
    None = 0,
    CubeCompatible = 1 << 0,
    ForceArrayView = 1 << 1,
};

// Texture description
struct TextureDesc
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t format = 0; // Format enum (platform-specific)
    uint32_t usage = 0;  // Usage flags (platform-specific)
    uint32_t sampleCount = 1;
    TextureCreateFlags flags = TextureCreateFlags::None;
    bool persistent = false; // Survives across frames
    ResourceState initialState = ResourceState::Undefined; // If != Undefined, backend transitions after creation
    // GENERAL-resident sampled texture: a storage image (UnorderedAccess) that is
    // ALSO sampled, kept in the backend's general layout for its whole lifetime so the
    // storage and sampled consumers never fight over the layout across queues
    // (View.DepthResolved: storage-read by the HZB compute pass, sampled by AO /
    // world / fog / etc). Sampled + combined-image-sampler descriptors of this
    // texture claim GENERAL (sampling from GENERAL is legal) instead of
    // SHADER_READ_ONLY, matching the render graph's SampledInGeneralLayout constraint — otherwise
    // the layout ping-pongs and trips VUID-vkCmdDraw-None-09600 with async compute.
    bool sampledInGeneralLayout = false;
    const char* debugName = nullptr;
};

// Whether views of this image must be ARRAY views. This is the rule the backend
// applies when it builds an image's default view, so a hand-built view of the
// same image (a per-mip storage view, say) agrees with the default rather than
// presenting a different shape for the same texture. Two ways in: more than one
// layer, or a single-layer image the author declared array-shaped with
// ForceArrayView so shaders can bind it as sampler2DArray / image2DArray.
inline bool NeedsArrayView(const TextureDesc& desc)
{
    return desc.arrayLayers > 1u ||
           (desc.flags & TextureCreateFlags::ForceArrayView) != TextureCreateFlags::None;
}

// Texture view swizzle and aspect enums
enum class TextureAspect : uint32_t
{
    None = 0,
    Color = 1 << 0,
    Depth = 1 << 1,
    Stencil = 1 << 2,
    DepthStencil = Depth | Stencil
};
inline TextureAspect operator|(TextureAspect a, TextureAspect b)
{
    return static_cast<TextureAspect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
enum class TextureSwizzle : uint32_t
{
    Identity = 0,
    Zero,
    One,
    R,
    G,
    B,
    A
};

enum class TextureViewType : uint32_t
{
    View2D = 0,
    View2DArray = 1,
    ViewCube = 2,
    ViewCubeArray = 3,
    View3D = 4,
};

// Texture view description (format/aspect/subrange/swizzle)
struct TextureViewDesc
{
    uint32_t formatOverride = 0; // 0 means same as image
    TextureAspect aspect = TextureAspect::Color;
    TextureViewType viewType = TextureViewType::View2D;
    uint32_t baseMip = 0;
    uint32_t levelCount = 0; // 0 means "all remaining"
    uint32_t baseLayer = 0;
    uint32_t layerCount = 0; // 0 means "all remaining"
    TextureSwizzle r = TextureSwizzle::Identity;
    TextureSwizzle g = TextureSwizzle::Identity;
    TextureSwizzle b = TextureSwizzle::Identity;
    TextureSwizzle a = TextureSwizzle::Identity;
    const char* debugName = nullptr;
};

// Forward declaration for sampler compare op
enum class CompareOp;

// Sampler description
struct SamplerDesc
{
    uint32_t minFilter = 0;
    uint32_t magFilter = 0;
    uint32_t mipFilter = 0;
    uint32_t addressModeU = 0;
    uint32_t addressModeV = 0;
    uint32_t addressModeW = 0;
    float mipLodBias = 0.0f;
    float maxAnisotropy = 1.0f;
    float minLod = 0.0f;
    float maxLod = 1000.0f;
    // New: comparison sampling support and border color selection
    bool compareEnable = false;
    CompareOp compareOp; // set only when compareEnable=true
    // Border color: 0 = OpaqueBlack, 1 = OpaqueWhite
    uint32_t borderColor = 0;
    const char* debugName = nullptr;

    // Static preset helpers to avoid bloating Device with ad-hoc methods. Every
    // helper filters isotropically; the anisotropic material sampler comes from
    // ResolveSamplerPreset(SamplerPreset::LinearRepeat, profile).
    static SamplerDesc ShadowClampNearest(const char* name = "ShadowClampNearest");
    static SamplerDesc ShadowComparePCF(const char* name = "ShadowComparePCF");
    static SamplerDesc MaterialLinearRepeat(const char* name = "MaterialLinearRepeat");
    static SamplerDesc MaterialLinearClamp(const char* name = "MaterialLinearClamp");
    static SamplerDesc MaterialBilinearRepeat(const char* name = "MaterialBilinearRepeat");
    static SamplerDesc PointClamp(const char* name = "PointClamp");
    static SamplerDesc PointRepeat(const char* name = "PointRepeat");
};

// Standard sampler presets for use with TextureService::GetSampler().
enum class SamplerPreset
{
    LinearRepeat,    // Default material sampling (trilinear, repeat, anisotropic)
    LinearClamp,     // Heightmaps, environment probes, LUTs
    BilinearRepeat,  // Linear min/mag with nearest mip (no trilinear interpolation)
    PointClamp,      // UI textures, pixel-perfect sampling
    PointRepeat,     // Pixel art, nearest-neighbor tiling
    // Colour sampled clamp-to-edge with the material sampler's filtering: an image that covers its
    // surface once (a terrain's Planar basemap or orthophoto). LinearClamp stays isotropic, because
    // heightmaps, atlases and LUTs read it as data.
    LinearClampAnisotropic,
    kCount           // Must be last — used to size the sampler cache array
};

struct RendererProfile;

/// The sampler description a standard preset resolves to under a renderer
/// profile: the single preset-to-descriptor mapping. TextureService creates its
/// cached preset samplers from it, so the bindless sampler array (full profile)
/// and the per-material bind groups (compatibility profile) sample through the
/// same descriptions.
///
/// LinearRepeat, the material trilinear sampler, and LinearClampAnisotropic, its
/// clamp-to-edge twin, request profile.MaterialSamplerAnisotropy (already clamped
/// to the device limit); every other preset filters isotropically, which also keeps the non-trilinear
/// presets legal on WebGPU, where anisotropy requires linear min, mag and mip
/// filtering.
SamplerDesc ResolveSamplerPreset(SamplerPreset preset, const RendererProfile& profile);

// Descriptor type enumeration.
//
// Append only: MetalShaderTranslator's on-disk MSL cache hashes the raw
// numeric value of every binding's type, so renumbering silently invalidates
// nothing and reuses stale translations.
enum class DescriptorType
{
    UniformBuffer,
    StorageBuffer,
    Texture,
    Sampler,
    CombinedImageSampler,
    StorageImage,
    // Ray-query scene TLAS (GLSL accelerationStructureEXT / MSL
    // raytracing::acceleration_structure). Only meaningful on a device whose
    // RenderingDeviceCapabilities::supportsRayQuery is set — a layout
    // containing one cannot be created otherwise.
    AccelerationStructure
};

// Backend-agnostic shader stage mask constants (matches Vulkan bit positions)
static constexpr uint32_t kShaderStageVertex = 0x00000001u;
static constexpr uint32_t kShaderStageFragment = 0x00000010u;
static constexpr uint32_t kShaderStageCompute = 0x00000020u;

// Descriptor binding flags (map onto the backend's binding-flag bits)
static constexpr uint32_t kDescriptorBindingUpdateAfterBind = 0x00000001u;
static constexpr uint32_t kDescriptorBindingPartiallyBound  = 0x00000004u;
// Engine-internal (not a backend binding-flag value): the shader
// declares this storage buffer read-only (SPIR-V NonWritable). WebGPU layouts
// must state ReadOnlyStorage to match; Vulkan ignores it.
static constexpr uint32_t kDescriptorBindingReadOnlyStorage = 0x80000000u;

// Descriptor binding description
struct DescriptorBinding
{
    uint32_t binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    uint32_t count = 1;
    uint32_t shaderStages = 0; // Shader stage flags
    const char* debugName = nullptr;
    uint32_t flags = 0;        // Descriptor binding flags (kDescriptorBinding* constants)
    // Image-binding shape from shader reflection (WebGPU layouts declare view
    // dimension and storage texel format explicitly; Vulkan/Metal/D3D12
    // ignore these). imageDim: SPIR-V convention (1/2/3 = 1D/2D/3D, 4 = cube).
    uint32_t imageDim = 2;
    bool imageArrayed = false;
    // Shadow/comparison sampling: a depth texture pairs with a comparison
    // sampler, and a WebGPU layout that declares a filterable float texture
    // instead rejects the bind outright.
    bool imageIsDepth = false;
    // Integer-sampled image (GLSL utexture2D / WGSL texture_2d<u32>), read with
    // texelFetch. A WebGPU layout that leaves this a filterable float rejects
    // the bind outright ("None of the supported sample types (Uint) of
    // [Texture]"); Vulkan/Metal/D3D12 carry the type on the view and ignore it.
    bool imageIsUnsignedInteger = false;
    // Engine TextureFormat for StorageImage bindings; 0 (Unknown) when the
    // shader declared no format (or reflection was unavailable).
    uint32_t storageTexelFormat = 0;
    // StorageImage access as the shader declares it. WebGPU bakes access into
    // the bind group layout and rejects a WriteOnly layout against a shader
    // whose image is readonly; Vulkan/Metal carry access on the image and
    // ignore this.
    bool storageReadOnly = false;
    // Sampled-in-compute with a real (filtering) sampler over a filterable
    // format. The compute default below is UnfilterableFloat because most
    // compute taps are texelFetch over depth/R32F; a pass that genuinely
    // filters (the sky LUT chain over rgba16f) says so here or WebGPU rejects
    // the pipeline for pairing an unfilterable layout with its sampler.
    bool imageFilterableFloat = false;
    // The image fields above come from reflecting the shader's own usage, so an
    // unset imageFilterableFloat means no stage filters this binding. False on a
    // hand-built layout, which says nothing about usage.
    bool imageUsageReflected = false;
    // Multisampled sampled image (GLSL sampler2DMS / WGSL texture_multisampled_2d), read with
    // texelFetch and never with a sampler. WebGPU bakes this into the bind group layout: a layout
    // that leaves it false rejects both the MSAA texture at bind time ("Sample count (4) ...
    // doesn't match expectation (multisampled: 0)") and the shader module itself, whose entry
    // point declares the multisampled type. Vulkan/Metal/D3D12 carry it on the view and ignore it.
    bool imageMultisample = false;
};

// Descriptor set layout description
struct DescriptorSetLayoutDesc
{
    std::vector<DescriptorBinding> bindings;
    const char* debugName = nullptr;
};

// Pool-level descriptor set creation flags (extensible).
enum class DescriptorPoolFlags : uint32_t
{
    None = 0,
    UpdateAfterBind = 1 << 0, // Pool supports UPDATE_AFTER_BIND sets
};
inline DescriptorPoolFlags operator|(DescriptorPoolFlags a, DescriptorPoolFlags b)
{
    return static_cast<DescriptorPoolFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline DescriptorPoolFlags operator&(DescriptorPoolFlags a, DescriptorPoolFlags b)
{
    return static_cast<DescriptorPoolFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// Descriptor set description
struct DescriptorSetDesc
{
    DescriptorSetLayoutDesc layout;
    const char* debugName = nullptr;
    bool transient = false; // If true, allocates from per-frame transient pool and freed on BeginFrame
    DescriptorPoolFlags poolFlags = DescriptorPoolFlags::None;
};

// Descriptor set update
struct DescriptorSetUpdate
{
    uint32_t binding = 0;
    uint32_t arrayElement = 0;
    DescriptorType type = DescriptorType::UniformBuffer;

    // Resource handles to bind
    std::vector<BufferHandle> buffers;
    std::vector<TextureHandle> textures;         // Uses texture's default view
    std::vector<TextureViewHandle> textureViews; // If provided, overrides 'textures'
    std::vector<SamplerHandle> samplers;

    // Buffer ranges (for uniform/storage buffers)
    std::vector<size_t> bufferOffsets;
    std::vector<size_t> bufferRanges;

    // DescriptorType::AccelerationStructure. Slots are resolved through the
    // device's own acceleration-structure backend at write time, so the TLAS
    // object a slot currently owns never has to be cached by the caller (a
    // capacity grow recreates it).
    std::vector<TlasSlotHandle> accelerationStructures;
};

// Pipeline type enumeration
enum class PipelineType
{
    Graphics,
    Compute,
    Mesh,
    RayTracing
};

enum class PrimitiveTopology
{
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    PatchList,
    LineListWithAdjacency,
    LineStripWithAdjacency,
    TriangleListWithAdjacency,
    TriangleStripWithAdjacency
};

// Enhanced pipeline state enums based on reference implementation
enum class PolygonMode
{
    Fill = 0,
    Line = 1,
    Point = 2
};

enum class FrontFace
{
    CounterClockwise = 0,
    Clockwise = 1
};

// The opposite winding. Used by the per-instance mirrored-winding path to draw
// negative-determinant instances with a flipped front face relative to the
// view's base winding (never hardcode CW/CCW — planar-reflection views already
// carry a non-default base via their negative-height viewport).
inline constexpr FrontFace FlipWinding(FrontFace ff)
{
    return ff == FrontFace::Clockwise ? FrontFace::CounterClockwise : FrontFace::Clockwise;
}

namespace CullModeFlagBits
{
enum CullModeFlagBits
{
    None = 0,
    Front = 0x00000001,
    Back = 0x00000002,
    FrontAndBack = 0x00000003
};
}
using CullModeFlags = uint32_t;

enum class CompareOp
{
    Never = 0,
    Less = 1,
    Equal = 2,
    LessOrEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterOrEqual = 6,
    Always = 7
};

enum class BlendFactor
{
    Zero = 0,
    One = 1,
    SrcColor = 2,
    OneMinusSrcColor = 3,
    DstColor = 4,
    OneMinusDstColor = 5,
    SrcAlpha = 6,
    OneMinusSrcAlpha = 7,
    DstAlpha = 8,
    OneMinusDstAlpha = 9,
    ConstantColor = 10,
    OneMinusConstantColor = 11,
    ConstantAlpha = 12,
    OneMinusConstantAlpha = 13,
    AlphaSaturate = 14,
    // Dual-source blending factors (fragment output at location 0, index 1).
    // Pipelines using these require RenderingDeviceCapabilities::
    // supportsDualSourceBlending — callers gate, backends translate.
    Src1Color = 15,
    OneMinusSrc1Color = 16,
    Src1Alpha = 17,
    OneMinusSrc1Alpha = 18
};

enum class BlendOp
{
    Add = 0,

    Subtract = 1,
    ReverseSubtract = 2,
    Min = 3,
    Max = 4
};

enum class DynamicState
{
    Viewport = 0,
    Scissor = 1,
    LineWidth = 2,
    DepthBias = 3,
    BlendConstants = 4,
    DepthBounds = 5,
    StencilCompareMask = 6,
    StencilWriteMask = 7,
    StencilReference = 8
};

struct VertexInputBinding
{
    uint32_t binding;
    uint32_t stride;
    uint32_t inputRate; // 0: vertex, 1: instance
};

struct VertexInputAttribute
{
    uint32_t location;
    uint32_t binding;
    Format format; // backend-agnostic format (mapped in backends)
    uint32_t offset;
};

// Enhanced pipeline state structures based on reference implementation
struct RasterizationState
{
    bool depthClampEnable = false;
    bool rasterizerDiscardEnable = false;
    PolygonMode polygonMode = PolygonMode::Fill;
    CullModeFlags cullMode = CullModeFlagBits::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    bool depthBiasEnable = false;
    float depthBiasConstantFactor = 0.0f;
    float depthBiasClamp = 0.0f;
    float depthBiasSlopeFactor = 0.0f;
    float lineWidth = 1.0f;
};

struct DepthStencilState
{
    bool depthTestEnable = false;
    bool depthWriteEnable = false;
    // Reverse-Z: GreaterOrEqual so enabling a depth prepass doesn't accidentally
    // cause base passes (that didn't explicitly set compare op) to reject all
    // fragments due to equal-depth (prepass wrote the same depth).
    CompareOp depthCompareOp = CompareOp::GreaterOrEqual;
    bool depthBoundsTestEnable = false;
    bool stencilTestEnable = false;
    float minDepthBounds = 0.0f;

    float maxDepthBounds = 1.0f;
    // Note: Stencil ops can be added later if needed
};

struct ColorBlendAttachmentState
{
    bool blendEnable = false;
    BlendFactor srcColorBlendFactor = BlendFactor::One;
    BlendFactor dstColorBlendFactor = BlendFactor::Zero;
    BlendOp colorBlendOp = BlendOp::Add;
    BlendFactor srcAlphaBlendFactor = BlendFactor::One;
    BlendFactor dstAlphaBlendFactor = BlendFactor::Zero;
    BlendOp alphaBlendOp = BlendOp::Add;
    uint32_t colorWriteMask = 0xF; // RGBA

    friend bool operator==(const ColorBlendAttachmentState&,
                           const ColorBlendAttachmentState&) = default;
};

struct ColorBlendState
{
    bool logicOpEnable = false;
    // Fragment alpha becomes the multisample coverage mask (alpha-to-coverage). Rides the blend
    // state (the D3D12 model) even though Vulkan translates it into multisample state; the pass's
    // sample count still comes from the format key. At 1 sample the hardware degenerates it to a
    // hard 0.5 alpha cutoff, so single-sample paths should not enable it.
    bool alphaToCoverageEnable = false;
    std::vector<ColorBlendAttachmentState> attachments;
    float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct DynamicStateInfo
{
    std::vector<DynamicState> states;
};

// Enhanced Pipeline description with structured state
struct PipelineDesc
{
    PipelineType type = PipelineType::Graphics;

    // Shader stages
    std::vector<uint8_t> vertexShader;
    std::vector<uint8_t> pixelShader;
    std::vector<uint8_t> computeShader;
    std::vector<uint8_t> meshShader;
    std::vector<uint8_t> amplificationShader;

    // Descriptor set layouts
    std::vector<DescriptorSetLayoutDesc> descriptorSetLayouts;

    // Structured render state
    RasterizationState rasterizationState;
    DepthStencilState depthStencilState;
    ColorBlendState colorBlendState;
    DynamicStateInfo dynamicState;

    // Optional push constant declaration for pipeline layout caching/dedup
    // 0 means: use backend policy (Vulkan defaults to 128 bytes if reflection is not present)
    uint32_t pushConstantSize = 0;
    // Stage mask is backend-agnostic numeric; 0 means: use backend policy (Vulkan defaults to ALL stages)
    uint32_t pushConstantStagesMask = 0;

    // Optional: multiple named push constant ranges (packed with 4-byte alignment). If empty, backends use single range from pushConstantSize/mask
    struct PushConstantRangeDesc
    {
        std::string name;
        uint32_t size = 0;
        uint32_t stagesMask = 0;
        uint32_t offset = 0;
    };
    std::vector<PushConstantRangeDesc> pushConstantRanges;

    // Vertex input descriptions
    std::vector<VertexInputBinding> vertexBindings;

    // Dynamic rendering formats (optional; used when the active backend supports dynamic rendering-like features).
    // If empty, the backend will fall back to its default surface/swapchain format for simple pipelines.
    // Numeric format values keep this header backend-agnostic; each backend interprets them using its native format enum.
    std::vector<uint32_t> colorAttachmentFormats; // Backends map appropriately
    uint32_t depthAttachmentFormat = 0;           // 0 = undefined

    std::vector<VertexInputAttribute> vertexAttributes;

    // Optional stencil format for dynamic rendering; 0 = undefined. Backend interprets numeric value in its own format space.
    uint32_t stencilAttachmentFormat = 0;

    // Multisampling: pipeline rasterization sample count (must match color attachment samples)
    uint32_t rasterizationSamples = 1;

    PrimitiveTopology topology = PrimitiveTopology::TriangleList;

    const char* debugName = nullptr;

    // Specialization constants for shader optimization
    const class SpecializationConstants* specializationConstants = nullptr;

    // Helper methods for common configurations
    void SetCullingMode(CullModeFlags cullMode, FrontFace frontFace = FrontFace::CounterClockwise)
    {
        rasterizationState.cullMode = cullMode;
        rasterizationState.frontFace = frontFace;
    }

    void EnableDepthTest(bool enable = true, CompareOp compareOp = CompareOp::Greater)
    {
        depthStencilState.depthTestEnable = enable;
        depthStencilState.depthWriteEnable = enable;
        depthStencilState.depthCompareOp = compareOp;
    }

    void EnableBlending(bool enable = true, BlendFactor srcFactor = BlendFactor::SrcAlpha,
                        BlendFactor dstFactor = BlendFactor::OneMinusSrcAlpha)
    {
        if (colorBlendState.attachments.empty())
        {
            colorBlendState.attachments.resize(1);
        }
        colorBlendState.attachments[0].blendEnable = enable;
        colorBlendState.attachments[0].srcColorBlendFactor = srcFactor;
        colorBlendState.attachments[0].dstColorBlendFactor = dstFactor;
        colorBlendState.attachments[0].srcAlphaBlendFactor = srcFactor;
        colorBlendState.attachments[0].dstAlphaBlendFactor = dstFactor;
    }

    void AddDynamicState(DynamicState state)
    {
        // Avoid duplicate dynamic states (violates Vulkan VUID if duplicated)
        if (std::find(dynamicState.states.begin(), dynamicState.states.end(), state) == dynamicState.states.end())
        {
            dynamicState.states.push_back(state);
        }
    }
};

/**
 * @brief Abstract graphics device interface
 *
 * Provides unified interface for Vulkan and DirectX 12 backends.
 * Supports modern GPU-driven rendering with bindless resources.
 *
 * WSI usage (GLFW): When using GLFW for window system integration,
 * applications must call glfwInit() before calling IDevice::Initialize().
 * The Vulkan backend may query required instance extensions from GLFW during
 * initialization; initializing GLFW first ensures the correct extension list.
 * After device creation, create a GLFW window and call CreateAndActivateWindowTarget().
 *
 * Present semantics (timeline-first):
 * - Render passes end in COLOR_ATTACHMENT_OPTIMAL. Do not record COLOR_ATTACHMENT -> PRESENT barriers in passes.
 * - Device::Present performs the COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR transition and coordinates waits using
 *   timeline semaphores when available; when WSI acquire binaries must be consumed, the device bridges timelines to
 *   binary waits internally (validation-friendly).
 * - The acquire binary (imageAvailable) is never a wait in a vkQueueSubmit2 batch: a submit that needs it takes the
 *   legacy vkQueueSubmit path instead. Exactly one submit per frame consumes it -- the first graphics submit after
 *   acquire, or the present barrier submit when the frame did no backbuffer work.
 * - vkQueuePresentKHR waits on presentReady only.
 * - Offscreen (no swapchain): prefer calling FinalizeFrame() to signal the per-frame fence and rotate the frame index.
 *   Present() will forward to the same behavior when no swapchain is active.
 *
 * .shaderdesc usage:
 * - Backends do not perform file IO for metadata. Callers should load/apply .shaderdesc via MaterialHelper before creating pipelines.
 * - The helper uses ReflectionCache and prefers .shaderdesc when available; fall back to reflection in dev builds as needed.
 */

// Backend-agnostic push constant info for introspection
struct PipelinePushConstantInfo
{
    uint32_t size = 0;
    uint32_t stagesMask = 0;
};

// Detailed per-range info for multi-range push constants
struct PushConstantRangeInfo
{
    uint32_t id = 0;            // Stable index for this pipeline's lifetime
    const char* name = nullptr; // Optional; may be null/empty if unnamed
    uint32_t offset = 0;        // Aligned offset in bytes
    uint32_t size = 0;          // Declared size in bytes (before alignment padding)
    uint32_t stagesMask = 0;    // Backend-agnostic stage mask
};

// Forward declaration: engine-level pipeline variant cache

// One live GPU allocation, surfaced to the VRAM panel's "GPU allocations" list
// (the by-asset view, à la Godot's Video RAM tab). `bytes` is the real device
// footprint. Texture fields are 0 for buffers.
struct DebugResourceInfo
{
    enum class Kind
    {
        Buffer,
        Texture
    };
    Kind Type = Kind::Buffer;
    std::string Name;
    uint64_t Bytes = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    TextureFormat Format = TextureFormat::Unknown;
};

// One tracked validation-layer VUID: exact count plus first-occurrence context.
// Counts stay exact even when the log is thinned; `FirstMessage` is the full text
// of the first hit (truncated at the store's cap) with involved-object debug names
// and the command-buffer label stack captured alongside.
struct ValidationVuidStat
{
    std::string Vuid;
    bool IsError = false;
    uint64_t Count = 0;
    uint64_t SuppressedCount = 0; // hits matching the checked-in suppression table
    uint64_t FirstFrame = 0;
    uint64_t LastFrame = 0;
    std::string FirstMessage;
    std::string FirstObjects;
    std::string FirstLabels;
};

// Snapshot of validation-layer telemetry (GetValidationStats). Totals are exact
// and include messages past the per-VUID tracking cap, which only limits how many
// distinct VUIDs carry detailed entries (OverflowCount makes the cap visible).
struct ValidationStats
{
    bool Enabled = false; // validation layer active on this device
    uint64_t FrameSerial = 0;
    uint64_t ErrorCount = 0;
    uint64_t WarningCount = 0;
    uint64_t SuppressedCount = 0;
    uint64_t OverflowCount = 0; // messages recorded after the distinct-VUID cap filled
    std::vector<ValidationVuidStat> Vuids;
};

// One graphics tool the runtime reports as attached to a device — a capture,
// profiling or validation layer sitting between the engine and the driver.
struct AttachedGraphicsTool
{
    std::string Name;
    std::string Version;
    uint32_t    PurposeBits = 0; ///< Raw purpose flags, testable; Purposes is its rendering.
    std::string Purposes;        ///< Decoded purpose flags, e.g. "TRACING|ADDITIONAL_FEATURES".
    std::string Description;
    std::string Layer; ///< Layer implementing the tool, when the runtime names one; else empty.
};

// What diagnostic tooling is live on a device right now (GetGpuToolingReport).
//
// Every field is a runtime observation and never a compile-time one: a build
// with debug-label calls compiled into it still reports DebugLabelsAvailable
// false when the entry points did not resolve on this device, and that gap is
// exactly what asking is for.
struct GpuToolingReport
{
    // GPU debug labels and object names are callable on this device.
    bool DebugLabelsAvailable = false;
    // The engine enabled the validation layer on this instance. Request-derived,
    // not observed: a layer injected from outside through a loader environment
    // variable reads false here and surfaces in AttachedTools instead.
    // Independent of labels.
    bool ValidationLayerEnabled = false;
    // Whether the runtime could be asked which tools are attached. False means
    // no answer — a different fact from an empty AttachedTools, which means the
    // runtime answered that nothing is attached. A caller that collapsed the two
    // would report a clean device for one that was never queried.
    bool ToolingQueryAvailable = false;
    std::vector<AttachedGraphicsTool> AttachedTools;
};

class IDevice
{
  public:
    IDevice();
    virtual ~IDevice();

    IDevice(const IDevice&) = delete;
    IDevice& operator=(const IDevice&) = delete;

    // Device management
    virtual bool Initialize(const DeviceDesc& desc) = 0;
    virtual void Shutdown() = 0;
    virtual const RenderingDeviceCapabilities& GetCapabilities() const = 0;
    virtual GraphicsAPI GetAPI() const = 0;

    // The shader form this device's pipeline creation accepts. SPIR-V is the
    // engine's native cooked form and every desktop backend consumes it; a
    // backend that cannot (browser WebGPU has no SPIR-V ingestion) answers Wgsl
    // and the shader loaders serve the package's WGSL chunk instead.
    virtual ShaderSourceKind PreferredShaderSource() const { return ShaderSourceKind::SpirV; }

    // Device-loss health (Q6). Backends that can observe a device-lost error (or
    // a finite-wait fence timeout) surface it here so the editor / higher layers
    // can react (skip frames, hold the last-good frame, surface a toast) without
    // reaching into backend internals. Default: always Healthy for backends
    // without loss handling.
    virtual DeviceHealth GetDeviceHealth() const { return DeviceHealth::Healthy; }

    // Monotonic count of successful in-place device rebuilds (Q6). Zero means the
    // device has never been rebuilt. Bumped once per RebuildDevice success, before
    // the re-provision callbacks fire. Poll-based recovery consumers that hold no
    // device-rebuilt callback — the ECS component-handle pass (slice 3b), EZTree
    // regen, HLOD re-reconcile — compare this against a stored value each tick to
    // run their recovery exactly once per rebuild, on a thread that owns the ECS
    // World. Default: always 0 for backends without loss handling.
    virtual uint64_t GetDeviceRebuildGeneration() const { return 0; }

    // Q6 Tier-2 in-place device rebuild (slice 2). Rebuilds the device-scoped GPU
    // objects (logical device, queues, VMA, command infrastructure, per-window
    // swapchains) after a device loss, keeping the IDevice object (and so
    // every cached IDevice* and the retained pipeline SPIR-V) alive. Runs on the
    // render thread at a frame boundary. Backends without loss handling return
    // false. Returns true iff the device came back functional; on success the
    // health moves to AwaitingReprovision and RegisterDeviceRebuiltCallback
    // consumers have fired.
    virtual bool RebuildDevice() { return false; }

    // Poll-driven rebuild retry. The render loop MUST call this every tick,
    // UNCONDITIONALLY (before any device-health render-skip gate) — a failed rebuild
    // suppresses rendering, so a retry driven off the render/submit path would starve
    // (the M3 gap). The backend runs the bounded, timed-backoff retry from the health
    // state here so a real-TDR adapter reset can settle before the next vkCreateDevice.
    // No-op unless the device is in a latched-loss state. Backends without loss
    // handling default to nothing.
    virtual void TickDeviceRecovery() {}

    // Re-provision seam (slice 3a consumes this; slice 2 only defines it). After a
    // rebuild the device is functional but the upper layers hold dead handles, so
    // the health sits at AwaitingReprovision and rendering stays suppressed.
    // Callbacks registered here fire (in registration order) at the end of a
    // successful RebuildDevice, AFTER the health has moved to AwaitingReprovision
    // so the device reports IsDeviceUsable() during the callback — a consumer can
    // create buffers / upload textures to recreate its GPU resources, and (once it
    // has restored a renderable picture) call NotifyReprovisionComplete() to return
    // the device to Healthy. Registration is idempotent per `id`, and the callbacks
    // are kept rather than consumed, so a second rebuild in the same session fires
    // them again. RegisterPerDeviceCacheCleanup is the opposite-timing counterpart:
    // it fires BEFORE the GPU objects die, to drop handles rather than recreate them.
    void RegisterDeviceRebuiltCallback(const char* id, std::function<void(IDevice*)> callback);

    // Signals that the upper-layer re-provision pass has completed: moves the
    // health from AwaitingReprovision back to Healthy so rendering resumes. No-op
    // unless currently AwaitingReprovision. Backends override; the default is a
    // no-op for backends without loss handling.
    virtual void NotifyReprovisionComplete() {}

    // Descriptor-buffer runtime gate. True iff the device supports the feature
    // AND the backend's runtime environment flag turned the path on at device
    // init. Default base-class impl returns false so non-Vulkan backends don't
    // need to override.
    virtual bool IsDescriptorBufferEnabled() const { return false; }

    // Human-readable capability report (no-op by default; backends may override)
    virtual void PrintCapabilityReport() const {}

    // Short hardware description suitable for surfacing in debug/profiler UI.
    // Empty string means the backend does not expose it. Backends may return
    // something like "Apple M2 Max  Vulkan 1.3  driver 2.3.0".
    virtual std::string GetHardwareDescription() const { return {}; }

    // Multi-range introspection
    virtual uint32_t GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const = 0;

    // Texture views
    // Default implementations return invalid handles; backends should override when supported
    virtual TextureViewHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
    {
        (void)texture;
        (void)desc;
        return INVALID_TEXTURE_VIEW_HANDLE;
    }
    virtual void DestroyTextureView(TextureViewHandle) {}

    virtual bool GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id, PushConstantRangeInfo& outInfo) const = 0;
    virtual bool FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const = 0;

    // Queue types and indices (for multi-queue)
    enum class QueueType
    {
        Graphics,
        Compute,
        Transfer
    };
    virtual uint32_t GetGraphicsQueueFamilyIndex() const = 0;
    virtual uint32_t GetComputeQueueFamilyIndex() const = 0;
    virtual uint32_t GetTransferQueueFamilyIndex() const = 0;

    // Introspection
    virtual bool GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const = 0;

    // Resource creation (parameter-based)
    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;

    // Backend pipeline creation. Called by the IDevice base's
    // `GetOrCreate{Graphics,Compute}Pipeline` on cache miss. Backends
    // implement these directly against their native pipeline-create APIs.
    virtual PipelineHandle CreateConcreteGraphicsPipeline(
        const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk) = 0;
    virtual PipelineHandle CreateConcreteComputePipeline(
        const ComputePipelineDesc& cd) = 0;

    // Convenience: build a typed desc from a `PipelineDesc` builder, intern,
    // and resolve through the cache. Used by tests and `PipelineBuilder` —
    // production code should construct `GraphicsPipelineDesc` /
    // `ComputePipelineDesc` directly and call `Intern*` + `GetOrCreate*` to
    // avoid the translation hop.
    PipelineHandle CreatePipeline(const PipelineDesc& desc);

    // Resource destruction
    virtual void DestroyBuffer(BufferHandle handle) = 0;
    virtual void DestroyTexture(TextureHandle handle) = 0;
    virtual void DestroySampler(SamplerHandle handle) = 0;
    virtual void DestroyPipeline(PipelineHandle handle) = 0;
    virtual bool IsTextureHandleLive(TextureHandle texture) const
    {
        return texture.IsValid();
    }

    // Resource access.
    // MapBuffer's returned pointer is valid only while the underlying
    // allocation lives: an in-place device rebuild (device-loss recovery)
    // frees every allocation, and the pointer does not observe it. Callers on
    // the frame-driving thread are safe by construction (the rebuild runs on
    // that thread); any other thread must bound the whole map..use..unmap
    // window against the rebuild — on Vulkan, VulkanDevice::ScopedBufferMap.
    virtual void* MapBuffer(BufferHandle handle) = 0;
    /// Whether a read-map on this buffer is live or still resolving. Web's
    /// map is asynchronous and a buffer in that window must not be written by
    /// a new submit; native maps resolve synchronously and never report busy.
    virtual bool IsBufferMapBusy(BufferHandle) const { return false; }
    virtual void UnmapBuffer(BufferHandle handle) = 0;
    virtual void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) = 0;
    // The default writes range by range through UpdateBuffer and cannot check a
    // batch before it writes, so it does not keep the all-or-nothing rule. An
    // override checks that every non-empty range has data and lies inside the
    // buffer before it writes any, and writes none of them when one fails.
    virtual void UpdateBufferRanges(BufferHandle handle, std::span<const BufferUpdateRange> ranges)
    {
        for (const auto& range : ranges)
            if (range.size != 0)
                UpdateBuffer(handle, range.offset, range.size, range.data);
    }

    // Declare that [offset, offset+size) of a mapped buffer is written through
    // the pointer MapBuffer returned, and must be visible to GPU work submitted
    // after this frame's writes complete.
    //
    // A mapping is whole-buffer dirty from MapBuffer until the next submit, so
    // a producer that writes once at initialization needs no declaration. Past
    // that point a persistent mapping — one held open across frames — must
    // declare every region it writes: the range is what the frame produced, and
    // undeclared bytes are not guaranteed to reach the GPU.
    //
    // Ranges may overlap, may arrive in any order, and may be declared before
    // the bytes are written, as long as the writes complete before the submit
    // that consumes them. Safe to call concurrently from several threads.
    //
    // Backends whose mapping IS the GPU allocation (Vulkan, Metal, D3D12,
    // coherent host-visible memory) have nothing to do — the write already
    // landed, and this is a no-op. WebGPU cannot map GPU memory into the page:
    // MapBuffer hands out a CPU shadow and only declared ranges are copied to
    // the queue, so an undeclared write is stale there and correct everywhere
    // else.
    virtual void FlushMappedRange(BufferHandle handle, size_t offset, size_t size)
    {
        (void)handle;
        (void)offset;
        (void)size;
    }

    // GPU virtual address of the buffer's first byte. The buffer must have been
    // created with BufferUsage::ShaderDeviceAddress (or on Vulkan, with
    // the descriptor-buffer path active, which currently forces the bit on
    // every buffer). Used to pass buffer contents to shaders via
    // GL_EXT_buffer_reference instead of an SSBO/UBO descriptor. Default
    // returns 0 so non-Vulkan backends don't need to override until they
    // gain buffer-device-address support.
    virtual uint64_t GetBufferDeviceAddress(BufferHandle /*handle*/) { return 0; }

    // Where a buffer's memory actually ended up, as resolved by the allocator.
    //
    // Capabilities describe what a device CAN do; this describes what one
    // allocation DID. The two answer different questions and the second cannot
    // be inferred from the first: a large resizable-BAR window means a mapped
    // write can land in VRAM, while the residency policy the buffer was created
    // under decides whether it was even a candidate.
    //
    // `reported` is false when the backend cannot answer, so an unimplemented
    // backend never masquerades as a device with no device-local memory.
    struct BufferMemoryResidency
    {
        bool reported = false;
        bool deviceLocal = false;
        bool hostVisible = false;
        bool hostCoherent = false;
        // Index and size of the heap the allocation landed in. Size is what
        // makes the index meaningful across devices.
        uint32_t heapIndex = 0;
        uint64_t heapSizeBytes = 0;
    };
    virtual BufferMemoryResidency GetBufferMemoryResidency(BufferHandle /*handle*/) const { return {}; }

    // Acceleration-structure backend for the ray-query shadow-mask lane.
    // Null whenever RenderingDeviceCapabilities::supportsRayQuery is false
    // (extension missing, feature unsupported, or a fallback device-creation
    // path) — callers gate on it and the flag stays silently inert.
    virtual IAccelerationStructureBackend* GetAccelerationStructureBackend() { return nullptr; }

    // Command list creation (queue-specific)
    virtual std::unique_ptr<CommandList> CreateCommandList(QueueType queue) = 0;

    // Secondary command list creation for parallel recording (Phase 4).
    // Returns a command list that must be begun with BeginSecondary() and
    // executed via primary->ExecuteSecondary(). Default returns nullptr
    // (backend does not support secondary command lists).
    virtual std::unique_ptr<CommandList> CreateSecondaryCommandList(QueueType queue);

    // Mark secondary command lists as submitted so their underlying GPU
    // resources are tracked for frame-based recycling. Must be called after
    // ExecuteSecondary() and before the unique_ptrs are destroyed.
    // Default is a no-op (each CL destructor handles its own cleanup).
    virtual void RetireSecondaryCommandLists(CommandList* const* lists, uint32_t count) { (void)lists; (void)count; }

    // Timeline semaphore (opaque) management – default no-op for backends without support
    virtual SemaphoreHandle CreateTimelineSemaphore(uint64_t initialValue = 0)
    {
        (void)initialValue;
        return INVALID_HANDLE;
    }
    virtual void DestroySemaphore(SemaphoreHandle) {}
    virtual bool QueueSubmit(QueueType queue,
                             const std::vector<CommandList*>& cmdLists,
                             const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                             const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores)
    {
        (void)queue;
        (void)cmdLists;
        (void)waitSemaphores;
        (void)signalSemaphores;
        return false;
    }

    // Timeline semaphore introspection/waits (deterministic GPU->CPU sync without device-wide stalls).
    // Default implementations return false for backends without timeline semaphore support.
    //
    // NOTE:
    // - For Vulkan: backed by vkGetSemaphoreCounterValue / vkWaitSemaphores (timeline).
    // - For D3D12: can be backed by ID3D12Fence GetCompletedValue / SetEventOnCompletion.
    //
    // These helpers are intentionally general-purpose so they can be reused for:
    // - texture/buffer readbacks (UI replays, camera bookmarks, thumbnails)
    // - async GPU compute results
    // - staged resource uploads when a CPU consumer needs completion.
    virtual bool GetTimelineSemaphoreValue(SemaphoreHandle /*sem*/, uint64_t& /*outValue*/) const { return false; }
    virtual bool WaitTimelineSemaphoreValue(SemaphoreHandle /*sem*/, uint64_t /*value*/, uint64_t /*timeoutNs*/ = ~0ull) { return false; }

    struct GpuSyncToken
    {
        // Timeline semaphore + target value.
        SemaphoreHandle sem{};
        uint64_t value = 0;
        bool IsValid() const { return sem.IsValid() && value != 0; }
    };

    // Completion state of a GpuSyncToken. Absence of a token is Unknown, never
    // Complete: a token is absent both when nothing was ever submitted and when the
    // backend publishes no timeline, and the token cannot tell those apart. A gate
    // that reuses memory the GPU may still be reading must treat Unknown as "not
    // proven finished" and fall back to evidence that does not need a timeline.
    enum class GpuSyncStatus : uint8_t
    {
        Unknown,  // no token, or a timeline this backend cannot read — no information
        Pending,  // the GPU has not reached the token's value
        Complete  // the GPU has passed the token's value
    };

    // Non-blocking poll — never stalls.
    GpuSyncStatus QueryGpuSyncToken(const GpuSyncToken& t) const
    {
        if (!t.IsValid())
            return GpuSyncStatus::Unknown;
        uint64_t completed = 0;
        if (!GetTimelineSemaphoreValue(t.sem, completed))
            return GpuSyncStatus::Unknown;
        return completed >= t.value ? GpuSyncStatus::Complete : GpuSyncStatus::Pending;
    }

    // Blocks until the token's value is reached. False when the token carries no
    // completion information — never a claim that unobservable work finished.
    bool WaitGpuSyncToken(const GpuSyncToken& t, uint64_t timeoutNs = ~0ull)
    {
        if (!t.IsValid())
            return false;
        return WaitTimelineSemaphoreValue(t.sem, t.value, timeoutNs);
    }

    // The graphics timeline value the most recent graphics submit will signal. Waiting it
    // retires exactly the graphics work already submitted — the scoped alternative to
    // WaitForIdle for a caller that must not rewrite state (descriptor elements, persistent
    // buffers) the GPU may still be reading. Unlike a device drain it makes no device-wide
    // idleness claim, so it never reroutes deferred destruction to the immediate path.
    // Invalid before the first submit, and on a backend that keeps no graphics timeline —
    // the same condition under which the rest of this token vocabulary (readback rings,
    // SubmitTextureUploads, RGFrame::SubmissionToken) is unavailable. Both cases read as
    // GpuSyncStatus::Unknown, so a caller that must distinguish them owns its own evidence.
    virtual GpuSyncToken LastGraphicsSubmissionToken() const { return {}; }

    // True when the most recently submitted graphics work has completed on the
    // GPU (a non-blocking timeline/fence poll — never stalls). A consumer that
    // reduces a one-frame-stale GPU->CPU readback mirror during the next frame's
    // setup uses this to reject a mirror that is actually up to frames-in-flight
    // stale under GPU lag: the device write it summed against and the CPU-side
    // per-slice layout would otherwise describe different frames. Default false: a
    // backend that polls nothing has no evidence of completion, and deriving one
    // from GetFramesInFlight() would read a second unimplemented default as proof.
    // Backends that can poll must override; consumers treat false as "mirror
    // untrustworthy", which is the conservative branch by construction.
    virtual bool IsPreviousFrameGraphicsComplete() const { return false; }

    // Batch texture upload request for SubmitTextureUploads.
    struct TextureUploadRequest
    {
        TextureHandle Texture;       // Must already exist with TransferDst usage
        const void* Pixels;          // CPU pixel data — copied into staging before call returns
        uint32_t Width;
        uint32_t Height;
        size_t RowPitchBytes;        // Bytes per row (e.g. Width * BytesPerPixel)
        uint32_t MipLevel = 0;
        uint32_t ArrayLayer = 0;
    };

    // Submit one or more texture uploads to the dedicated transfer queue.
    // CPU pixel data is copied into internal staging buffers before the call
    // returns — the caller may free source buffers immediately.
    //
    // Returns a GpuSyncToken that becomes signaled when all transfers complete
    // on the GPU. Poll via QueryGpuSyncToken() or block via WaitGpuSyncToken().
    // After the token is signaled the textures are in ShaderResource state.
    //
    // Thread safety: may be called from any thread.
    //
    // Requires a dedicated transfer queue. Returns an invalid token if none exists.
    virtual GpuSyncToken SubmitTextureUploads(const TextureUploadRequest* /*requests*/, uint32_t /*count*/)
    {
        return {};
    }

    // Execution and synchronization
    virtual void ExecuteCommandLists(const std::vector<CommandList*>& commandLists) = 0;
    virtual void WaitForIdle() = 0;

    // Monotonic count of WaitForIdle() calls. The GPU-resource-lifetime policy reserves the
    // device-wide drain for shutdown and device-rebuild teardown, so a test can pin a runtime
    // path at zero added drains. Backends that do not track it report 0 — an oracle must
    // therefore prove the counter moves (drain once, assert +1) before trusting a zero delta.
    virtual uint64_t GetIdleDrainCount() const { return 0; }

    // Frame management
    virtual bool BeginFrame() = 0;
    virtual void Present() = 0;
    // Called by higher layers at the end of a frame to finalize synchronization when no Present occurs
    // Default no-op; backends may signal per-frame fences here for offscreen frames
    virtual void FinalizeFrame() {}

    // Compile-time upper bound for per-frame resource arrays across all backends.
    // Backends must not exceed this value for their runtime frames-in-flight count.
    static constexpr uint32_t kMaxSupportedFramesInFlight = 4;

    // Frames-in-flight introspection (for frame-safe transient allocation and resource reuse).
    // Defaults are conservative for backends without explicit multi-frame pacing.
    virtual uint32_t GetFramesInFlight() const { return 1u; }
    // Current frame slot index in [0, GetFramesInFlight()) when applicable (default 0).
    virtual uint32_t GetFrameIndex() const { return 0u; }
    // Multi-window frame slot index for a specific presentation target.
    // Single-target backends can forward to GetFrameIndex().
    virtual uint32_t GetFrameIndexForWindowTarget(WindowTargetHandle target) const
    {
        (void)target;
        return GetFrameIndex();
    }
    // Convenience helper for the currently active presentation target.
    virtual uint32_t GetActiveWindowTargetFrameIndex() const
    {
        return GetFrameIndexForWindowTarget(GetActiveWindowTarget());
    }
    // Swapchain format for a specific presentation target, readable without
    // switching actives (the GetFrameIndexForWindowTarget contract). This is
    // what severs a caller's finalize sizing from active-target ordering: a
    // window owner resolves its own handle per frame instead of reading
    // whichever target happens to be active. Invalid handle -> Unknown, never
    // a silently inherited neighbour's format. Single-target backends forward
    // to the live swapchain read.
    virtual TextureFormat GetWindowTargetSwapchainFormat(WindowTargetHandle target) const
    {
        return target.IsValid() ? GetSwapchainTextureFormat() : TextureFormat::Unknown;
    }

    // Per-frame sync timing (best-effort; backend-specific).
    // Used to detect CPU stalls on GPU fences / WSI acquire / present.
    struct FrameSyncTimings
    {
        uint32_t frameIndex = 0;
        // Time spent waiting for per-frame fences in BeginFrame().
        double beginFrameWaitMs = 0.0;
        bool waitedGraphicsFence = false;
        bool waitedComputeFence = false;
        bool waitedTransferFence = false;

        // Time spent acquiring a swapchain image (vkAcquireNextImageKHR or equivalent).
        double acquireMs = 0.0;
        bool acquireTimedOut = false;

        // Time spent submitting the "present transition" (queue submit for PRESENT barrier).
        double presentTransitionSubmitMs = 0.0;

        // Time spent in the actual present call (vkQueuePresentKHR / Present()).
        double presentMs = 0.0;

        // Wall-clock PERIOD in milliseconds between consecutive frame-end GPU
        // timestamps — frame-to-frame spacing on the GPU timeline, NOT time the
        // GPU spent busy. Measured by writing one GPU timestamp into each frame's present
        // command buffer and reading the delta when the slot's fence signals
        // next cycle. Every gap between frames is inside it: vsync waits,
        // compositor pacing, and any CPU stall that delayed the next submit.
        //
        // It therefore bounds NOTHING about GPU occupancy, and the difference
        // against the per-pass gpuMs sum is not "unmeasured GPU work". A
        // display-paced frame reads ~260 ms here against ~0.5 ms of real pass
        // work, with the GPU parked at its lowest power state the whole time.
        // Read beginFrameWaitMs to tell a stalled frame from a busy one before
        // attributing any of this period to the GPU.
        // 0 until enough frames have elapsed to compute a delta.
        double frameGpuPeriodMs = 0.0;
    };
    virtual bool GetLastFrameSyncTimings(FrameSyncTimings& /*out*/) const { return false; }

    // New: user-extensible per-frame waits for Present
    struct SemaphoreWait
    {
        SemaphoreHandle sem;
        uint64_t value = 0;
        uint64_t stageMask = 0;
    };
    virtual void RegisterEndOfFrameWait(const SemaphoreWait& /*wait*/) {}
    virtual void ClearEndOfFrameWaits() {}
    // New: overload that accepts detailed waits (default forwards to legacy Present)
    virtual void Present(const std::vector<SemaphoreWait>& /*extraWaits*/)
    {
        Present();
    }

    // Window presentation abstraction for backend-agnostic rendering.
    virtual WindowTargetHandle CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height) = 0;
    virtual bool DestroyWindowTarget(WindowTargetHandle target) = 0;
    // Explicitly drain GPU work associated with a window target before teardown.
    // Backends may implement a lightweight, target-scoped drain; default is a no-op.
    virtual bool DrainWindowTarget(WindowTargetHandle target, uint64_t timeoutNs = ~0ull)
    {
        (void)target;
        (void)timeoutNs;
        return true;
    }
    virtual bool SetActiveWindowTarget(WindowTargetHandle target) = 0;
    virtual WindowTargetHandle GetActiveWindowTarget() const = 0;
    virtual bool RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height) = 0;
    virtual bool GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const = 0;
    // Toggle vsync at runtime. Recreates the swapchain with the appropriate present mode.
    virtual void SetVsync(bool /*vsync*/) {}
    // The vsync state actually in effect (reflects startup config + any env
    // override + runtime SetVsync), so UI can show what the device is doing
    // rather than a possibly-stale saved preference.
    virtual bool IsVsyncEnabled() const { return false; }
    bool CreateAndActivateWindowTarget(void* windowHandle, uint32_t width, uint32_t height, WindowTargetHandle* outTarget = nullptr)
    {
        WindowTargetHandle target = CreateWindowTarget(windowHandle, width, height);
        if (!target.IsValid())
            return false;
        if (!SetActiveWindowTarget(target))
        {
            DestroyWindowTarget(target);
            return false;
        }
        if (outTarget)
        {
            *outTarget = target;
        }
        return true;
    }
    bool RecreateActiveWindowTargetSwapchain(uint32_t width, uint32_t height)
    {
        WindowTargetHandle target = GetActiveWindowTarget();
        if (!target.IsValid())
            return false;
        return RecreateWindowTargetSwapchain(target, width, height);
    }

    // Query current swapchain size if available; returns true on success
    virtual bool GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    virtual bool AcquireNextImage(uint32_t& imageIndex) = 0;
    virtual bool PresentImage(uint32_t imageIndex) = 0;
    virtual uint32_t GetSwapchainImageCount() const = 0;
    virtual TextureHandle GetSwapchainImage(uint32_t index) const = 0;

    // Resource management
    virtual ResourceManager* GetResourceManager() = 0;
    virtual IQueryPool* GetQueryPool() = 0;

    // Swapchain integration
    virtual TextureHandle GetCurrentSwapchainImageHandle() = 0;

    // Optional debug and profiling (can be no-op in release builds)
    virtual void BeginEvent(const char*) {}
    virtual void EndEvent() {}
    virtual void SetMarker(const char*) {}

    // Indirect commands moved to CommandList interface

    // Simplified descriptor management (move complex logic to CommandList)
    // New: Create descriptor set with explicit layout description
    virtual DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) = 0;
    // New: Update descriptor set bindings generically (buffers/images/samplers)
    virtual void UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update) = 0;
    virtual void UpdateDescriptorSetBatch(DescriptorSetHandle descriptorSet, std::span<const DescriptorSetUpdate> updates)
    {
        for (const auto& u : updates)
            UpdateDescriptorSet(descriptorSet, u);
    }
    virtual void DestroyDescriptorSet(DescriptorSetHandle descriptorSet) = 0;

    // Convenience descriptor helpers
    void UpdateBufferBinding(DescriptorSetHandle set, uint32_t binding, BufferHandle buffer, size_t offset, size_t size)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.type = DescriptorType::UniformBuffer;
        upd.buffers = {buffer};
        upd.bufferOffsets = {offset};
        upd.bufferRanges = {size};
        UpdateDescriptorSet(set, upd);
    }
    // Pass size = 0 to bind the whole buffer from `offset` (the backend substitutes the
    // buffer's actual size). Any non-zero value is used verbatim as the binding range;
    // range = 0 is illegal per the Vulkan spec (a descriptor's buffer range must be non-zero).
    void UpdateStorageBufferBinding(DescriptorSetHandle set, uint32_t binding, BufferHandle buffer, size_t offset, size_t size)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.type = DescriptorType::StorageBuffer;
        upd.buffers = {buffer};
        upd.bufferOffsets = {offset};
        if (size > 0)
            upd.bufferRanges = {size};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateImageBinding(DescriptorSetHandle set, uint32_t binding, TextureHandle texture, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Texture;
        upd.textures = {texture};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateImageBinding(DescriptorSetHandle set, uint32_t binding, TextureViewHandle view, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Texture;
        upd.textureViews = {view};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateStorageImageBinding(DescriptorSetHandle set, uint32_t binding, TextureHandle texture, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::StorageImage;
        upd.textures = {texture};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateStorageImageBinding(DescriptorSetHandle set, uint32_t binding, TextureViewHandle view, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::StorageImage;
        upd.textureViews = {view};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateSamplerBinding(DescriptorSetHandle set, uint32_t binding, SamplerHandle sampler, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Sampler;
        upd.samplers = {sampler};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateCombinedImageSamplerBinding(DescriptorSetHandle set, uint32_t binding, TextureHandle texture, SamplerHandle sampler, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::CombinedImageSampler;
        upd.textures = {texture};
        upd.samplers = {sampler};
        UpdateDescriptorSet(set, upd);
    }
    void UpdateCombinedImageSamplerBinding(DescriptorSetHandle set, uint32_t binding, TextureViewHandle view, SamplerHandle sampler, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::CombinedImageSampler;
        upd.textureViews = {view};
        upd.samplers = {sampler};
        UpdateDescriptorSet(set, upd);
    }
    // Array convenience helpers
    void UpdateUniformBuffers(DescriptorSetHandle set, uint32_t binding,
                              const std::vector<BufferHandle>& buffers,
                              const std::vector<size_t>& offsets,
                              const std::vector<size_t>& sizes,
                              uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::UniformBuffer;
        upd.buffers = buffers;
        upd.bufferOffsets = offsets;
        upd.bufferRanges = sizes;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateStorageBuffers(DescriptorSetHandle set, uint32_t binding,
                              const std::vector<BufferHandle>& buffers,
                              const std::vector<size_t>& offsets,
                              const std::vector<size_t>& sizes,
                              uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::StorageBuffer;
        upd.buffers = buffers;
        upd.bufferOffsets = offsets;
        upd.bufferRanges = sizes;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateImages(DescriptorSetHandle set, uint32_t binding,
                      const std::vector<TextureHandle>& textures,
                      uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Texture;
        upd.textures = textures;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateStorageImages(DescriptorSetHandle set, uint32_t binding,
                             const std::vector<TextureHandle>& textures,
                             uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::StorageImage;
        upd.textures = textures;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateSamplers(DescriptorSetHandle set, uint32_t binding,
                        const std::vector<SamplerHandle>& samplers,
                        uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Sampler;
        upd.samplers = samplers;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateCombinedImageSamplers(DescriptorSetHandle set, uint32_t binding,
                                     const std::vector<TextureHandle>& textures,
                                     const std::vector<SamplerHandle>& samplers,
                                     uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::CombinedImageSampler;
        upd.textures = textures;
        upd.samplers = samplers;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateImages(DescriptorSetHandle set, uint32_t binding,
                      const std::vector<TextureViewHandle>& views,
                      uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::Texture;
        upd.textureViews = views;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateStorageImages(DescriptorSetHandle set, uint32_t binding,
                             const std::vector<TextureViewHandle>& views,
                             uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::StorageImage;
        upd.textureViews = views;
        UpdateDescriptorSet(set, upd);
    }
    void UpdateCombinedImageSamplers(DescriptorSetHandle set, uint32_t binding,
                                     const std::vector<TextureViewHandle>& views,
                                     const std::vector<SamplerHandle>& samplers,
                                     uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate upd{};
        upd.binding = binding;
        upd.arrayElement = arrayElement;
        upd.type = DescriptorType::CombinedImageSampler;
        upd.textureViews = views;
        upd.samplers = samplers;
        UpdateDescriptorSet(set, upd);
    }

    // Convenience helpers for common buffer types
    BufferHandle CreateReadbackBuffer(size_t size, const char* name = nullptr)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Readback;
        desc.flags = BufferCreateFlags::PersistentlyMapped;
        desc.debugName = name;
        return CreateBuffer(desc);
    }
    BufferHandle CreateUploadBuffer(size_t size, const char* name = nullptr)
    {
        BufferDesc desc{};
        desc.size = size;
        // Upload buffers are staging-only and should not be bound directly as uniforms/vertex buffers.
        desc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.flags = BufferCreateFlags::PersistentlyMapped;
        desc.debugName = name;
        return CreateBuffer(desc);

        // Convenience: query texture formats using engine enum
        // Default implementations return Unknown and can be overridden by backends.
    }

    // ---- Pipeline cache --------------------------------------------
    //
    // The authoring surface separates *what the pipeline is* (the desc,
    // hashed and interned once) from *what render-pass formats it binds to*
    // (the small `PipelineFormatKey`, hashed at lookup time). This gives
    // shared identity for material variants across passes, no format-fill
    // leakage at call sites, and per-pass-format prewarm without
    // duplication. Implementations live on this base class and dispatch
    // through `m_PipelineCache`; backends only override `CreatePipeline`.

    // Intern: returns a stable id for this content; idempotent across calls.
    GraphicsPipelineId    InternGraphicsPipeline(const GraphicsPipelineDesc& desc);
    ComputePipelineId     InternComputePipeline(const ComputePipelineDesc& desc);
    DescriptorSetLayoutId InternDescriptorSetLayout(const DescriptorSetLayoutDesc& desc);

    // Read back the interned desc behind an id (nullptr if invalid/freed).
    // Used by callers that want to derive variants of an interned desc
    // (e.g. swapping vertex layout per-draw).
    const GraphicsPipelineDesc*    LookupGraphicsPipeline(GraphicsPipelineId id) const;
    const ComputePipelineDesc*     LookupComputePipeline(ComputePipelineId id) const;
    const DescriptorSetLayoutDesc* LookupDescriptorSetLayout(DescriptorSetLayoutId id) const;

    // Thread-safe by-value variant: Lookup* pointers alias cache storage that
    // concurrent interning may reallocate; backends doing slow work between
    // lookup and use (shader translation/compilation) must copy instead.
    bool CopyDescriptorSetLayout(DescriptorSetLayoutId id, DescriptorSetLayoutDesc& outDesc) const;
    bool CopyGraphicsPipeline(GraphicsPipelineId id, GraphicsPipelineDesc& outDesc) const;

    // Lookup-or-create against the cache, on the calling thread. On a miss the
    // base class calls the backend's CreateConcrete* virtual. Each key builds
    // once: a miss on a key whose asynchronous build is queued takes that build
    // over and runs it here, and a miss on a key another thread is building waits
    // for that build instead of creating a second pipeline.
    PipelineHandle GetOrCreateGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk);
    PipelineHandle GetOrCreateComputePipeline(ComputePipelineId id);

    // Cache probes that never build. Return an invalid handle when the
    // (id, formatKey) pair, or the compute id, has no live concrete pipeline. A
    // thread that must not stall — a render-graph record worker, where a backend
    // build costs hundreds of ms (shader translation plus PSO creation) — probes
    // with these and asks for the build with Request*Pipeline instead.
    PipelineHandle TryGetWarmGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk) const;
    PipelineHandle TryGetWarmComputePipeline(ComputePipelineId id) const;

    // Asks for a pipeline without building it on the calling thread. Warm when
    // it is cached; otherwise the first request hands one build to the pipeline
    // build dispatcher and every request reads Pending until it lands, then Warm
    // (fetch the handle with TryGetWarm*) or Failed. A Failed pipeline is not
    // built again until the pipeline cache is invalidated (hot reload) or the
    // device is rebuilt. With no dispatcher installed, or on a device that
    // cannot create resources off its owning thread, the build runs inline and
    // the result is Warm or Failed on return. Thread-safe.
    PipelineBuildState RequestGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk);
    PipelineBuildState RequestComputePipeline(ComputePipelineId id);

    // Runs the builds Request*Pipeline queues: called once per build with a job
    // that builds and publishes the pipeline. The Rendering layer has no job
    // system, so the engine installs one (MaterialSystem, onto its compile-gated
    // prewarm workers). The dispatcher may run the job inline, later on any
    // thread, or destroy it without running it (shutdown); a destroyed job
    // releases its request, so the pipeline reads as never requested. An empty
    // function removes the dispatcher; the owner removes it before it stops
    // accepting jobs, and jobs it already holds must end before the device is
    // destroyed.
    using PipelineBuildDispatcher = std::function<void(std::function<void()>)>;
    void SetPipelineBuildDispatcher(PipelineBuildDispatcher dispatcher);

    // Returns the count of pipelines actually created (cache hits not counted).
    uint32_t PrewarmGraphicsPipeline(GraphicsPipelineId id, std::span<const PipelineFormatKey> fks);

    // Escape hatch — bypasses the cache for *this* call. Tests, screenshots,
    // debug overlays. Desc must be interned first; only the (id, formatKey)
    // combination is left un-cached.
    PipelineHandle CreateGraphicsPipelineDirect(GraphicsPipelineId id, const PipelineFormatKey& fk);

    PipelineCacheStats GetPipelineCacheStats() const;

    // Direct access to the pipeline cache. Used by hot-reload paths that need
    // tag-based invalidation; production draw paths should go through the
    // helpers above (`Intern* / GetOrCreate* / Prewarm*`).
    PipelineCache& GetMutablePipelineCache() { return *m_PipelineCache; }
    const PipelineCache& GetPipelineCache() const { return *m_PipelineCache; }
    std::string GetLastGraphicsPipelineFailure() const;

    // Descriptor allocator stats (for diagnostics on exit)
    struct DescriptorAllocatorStats
    {
        uint32_t TransientPools = 0, TransientAllocated = 0, TransientCapacity = 0, PersistentPools = 0, PersistentAllocated = 0, PersistentCapacity = 0;
    };
    virtual DescriptorAllocatorStats GetDescriptorAllocatorStats() const
    {
        return {};
    }

    // Optional lightweight per-frame bind counters for tests/diagnostics
    struct DebugBindCounters
    {
        uint32_t pipelineBindsGraphics = 0;
        uint32_t pipelineBindsCompute = 0;
        uint32_t descriptorBindsGraphics[8] = {0};
        uint32_t descriptorBindsCompute[8] = {0};
    };
    // Default no-op implementations; backends can override to expose counters
    virtual DebugBindCounters DebugGetBindCounters() const
    {
        return {};
    }
    virtual void DebugResetBindCounters() {}

    // Validation-layer telemetry mirroring the DebugBindCounters pattern: exact
    // per-VUID counts plus first-occurrence context. Empty (Enabled=false) for
    // backends without a validation layer.
    virtual ValidationStats GetValidationStats() const
    {
        return {};
    }
    virtual void ResetValidationStats() {}

    // Which tools are watching this device, and whether the engine's own debug
    // instrumentation is live on it. Backends that cannot enumerate attached
    // tools return the default, which reports no answer rather than no tools.
    virtual GpuToolingReport GetGpuToolingReport() const
    {
        return {};
    }

    // Debug/testing: Allocation stats (0 for backends without memory tracking)
    virtual size_t DebugGetAllocationCount() const
    {
        return 0;
    }
    virtual size_t DebugGetBufferRegistryCount() const
    {
        return 0;
    }
    virtual size_t DebugGetImageRegistryCount() const
    {
        return 0;
    }

    // Debug: total device-local bytes currently allocated by the backend's
    // memory allocator. Returns 0 for backends without byte-level tracking.
    virtual size_t DebugGetAllocatedBytes() const
    {
        return 0;
    }

    // Debug: enumerate every live GPU allocation (the VRAM panel's by-allocation
    // list). Invokes `fn` once per resource. No-op for backends without a
    // resource registry — the panel then falls back to its render-graph view.
    virtual void DebugEnumerateResources(const std::function<void(const DebugResourceInfo&)>& fn) const
    {
        (void)fn;
    }

    // Debug: VRAM bytes attributable to image (texture / render-target) and
    // buffer (vertex/index/uniform/storage) allocations. Returns 0 for
    // backends without per-category tracking. Sum may differ from
    // DebugGetAllocatedBytes() when intermediate / pool blocks are included.
    virtual size_t DebugGetTextureBytes() const
    {
        return 0;
    }
    virtual size_t DebugGetBufferBytes() const
    {
        return 0;
    }

    // Debug/testing: number of resources retired during the last BeginFrame() (0 for backends without deferred retirement)
    virtual size_t DebugGetLastRetiredTextureCount() const
    {
        return 0;
    }
    virtual size_t DebugGetLastRetiredBufferCount() const
    {
        return 0;
    }

    // Per-pass descriptor capture for GPU debugging.
    // Enable capture for one frame, then retrieve the results.
    struct DebugDescriptorBinding
    {
        uint32_t Binding = 0;
        std::string ResourceType;
        uint64_t ResourceHandle = 0;
        std::string DebugName;
    };
    struct DebugDescriptorCapture
    {
        std::string PassName;
        uint32_t DrawIndex = 0;
        uint64_t DescriptorSetHandle = 0;
        uint32_t SetIndex = 0;
        std::vector<DebugDescriptorBinding> Bindings;
    };
    virtual void DebugSetDescriptorCaptureEnabled(bool enabled, const std::string& passFilter) { (void)enabled; (void)passFilter; }
    virtual bool DebugIsDescriptorCaptureEnabled() const { return false; }

    // True while the device fills every new float target and storage buffer with
    // NaN (GE_VK_FILL_NEW_TARGETS_NAN). A pool that recycles resources across
    // frames creates fresh ones instead while it is on, so a read before the
    // frame's first write reaches the fill rather than last frame's contents.
    virtual bool DebugFillsNewResourcesWithNaN() const { return false; }
    virtual void DebugClearDescriptorCaptures() {}
    virtual std::vector<DebugDescriptorCapture> DebugGetDescriptorCaptures() const { return {}; }

    // Comprehensive pool/container stats for verifying resource lifecycle stability.
    // Key invariant: in idle (no topology changes), these sizes should be constant
    // after a short warmup period.
    struct ResourcePoolStats
    {
        // Command buffer pools (summed across all frame slots)
        size_t cmdBuffersFree = 0;
        size_t cmdBuffersUsed = 0;
        // Deferred destroy queues
        size_t deferredBuffers = 0;
        size_t deferredTextures = 0;
        size_t deferredTextureViews = 0;
        size_t deferredSamplers = 0;
        // Upload staging buffers awaiting their transfer-timeline value. Counted
        // apart from deferredBuffers because they retire on a different signal:
        // the transfer timeline, not the per-queue destroy tags. A climb here and
        // a climb in deferredBuffers therefore have different causes.
        size_t deferredStagingBuffers = 0;
        // Live resource counts
        size_t liveBuffers = 0;
        size_t liveTextures = 0;
        size_t liveTextureViews = 0;
        size_t liveSamplers = 0;
        // Combined entry count of the per-view metadata the backend keeps
        // alongside each view (aspect, format, owning texture). All three are
        // written and erased together, so this is always 3 * liveTextureViews;
        // any other value means an entry was lost or duplicated.
        size_t textureViewMetadataEntries = 0;
        // Descriptor allocator
        size_t descriptorTransientPools = 0;
        size_t descriptorPersistentPools = 0;
    };
    virtual ResourcePoolStats GetResourcePoolStats() const
    {
        return {};
    }

    // Default implementations return Unknown and can be overridden by backends.
    virtual TextureFormat GetTextureFormat(TextureHandle /*texture*/) const
    {
        return TextureFormat::Unknown;
    }

    // Sample count of the underlying physical image. Lets passes pick a sampler
    // variant matching what is actually bound, even on a frame where a persistent
    // texture is being re-specced to a new sample count. Defaults to 1.
    virtual uint32_t GetTextureSampleCount(TextureHandle /*texture*/) const
    {
        return 1u;
    }
    // Extent of the underlying physical image. Externally-imported render
    // graph textures derive their declared extent from this — viewport and
    // letterbox math need it at declaration time, before any command list
    // exists. Defaults to 0x0 (unknown).
    virtual void GetTextureSize(TextureHandle /*texture*/, uint32_t& outWidth,
                                uint32_t& outHeight) const
    {
        outWidth = 0;
        outHeight = 0;
    }
    // Array layer count of the underlying physical image. An image bound where a
    // shader declares an array sampler must actually carry the layers that shader
    // indexes, and this is the direct probe for that: registering a per-layer view
    // is NOT, because an out-of-range layer still yields a non-zero bindless index
    // (TextureService::RegisterTextureBindless returns the slot even when the view
    // fails). Defaults to 1.
    virtual uint32_t GetTextureArrayLayers(TextureHandle /*texture*/) const
    {
        return 1u;
    }

    // Whether this texture's sampled descriptors claim GENERAL
    // (TextureDesc::sampledInGeneralLayout). The render graph mirrors it when
    // it can see a desc; an EXTERNAL import has only the handle, so it asks
    // here — without it the graph transitions sampled reads to ShaderReadOnly
    // against a descriptor that claims GENERAL (VUID-...-00344 / 09600).
    virtual bool GetTextureSampledInGeneralLayout(TextureHandle /*texture*/) const
    {
        return false;
    }
    // True when the handle resolves to a live texture that is NOT queued for
    // deferred destruction. Lifetime probe for externally-owned physicals
    // served to consumers outside the owner's destruction path (e.g. a
    // pool-adopted shadow map handed to non-graph code). Backends without
    // destruction tracking may conservatively report any valid handle alive.
    virtual bool IsTextureAlive(TextureHandle texture) const { return texture.IsValid(); }
    // True when the handle resolves to a live pipeline. MUST be virtual: the
    // global handle managers are header-inline singletons, so a statically
    // double-linked module (the Player links GameEngineRendering AND
    // Engine.dll — see Apps/Player/CMakeLists.txt) gets a second, EMPTY
    // table; only a virtual call lands in the device's home module where
    // the pipelines actually live. Never probe IsValidPipeline directly
    // from code that can execute in another module.
    virtual bool IsPipelineAlive(PipelineHandle pipeline) const { return pipeline.IsValid(); }
    // Drop-your-device-handles seam, for subsystems keeping a per-IDevice cache of
    // GPU handles (helper passes, editor gizmo textures). `cleanup` runs at the top
    // of EVERY event that destroys this device's GPU objects — shutdown, and the
    // in-place rebuild after device loss — always BEFORE the objects go away, so a
    // cleanup destroying its own handles is destroying live ones.
    //
    // The rebuild arm is the one that is easy to miss and impossible to see: the
    // IDevice POINTER survives RebuildDevice by design, so a cache keyed on it and
    // cleaned only at shutdown keeps serving handles from the destroyed logical device
    // for the rest of the session. This is the only registration for that, which is
    // what makes missing an arm unrepresentable rather than merely discouraged.
    //
    // RELEASE ONLY. On the rebuild arm this runs inside the device-rebuild lock,
    // against a device that is mid-teardown: destroy handles and drop state, never
    // create, upload, or wait. Re-creation belongs to the lazy path that revives the
    // cache, or to RegisterDeviceRebuiltCallback — opposite timing, different job,
    // fires AFTER a rebuild specifically so re-provisioning is legal there.
    //
    // Stored on the OBJECT — the predecessor (DeviceShutdownRegistry) was a
    // file-static map that exists once PER MODULE COPY, so a registration made in
    // one copy never fired when another copy's code tore the device down (the
    // double-link disease). `id` is unique per device; re-registering the same id
    // is ignored, so a lazy cache may re-register freely after being dropped.
    // Cleanups must be idempotent: they can fire more than once (rebuild, then
    // shutdown) with no re-population in between.
    void RegisterPerDeviceCacheCleanup(const char* id, std::function<void(IDevice*)> cleanup);
    virtual TextureFormat GetSwapchainTextureFormat() const
    {
        return TextureFormat::Unknown;
    }
    virtual HdrOutputState GetHdrOutputState() const
    {
        return {};
    }
    virtual HdrOutputMode GetActiveHdrOutputMode() const
    {
        return GetHdrOutputState().activeMode;
    }
    virtual bool SetHdrOutputMode(HdrOutputMode /*mode*/, const HdrStaticMetadata* /*metadata*/ = nullptr,
                                  HdrSwapchainBitDepth /*bitDepth*/ = HdrSwapchainBitDepth::Bit10)
    {
        return false;
    }

    // Per-backend format support query: can this device create `format` with
    // every bit in `usageFlags` (bitwise OR of TextureUsage values, matching
    // TextureDesc::usage)? Every backend answers for itself; there is no
    // permissive default, because a default of "supported" is how a missing
    // override reads as a capability. CreateTexture consults it through
    // TextureFormatSupportGate, so a backend that answers NO must also refuse
    // the create.
    virtual bool IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const = 0;

    // Helper: does the current swapchain format require a manual linear -> sRGB encode step?
    // This is primarily used by the Editor/UI to decide whether to add a final color-space
    // conversion pass before presenting. Backends should ensure GetSwapchainTextureFormat()
    // returns an accurate TextureFormat for the swapchain.
    bool SwapchainNeedsManualSRGBEncode() const
    {
        if (IsHdrOutputModeActive(GetActiveHdrOutputMode()))
            return false;
        TextureFormat fmt = GetSwapchainTextureFormat();
        switch (fmt)
        {
        case TextureFormat::RGBA8_UNORM:
        case TextureFormat::BGRA8_UNORM:
        case TextureFormat::RGB10A2_UNORM:
            // Linear UNORM formats presented to an sRGB framebuffer need explicit encode.
            return true;
        case TextureFormat::RGBA8_SRGB:
        case TextureFormat::BGRA8_SRGB:
            // Hardware performs sRGB encode on presentation.
            return false;
        default:
            // For HDR/float or unknown formats we conservatively assume no manual encode.
            // Callers that need special handling for those formats should check explicitly.
            return false;
        }
    }

    // True if the swapchain images were created with transfer-source usage, so the
    // backbuffer can be copied (read back) directly — e.g. for full-window screenshots.
    // Default false; backends opt in when the surface permits it.
    virtual bool SwapchainSupportsReadback() const
    {
        return false;
    }

  protected:
    // Intern tables + (id, formatKey) → handle map + tag-based selective
    // invalidation. Owned via unique_ptr so PipelineCache stays
    // forward-declared in this header (avoids include cycle).
    std::unique_ptr<PipelineCache> m_PipelineCache;

    // Invoke the registered per-device cache cleanups. Backends call this at the
    // top of EVERY path that destroys this device's GPU objects — Shutdown and the
    // rebuild teardown — before destroying anything, so a cleanup's own Destroy*
    // calls land on live objects. Kept, not cleared: a session can kill a device
    // more than once (rebuild, rebuild again, then shut down). It first cancels
    // the queued pipeline builds and waits for the running ones, so no build is
    // inside a backend create call while the objects it uses are destroyed.
    void InvokePerDeviceCacheCleanups();
    // Invoke the registered device-rebuilt callbacks (kept, not cleared — a device
    // can be rebuilt more than once). Backends call this at the end of a
    // successful RebuildDevice, AFTER entering AwaitingReprovision so the device is
    // usable for the GPU ops the consumers issue.
    void InvokeDeviceRebuiltCallbacks();
    void SetLastGraphicsPipelineFailure(std::string message) const;

  private:
    // One build of a pipeline-cache key: what it builds, the build-table ticket
    // it runs under, and the cache's invalidation epoch when it was asked for
    // (an invalidation after that refuses its publish).
    struct PipelineBuildRequest
    {
        uint64_t Key = 0;
        uint64_t Ticket = 0;
        uint64_t InvalidationEpoch = 0;
        bool IsCompute = false;
        uint32_t PipelineIdValue = 0;
        PipelineFormatKey FormatKey{};
    };
    PipelineBuildState RequestPipelineBuild(const PipelineBuildRequest& request);
    PipelineHandle TryGetWarmPipeline(const PipelineBuildRequest& request) const;
    // The single-flight join every synchronous miss goes through.
    PipelineHandle BuildPipelineOnCallingThread(const PipelineBuildRequest& request);
    // The job a queued request dispatches; builds only if its ticket still owns the key.
    void RunQueuedPipelineBuild(const PipelineBuildRequest& request);
    // Builds through the backend and publishes into the concrete cache unless the
    // cache was invalidated since the request's epoch; `published` reports which.
    PipelineHandle CreateAndPublishPipeline(const PipelineBuildRequest& request, bool& published);
    PipelineHandle CreateAndPublishGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk,
                                                    uint64_t invalidationEpoch, bool& published);
    PipelineHandle CreateAndPublishComputePipeline(ComputePipelineId id, uint64_t invalidationEpoch,
                                                   bool& published);

    // Queued, running and failed builds by cache key (Source/Core/PipelineBuildTable.h).
    std::unique_ptr<PipelineBuildTable> m_PipelineBuilds;
    mutable std::mutex m_PipelineBuildDispatcherMutex;
    std::shared_ptr<const PipelineBuildDispatcher> m_PipelineBuildDispatcher;

    // (id, cleanup) pairs; linear scan — registrations are few and one-time.
    std::mutex m_PerDeviceCacheCleanupMutex;
    std::vector<std::pair<std::string, std::function<void(IDevice*)>>> m_PerDeviceCacheCleanups;
    // (id, callback) pairs fired on each successful device rebuild. Kept across
    // rebuilds (a session may lose the device more than once); linear scan.
    std::mutex m_DeviceRebuiltCallbackMutex;
    std::vector<std::pair<std::string, std::function<void(IDevice*)>>> m_DeviceRebuiltCallbacks;
    mutable std::mutex m_GraphicsPipelineFailureMutex;
    mutable std::string m_LastGraphicsPipelineFailure;
};

/**
 * @brief Device factory for creating graphics devices
 */
class DeviceFactory
{
  public:
    static std::unique_ptr<IDevice> CreateDevice(const DeviceDesc& desc = {});
    static std::vector<GraphicsAPI> GetAvailableAPIs();
    static bool IsAPISupported(GraphicsAPI api);
};

} // namespace Rendering
} // namespace GameEngine
