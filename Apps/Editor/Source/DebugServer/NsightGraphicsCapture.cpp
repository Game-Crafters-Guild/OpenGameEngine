#include "DebugServer/NsightGraphicsCapture.h"

#include "Logger/Logger.h"

#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <NGFX_GraphicsCapture_Vulkan.h>

#include <algorithm>
#include <vector>
#endif

namespace GameEngine
{

#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
namespace
{

// NGFX_GraphicsCapture_RequestCapture_Vulkan_Params::framesToCapture accepts
// [1, 60]; the SDK rejects the request outright outside that range.
constexpr uint32_t kMinCaptureFrames = 1;
constexpr uint32_t kMaxCaptureFrames = 60;

// The SDK resolves its implementation libraries through this callback. It
// returns only modules ngfx-capture.exe already injected: a capture DLL must
// never be pulled off disk into an editor session that was not launched for
// capture. Installing it is mandatory rather than optional -- the SDK's own
// default callback (NGFX_LoadLib_UserOverrideRequired) calls abort().
void* LoadInjectedModuleOnly(const NGFX_PathChar* libName)
{
    return GetModuleHandleW(libName);
}

const char* ResultName(NGFX_Result result)
{
    switch (result)
    {
    case NGFX_Result_Success: return "Success";
    case NGFX_Result_NotImplemented: return "NotImplemented";
    case NGFX_Result_LibNotFound: return "LibNotFound";
    case NGFX_Result_InvalidLib: return "InvalidLib";
    case NGFX_Result_DifferentActivityInjected: return "DifferentActivityInjected";
    case NGFX_Result_InvalidParameter: return "InvalidParameter";
    case NGFX_Result_InvalidState: return "InvalidState";
    case NGFX_Result_UnspecifiedError: return "UnspecifiedError";
    case NGFX_Result_Timeout: return "Timeout";
    case NGFX_Result_InsufficientBuffer: return "InsufficientBuffer";
    default: return "Unknown";
    }
}

std::string ToUtf8(const wchar_t* text, int length)
{
    if (length <= 0)
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace
#endif

bool NsightGraphicsCapture::Initialize()
{
#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
    NGFX_SetLibraryLoadFn(&LoadInjectedModuleOnly);

    NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params params{};
    params.version = NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params_VER;

    const NGFX_Result result = NGFX_GraphicsCapture_InitializeActivity_Vulkan(&params);
    if (result != NGFX_Result_Success)
    {
        // Not being launched under ngfx-capture is the ordinary case, not a
        // fault: the injection libraries are simply absent from the process.
        if (result == NGFX_Result_InvalidState || result == NGFX_Result_LibNotFound)
            Logger::Log::Info("Nsight: capture libraries not injected (editor not launched under ngfx-capture)");
        else
            Logger::Log::Warning("Nsight: capture activity initialization failed ({})", ResultName(result));
        return false;
    }

    m_Initialized = true;
    Logger::Log::Info("Nsight: bound to the injected Graphics Capture activity");
    return true;
#else
    return false;
#endif
}

std::optional<std::string> NsightGraphicsCapture::TriggerCapture(uint32_t frameCount)
{
#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
    if (!m_Initialized)
        return "Nsight Graphics capture activity is not initialized";

    const uint32_t frames = std::clamp(frameCount, kMinCaptureFrames, kMaxCaptureFrames);

    NGFX_GraphicsCapture_RequestCapture_Vulkan_Params params{};
    params.version = NGFX_GraphicsCapture_RequestCapture_Vulkan_Params_VER;
    params.delimiter = NGFX_GraphicsCapture_Delimiter_Present;
    params.framesBeforeStart = 0;
    params.framesToCapture = frames;

    const NGFX_Result result = NGFX_GraphicsCapture_RequestCapture_Vulkan(&params);
    if (result != NGFX_Result_Success)
    {
        Logger::Log::Warning("Nsight: capture request rejected ({})", ResultName(result));
        return std::string("Nsight rejected the capture request (") + ResultName(result) + ")";
    }

    Logger::Log::Info("Nsight: capture requested ({} frame(s) from the next present)", frames);
    return std::nullopt;
#else
    (void)frameCount;
    return "Editor was built without the Nsight Graphics SDK";
#endif
}

uint32_t NsightGraphicsCapture::GetCaptureCount() const
{
#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
    if (!m_Initialized)
        return 0;

    NGFX_ArtifactFileCount_Params params{};
    params.version = NGFX_ArtifactFileCount_Params_VER;

    if (NGFX_GraphicsCapture_GetCaptureFileCount(&params) != NGFX_Result_Success)
        return 0;

    return params.count;
#else
    return 0;
#endif
}

std::string NsightGraphicsCapture::GetLastCapturePath() const
{
#if defined(_WIN32) && defined(GE_HAVE_NGFX_SDK)
    const uint32_t count = GetCaptureCount();
    if (count == 0)
        return {};

    NGFX_ArtifactFilePath_Params params{};
    params.version = NGFX_ArtifactFilePath_Params_VER;
    params.artifactIndex = count - 1;

    // Sizing call: a null buffer reports the capacity the path needs, including
    // its null terminator. Only requiredPathCapacity is meaningful here, so
    // either "fits" or "too small" answers the question.
    NGFX_Result result = NGFX_GraphicsCapture_GetCaptureFilePath(&params);
    if (result != NGFX_Result_Success && result != NGFX_Result_InsufficientBuffer)
    {
        Logger::Log::Warning("Nsight: capture path size query failed ({})", ResultName(result));
        return {};
    }
    if (params.requiredPathCapacity == 0)
        return {};

    std::vector<wchar_t> buffer(params.requiredPathCapacity);
    params.filePath = buffer.data();
    params.filePathCapacity = static_cast<uint32_t>(buffer.size());

    result = NGFX_GraphicsCapture_GetCaptureFilePath(&params);
    if (result != NGFX_Result_Success)
    {
        Logger::Log::Warning("Nsight: capture path query failed ({})", ResultName(result));
        return {};
    }

    return ToUtf8(buffer.data(), static_cast<int>(wcsnlen(buffer.data(), buffer.size())));
#else
    return {};
#endif
}

} // namespace GameEngine
