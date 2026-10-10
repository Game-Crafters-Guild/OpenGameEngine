#include "Assets/SvgRasterizer.h"

#include "Logger/Logger.h"
#include "Platform/Capabilities.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <mutex>
#include <system_error>

#if defined(GE_HAVE_THORVG)
#include <thread>
#include <thorvg.h>
#endif

namespace GameEngine {

namespace
{
constexpr uint32 kMaxThorvgThreads = 4;

// Accessed from multiple threads (editor settings write, asset loaders read).
static std::atomic<float32> s_UiDefaultSize{256.0f};
static std::atomic<float32> s_TextureDefaultSize{256.0f};

#if defined(GE_HAVE_THORVG)
static std::once_flag s_ThorvgInitFlag;
// Relaxed ordering is safe: std::call_once provides the happens-before
// guarantee that makes the store in the lambda visible to all readers.
static std::atomic<bool> s_ThorvgInitResult{false};

#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
struct ThorvgPaintDeleter
{
    void operator()(tvg::Paint* paint) const noexcept
    {
        tvg::Paint::rel(paint);
    }
};

using ThorvgPicturePtr = std::unique_ptr<tvg::Picture, ThorvgPaintDeleter>;
using ThorvgCanvasPtr = std::unique_ptr<tvg::SwCanvas>;

static ThorvgPicturePtr MakeThorvgPicture()
{
    return ThorvgPicturePtr(tvg::Picture::gen());
}

static ThorvgCanvasPtr MakeThorvgCanvas()
{
    return ThorvgCanvasPtr(tvg::SwCanvas::gen());
}
#else
using ThorvgPicturePtr = std::unique_ptr<tvg::Picture>;
using ThorvgCanvasPtr = std::unique_ptr<tvg::SwCanvas>;

static ThorvgPicturePtr MakeThorvgPicture()
{
    return tvg::Picture::gen();
}

static ThorvgCanvasPtr MakeThorvgCanvas()
{
    return tvg::SwCanvas::gen();
}
#endif

static bool EnsureThorvgInitialized()
{
    std::call_once(s_ThorvgInitFlag, []()
    {
        // ThorVG's pool is additional to the engine's reserved workers.
        // Zero selects its synchronous rasterizer without creating pthreads.
        const uint32 threads = Platform::SupportsAuxiliaryThreadPools()
            ? std::min(kMaxThorvgThreads, std::max(1u, std::thread::hardware_concurrency()))
            : 0u;
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        const tvg::Result r = tvg::Initializer::init(threads);
#else
        const tvg::Result r = tvg::Initializer::init(tvg::CanvasEngine::Sw, threads);
#endif
        if (r != tvg::Result::Success)
        {
            Logger::Log::Error("RasterizeSvgToRgba: ThorVG Initializer::init failed with code {}", static_cast<int>(r));
            s_ThorvgInitResult.store(false, std::memory_order_relaxed);
            return;
        }
        s_ThorvgInitResult.store(true, std::memory_order_relaxed);
    });
    return s_ThorvgInitResult.load(std::memory_order_relaxed);
}
#endif
} // namespace

void SetSvgRasterizerUserScale(float32 scale)
{
    const float32 clamped = std::clamp(
        scale, static_cast<float32>(kMinSvgRasterSize), static_cast<float32>(kMaxSvgRasterSize));
    s_UiDefaultSize.store(clamped, std::memory_order_relaxed);
}

float32 GetSvgRasterizerUserScale()
{
    return s_UiDefaultSize.load(std::memory_order_relaxed);
}

void SetSvgTextureRasterizerDefaultSize(float32 targetPixels)
{
    const float32 clamped = std::clamp(
        targetPixels, static_cast<float32>(kMinSvgRasterSize), static_cast<float32>(kMaxSvgRasterSize));
    s_TextureDefaultSize.store(clamped, std::memory_order_relaxed);
}

float32 GetSvgTextureRasterizerDefaultSize()
{
    return s_TextureDefaultSize.load(std::memory_order_relaxed);
}

bool ParseSvgRasterSizeMeta(const std::string& value, uint32& outTargetPixels)
{
    if (value.empty())
        return false;

    uint32 parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;

    outTargetPixels = std::clamp(parsed, kMinSvgRasterSize, kMaxSvgRasterSize);
    return true;
}

bool GetSvgSourceSize(const std::string& svgUtf8,
                      float32& outWidth,
                      float32& outHeight)
{
#if !defined(GE_HAVE_THORVG)
    (void)svgUtf8;
    (void)outWidth;
    (void)outHeight;
    return false;
#else
    if (svgUtf8.empty() || !EnsureThorvgInitialized())
        return false;

    ThorvgPicturePtr picture = MakeThorvgPicture();
    if (!picture)
        return false;

    const tvg::Result loadResult =
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", nullptr, true);
#else
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", true);
#endif
    if (loadResult != tvg::Result::Success)
        return false;

    float32 width = 0.0f;
    float32 height = 0.0f;
    picture->size(&width, &height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
        return false;

    outWidth = width;
    outHeight = height;
    return true;
#endif
}

bool CalculateSvgRasterDimensions(float32 sourceWidth,
                                  float32 sourceHeight,
                                  float32 targetPixels,
                                  uint32& outWidth,
                                  uint32& outHeight)
{
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) ||
        !std::isfinite(targetPixels) || sourceWidth <= 0.0f ||
        sourceHeight <= 0.0f || targetPixels <= 0.0f)
    {
        return false;
    }

    // Explicit callers such as platform icon creation may legitimately request
    // a sub-16-pixel target. Per-asset/editor settings clamp to 16 before here.
    const float32 targetPx = std::clamp(
        targetPixels, 1.0f, static_cast<float32>(kMaxSvgRasterSize));
    const float32 effectiveScale = targetPx / std::max(sourceWidth, sourceHeight);

    outWidth = static_cast<uint32>(std::max(0.0f, std::round(sourceWidth * effectiveScale)));
    outHeight = static_cast<uint32>(std::max(0.0f, std::round(sourceHeight * effectiveScale)));
    outWidth = std::clamp<uint32>(outWidth, 1u, kMaxSvgRasterSize);
    outHeight = std::clamp<uint32>(outHeight, 1u, kMaxSvgRasterSize);
    return true;
}

bool RasterizeSvgToRgbaAtSize(const std::string& svgUtf8,
                              float32 targetPixels,
                              SvgRasterizedImage& outImage)
{
#if !defined(GE_HAVE_THORVG)
    Logger::Log::Warning("RasterizeSvgToRgba: ThorVG not available at build time; SVGs will fall back to dummy textures");
    (void)svgUtf8;
    (void)targetPixels;
    outImage.Width = 0;
    outImage.Height = 0;
    outImage.Data.reset();
    outImage.DataSize = 0;
    return false;
#else
    if (svgUtf8.empty())
    {
        Logger::Log::Error("RasterizeSvgToRgba: empty SVG string");
        return false;
    }
    if (!EnsureThorvgInitialized())
    {
        return false;
    }
    ThorvgPicturePtr picture = MakeThorvgPicture();
    if (!picture)
    {
        Logger::Log::Error("RasterizeSvgToRgba: failed to create ThorVG picture");
        return false;
    }

    const tvg::Result loadResult =
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", nullptr, true);
#else
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", true);
#endif
    if (loadResult != tvg::Result::Success)
    {
        Logger::Log::Error("RasterizeSvgToRgba: ThorVG failed to load SVG, error code {}", static_cast<int>(loadResult));
        return false;
    }

    float fw = 0.0f;
    float fh = 0.0f;
    picture->size(&fw, &fh);
    if (fw <= 0.0f || fh <= 0.0f)
    {
        Logger::Log::Error("RasterizeSvgToRgba: SVG reported non-positive size {}x{}", fw, fh);
        return false;
    }

    uint32 width = 0;
    uint32 height = 0;
    if (!CalculateSvgRasterDimensions(fw, fh, targetPixels, width, height))
    {
        Logger::Log::Error(
            "RasterizeSvgToRgba: invalid source {}x{} or target size {}", fw, fh, targetPixels);
        return false;
    }

    picture->size(static_cast<float>(width), static_cast<float>(height));

    ThorvgCanvasPtr canvas = MakeThorvgCanvas();
    if (!canvas)
    {
        Logger::Log::Error("RasterizeSvgToRgba: failed to create ThorVG SwCanvas");
        return false;
    }

    const size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    auto buffer = std::make_unique<uint8[]>(bufferSize);

    const tvg::Result targetResult =
        canvas->target(reinterpret_cast<uint32_t*>(buffer.get()),
                       width,
                       width,
                       height,
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
                       tvg::ColorSpace::ABGR8888S);
#else
                       tvg::SwCanvas::ABGR8888S);
#endif
    if (targetResult != tvg::Result::Success)
    {
        Logger::Log::Error("RasterizeSvgToRgba: ThorVG failed to set canvas target, error {}", static_cast<int>(targetResult));
        return false;
    }

#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
    const tvg::Result pushResult = canvas->add(picture.get());
    if (pushResult == tvg::Result::Success)
    {
        picture.release();
    }
#else
    const tvg::Result pushResult = canvas->push(std::move(picture));
#endif
    if (pushResult != tvg::Result::Success)
    {
        Logger::Log::Error("RasterizeSvgToRgba: ThorVG failed to add picture, error {}", static_cast<int>(pushResult));
        return false;
    }

    const tvg::Result drawResult = canvas->draw();
    if (drawResult != tvg::Result::Success)
    {
        Logger::Log::Error("RasterizeSvgToRgba: ThorVG draw failed, error {}", static_cast<int>(drawResult));
        return false;
    }

    const tvg::Result syncResult = canvas->sync();
    if (syncResult != tvg::Result::Success)
    {
        Logger::Log::Error("RasterizeSvgToRgba: ThorVG sync failed, error {}", static_cast<int>(syncResult));
        return false;
    }

    outImage.Width = width;
    outImage.Height = height;
    outImage.Data = std::move(buffer);
    outImage.DataSize = bufferSize;
    return true;
#endif
}

bool RasterizeSvgToRgba(const std::string& svgUtf8,
                        float32 scale,
                        SvgRasterizedImage& outImage)
{
    const float32 baseScale = (scale <= 0.0f) ? 1.0f : scale;
    return RasterizeSvgToRgbaAtSize(
        svgUtf8, GetSvgRasterizerUserScale() * baseScale, outImage);
}

} // namespace GameEngine
