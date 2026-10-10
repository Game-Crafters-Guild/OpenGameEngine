#include "MenuIconBitmap_Win32.h"

#if defined(_WIN32)

#include "Platform/Shell.h"

#include "Assets/SvgRasterizer.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace GameEngine {
namespace Platform {
namespace {

constexpr std::string_view kEditorAliasColon = "editor:";
constexpr std::string_view kEditorAliasSlash = "@editor/";

// RGBA8 with straight (non-premultiplied) alpha — what both decoders hand back.
struct SourceImage
{
    std::vector<uint8_t> Pixels;
    uint32_t Width = 0;
    uint32_t Height = 0;
};

// The editor mount: assets staged next to the executable, the same tree the
// running editor reads. Never resolved back into the repo — a shipped exe has
// no repo next to it.
std::filesystem::path EditorAssetRoot()
{
    const std::filesystem::path exe = GetExecutablePath();
    if (exe.empty())
        return {};
    return exe.parent_path() / "Assets";
}

// Alias prefixes are the CSS spellings, so `editor:` and `@editor/` mean the
// editor mount. A drive letter is not an alias, and an alias this layer cannot
// resolve (`project:`, whose root only the AssetManager knows) yields nothing
// rather than being mistaken for a relative path.
std::filesystem::path ResolveImagePath(const std::string& imagePath)
{
    if (imagePath.empty())
        return {};

    std::string_view rest(imagePath);
    if (rest.starts_with(kEditorAliasColon))
        rest.remove_prefix(kEditorAliasColon.size());
    else if (rest.starts_with(kEditorAliasSlash))
        rest.remove_prefix(kEditorAliasSlash.size());
    else
    {
        const std::filesystem::path given(rest);
        if (given.is_absolute())
            return given;
        const size_t colon = rest.find(':');
        const size_t slash = rest.find_first_of("/\\");
        if (colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash))
            return {};
    }

    if (rest.empty())
        return {};
    const std::filesystem::path root = EditorAssetRoot();
    if (root.empty())
        return {};
    return root / std::filesystem::path(rest);
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool IsSvgPath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".svg";
}

bool DecodeSourceImage(const std::filesystem::path& path, int targetPixels, SourceImage& out)
{
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty())
        return false;

    if (IsSvgPath(path))
    {
        SvgRasterizedImage raster;
        if (!RasterizeSvgToRgbaAtSize(std::string(bytes.begin(), bytes.end()),
                                      static_cast<float32>(targetPixels), raster))
            return false;
        if (!raster.Data || raster.Width == 0 || raster.Height == 0)
            return false;
        out.Width = raster.Width;
        out.Height = raster.Height;
        out.Pixels.assign(raster.Data.get(), raster.Data.get() + raster.DataSize);
        return out.Pixels.size() >= static_cast<size_t>(out.Width) * out.Height * 4u;
    }

    DecodedImage decoded = DecodeImageToRGBA(bytes.data(), bytes.size());
    if (!decoded.valid || decoded.width == 0 || decoded.height == 0)
        return false;
    out.Width = decoded.width;
    out.Height = decoded.height;
    out.Pixels = std::move(decoded.pixels);
    return out.Pixels.size() >= static_cast<size_t>(out.Width) * out.Height * 4u;
}

// Box-average each destination pixel over the source rectangle it covers.
// Averaging happens in premultiplied space: filtering straight alpha would let
// fully transparent pixels contribute their colour and fringe the edges.
void ResampleToPremultipliedBgra(const SourceImage& src, uint8_t* dst, int dstWidth, int dstHeight)
{
    for (int y = 0; y < dstHeight; ++y)
    {
        const uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * src.Height / dstHeight);
        const uint32_t sy1 = std::max(sy0 + 1,
            static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * src.Height / dstHeight));
        for (int x = 0; x < dstWidth; ++x)
        {
            const uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * src.Width / dstWidth);
            const uint32_t sx1 = std::max(sx0 + 1,
                static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * src.Width / dstWidth));

            uint32_t r = 0, g = 0, b = 0, a = 0, count = 0;
            for (uint32_t sy = sy0; sy < sy1 && sy < src.Height; ++sy)
            {
                for (uint32_t sx = sx0; sx < sx1 && sx < src.Width; ++sx)
                {
                    const uint8_t* p = src.Pixels.data() + (static_cast<size_t>(sy) * src.Width + sx) * 4u;
                    const uint32_t alpha = p[3];
                    r += p[0] * alpha / 255u;
                    g += p[1] * alpha / 255u;
                    b += p[2] * alpha / 255u;
                    a += alpha;
                    ++count;
                }
            }

            uint8_t* out = dst + (static_cast<size_t>(y) * dstWidth + x) * 4u;
            if (count == 0)
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }
            out[0] = static_cast<uint8_t>(b / count);
            out[1] = static_cast<uint8_t>(g / count);
            out[2] = static_cast<uint8_t>(r / count);
            out[3] = static_cast<uint8_t>(a / count);
        }
    }
}

} // namespace

int MenuIconPixelSize()
{
    const int size = GetSystemMetrics(SM_CXSMICON);
    return size > 0 ? size : 16;
}

HBITMAP CreateMenuIconBitmap(const std::string& imagePath, int width, int height)
{
    if (width <= 0 || height <= 0)
        return nullptr;

    const std::filesystem::path resolved = ResolveImagePath(imagePath);
    if (resolved.empty())
        return nullptr;

    SourceImage source;
    if (!DecodeSourceImage(resolved, std::max(width, height), source))
        return nullptr;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down: row 0 is the top row
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        return nullptr;
    }

    ResampleToPremultipliedBgra(source, static_cast<uint8_t*>(bits), width, height);
    return bitmap;
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
