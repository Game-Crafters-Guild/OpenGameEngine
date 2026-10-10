#include "Engine/Build/AppIconGenerator.h"

#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Core/Application.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

#if defined(GE_HAVE_STB)
#include <stb_image.h>
#include <stb_image_write.h>
#endif

namespace fs = std::filesystem;

namespace GameEngine {

namespace {

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#if defined(GE_HAVE_STB)

struct RgbaImage
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

static bool LoadRgbaImage(const fs::path& path, RgbaImage& out, std::string& error)
{
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data)
    {
        error = "Failed to decode icon image: " + path.string();
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    stbi_image_free(data);
    if (w <= 0 || h <= 0)
    {
        error = "Icon image has invalid dimensions";
        return false;
    }
    return true;
}

static RgbaImage ResizeRgbaBilinear(const RgbaImage& src, int dstW, int dstH)
{
    RgbaImage dst;
    dst.width = dstW;
    dst.height = dstH;
    dst.pixels.resize(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4u);
    if (src.width <= 0 || src.height <= 0 || dstW <= 0 || dstH <= 0)
        return dst;

    const auto sample = [&](float fx, float fy, int c) -> float {
        fx = std::clamp(fx, 0.0f, static_cast<float>(src.width - 1));
        fy = std::clamp(fy, 0.0f, static_cast<float>(src.height - 1));
        const int x0 = static_cast<int>(fx);
        const int y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, src.width - 1);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const auto at = [&](int x, int y) -> float {
            return static_cast<float>(src.pixels[(static_cast<size_t>(y) * static_cast<size_t>(src.width)
                                                    + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)]);
        };
        const float v00 = at(x0, y0);
        const float v10 = at(x1, y0);
        const float v01 = at(x0, y1);
        const float v11 = at(x1, y1);
        const float v0 = v00 + (v10 - v00) * tx;
        const float v1 = v01 + (v11 - v01) * tx;
        return v0 + (v1 - v0) * ty;
    };

    for (int y = 0; y < dstH; ++y)
    {
        const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src.height) / static_cast<float>(dstH) - 0.5f;
        for (int x = 0; x < dstW; ++x)
        {
            const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src.width) / static_cast<float>(dstW) - 0.5f;
            const size_t di = (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4u;
            for (int c = 0; c < 4; ++c)
                dst.pixels[di + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(sample(fx, fy, c), 0.0f, 255.0f));
        }
    }
    return dst;
}

static bool WritePngFile(const fs::path& path, const RgbaImage& image)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0;
}

static void PngWriteToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

static bool EncodePng(const RgbaImage& image, std::vector<uint8_t>& outPng)
{
    outPng.clear();
    return stbi_write_png_to_func(PngWriteToVector, &outPng, image.width, image.height, 4, image.pixels.data(),
                                  image.width * 4) != 0;
}

static bool WriteWindowsIco(const fs::path& icoPath, const RgbaImage& source, std::string& error)
{
    static constexpr int kSizes[] = {16, 32, 48, 256};
    std::vector<std::vector<uint8_t>> pngBlobs;
    pngBlobs.reserve(std::size(kSizes));

    for (const int size : kSizes)
    {
        const RgbaImage resized = ResizeRgbaBilinear(source, size, size);
        std::vector<uint8_t> png;
        if (!EncodePng(resized, png) || png.empty())
        {
            error = "Failed to encode ICO PNG payload";
            return false;
        }
        pngBlobs.push_back(std::move(png));
    }

    std::ofstream out(icoPath, std::ios::binary);
    if (!out.is_open())
    {
        error = "Failed to write " + icoPath.string();
        return false;
    }

    const uint16_t reserved = 0;
    const uint16_t type = 1;
    const uint16_t count = static_cast<uint16_t>(pngBlobs.size());
    out.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
    out.write(reinterpret_cast<const char*>(&type), sizeof(type));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    const uint32_t headerSize = 6u + static_cast<uint32_t>(count) * 16u;
    uint32_t offset = headerSize;
    for (size_t i = 0; i < pngBlobs.size(); ++i)
    {
        const int size = kSizes[i];
        uint8_t width = size >= 256 ? 0 : static_cast<uint8_t>(size);
        uint8_t height = width;
        uint8_t colorCount = 0;
        uint8_t reservedByte = 0;
        uint16_t planes = 1;
        uint16_t bitCount = 32;
        const uint32_t bytesInRes = static_cast<uint32_t>(pngBlobs[i].size());
        out.write(reinterpret_cast<const char*>(&width), 1);
        out.write(reinterpret_cast<const char*>(&height), 1);
        out.write(reinterpret_cast<const char*>(&colorCount), 1);
        out.write(reinterpret_cast<const char*>(&reservedByte), 1);
        out.write(reinterpret_cast<const char*>(&planes), sizeof(planes));
        out.write(reinterpret_cast<const char*>(&bitCount), sizeof(bitCount));
        out.write(reinterpret_cast<const char*>(&bytesInRes), sizeof(bytesInRes));
        out.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
        offset += bytesInRes;
    }
    for (const auto& blob : pngBlobs)
        out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));

    return out.good();
}

static bool WriteWindowsResourceScript(const fs::path& rcPath)
{
    std::ofstream out(rcPath);
    if (!out.is_open())
        return false;
    out << "// Auto-generated by GameEngine build pipeline\n";
    out << "#include <windows.h>\n\n";
    out << "1 ICON \"AppIcon.ico\"\n";
    return out.good();
}

struct MacIconsetEntry
{
    const char* fileName;
    int pixelSize;
};

static bool WriteMacIconset(const fs::path& iconsetDir, const RgbaImage& source, std::string& error)
{
    static constexpr MacIconsetEntry kEntries[] = {
        {"icon_16x16.png", 16},
        {"icon_16x16@2x.png", 32},
        {"icon_32x32.png", 32},
        {"icon_32x32@2x.png", 64},
        {"icon_128x128.png", 128},
        {"icon_128x128@2x.png", 256},
        {"icon_256x256.png", 256},
        {"icon_256x256@2x.png", 512},
        {"icon_512x512.png", 512},
        {"icon_512x512@2x.png", 1024},
    };

    std::error_code ec;
    fs::create_directories(iconsetDir, ec);
    for (const MacIconsetEntry& entry : kEntries)
    {
        const RgbaImage resized = ResizeRgbaBilinear(source, entry.pixelSize, entry.pixelSize);
        if (!WritePngFile(iconsetDir / entry.fileName, resized))
        {
            error = "Failed to write macOS iconset PNG";
            return false;
        }
    }
    return true;
}

static bool ConvertIconsetToIcns([[maybe_unused]] const fs::path& iconsetDir, const fs::path& icnsPath, std::vector<std::string>& warnings,
                                 std::string& error)
{
    std::error_code ec;
    if (fs::exists(icnsPath, ec))
        fs::remove(icnsPath, ec);

#if defined(__APPLE__)
    const ShellProcessResult iconResult = RunProcessCaptured(
        "iconutil", {"-c", "icns", iconsetDir.string(), "-o", icnsPath.string()});
    if (iconResult.exitCode != 0)
    {
        error = "iconutil failed (exit " + std::to_string(iconResult.exitCode) + "): " + iconResult.output;
        return false;
    }
    return fs::exists(icnsPath, ec);
#else
    (void)warnings;
    error = "macOS .icns generation requires building on macOS (iconutil not available)";
    return false;
#endif
}

static bool WriteLinuxIconTree(const fs::path& linuxRoot, const std::string& iconName, const RgbaImage& source,
                               std::string& error)
{
    static constexpr int kSizes[] = {16, 32, 48, 64, 128, 256, 512};
    for (const int size : kSizes)
    {
        const RgbaImage resized = ResizeRgbaBilinear(source, size, size);
        const fs::path pngPath =
            linuxRoot / ("hicolor/" + std::to_string(size) + "x" + std::to_string(size) + "/apps/" + iconName + ".png");
        if (!WritePngFile(pngPath, resized))
        {
            error = "Failed to write Linux icon PNG";
            return false;
        }
    }
    return true;
}

#endif // GE_HAVE_STB

static std::string BuildCmakeIconSection()
{
    return R"(set(_GE_ICON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/icons")
if(WIN32 AND EXISTS "${_GE_ICON_ROOT}/Windows/AppIcon.rc")
    target_sources(Player PRIVATE "${_GE_ICON_ROOT}/Windows/AppIcon.rc")
endif()
if(APPLE AND EXISTS "${_GE_ICON_ROOT}/Mac/AppIcon.icns")
    set_target_properties(Player PROPERTIES MACOSX_BUNDLE_ICON_FILE AppIcon.icns)
    set(_GE_PLAYER_ICNS "${_GE_ICON_ROOT}/Mac/AppIcon.icns")
    target_sources(Player PRIVATE "${_GE_PLAYER_ICNS}")
    set_source_files_properties("${_GE_PLAYER_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
)";
}

enum class IconBundleKind
{
    Windows,
    Mac,
    Linux
};

static IconBundleKind IconKindForPlatform(const std::string& platformName)
{
    const std::string plat = ToLowerAscii(platformName);
    if (plat == "windows")
        return IconBundleKind::Windows;
    if (plat == "mac")
        return IconBundleKind::Mac;
    return IconBundleKind::Linux;
}

static constexpr int kMinDefaultIconDimensionPx = 128;

#if defined(GE_HAVE_STB)
static int MaxIconDimension(const fs::path& imagePath)
{
    int width = 0;
    int height = 0;
    int components = 0;
    if (!stbi_info(imagePath.string().c_str(), &width, &height, &components))
        return 0;
    return std::max(width, height);
}

static bool IconMeetsPreviewQuality(const fs::path& imagePath)
{
    return MaxIconDimension(imagePath) >= kMinDefaultIconDimensionPx;
}
#endif

#if defined(__APPLE__)
static fs::path ExportMacIcnsToCachedPng(const fs::path& icnsPath)
{
    std::error_code ec;
    if (!fs::is_regular_file(icnsPath, ec))
        return {};

    static fs::path s_CachedPath;
    static fs::file_time_type s_SourceWriteTime{};

    const fs::file_time_type sourceTime = fs::last_write_time(icnsPath, ec);
    if (!s_CachedPath.empty() && fs::is_regular_file(s_CachedPath, ec) && s_SourceWriteTime == sourceTime)
        return s_CachedPath;

    const fs::path outPath = fs::temp_directory_path() / "GameEngineEditorAppIconPreview.png";
    const ShellProcessResult sipsResult = RunProcessCaptured(
        "sips", {"-s", "format", "png", icnsPath.string(), "-z", "512", "512", "--out", outPath.string()});
    if (sipsResult.exitCode != 0 || !fs::is_regular_file(outPath, ec))
        return {};

    s_CachedPath = outPath;
    s_SourceWriteTime = sourceTime;
    return outPath;
}
#endif // __APPLE__

} // namespace

fs::path ResolveDefaultEditorApplicationIconPath(const fs::path& editorSdkPath, const fs::path& runtimeDepsPath)
{
    std::error_code ec;
    const auto tryFile = [&](const fs::path& path) -> fs::path {
        if (!path.empty() && fs::is_regular_file(path, ec))
            return path;
        return {};
    };

    fs::path sdkRoot = editorSdkPath;
    fs::path runtimeRoot = runtimeDepsPath;
    if (sdkRoot.empty() || runtimeRoot.empty())
    {
        const fs::path exeDir = PathUtils::GetExecutableDirectory();
        if (sdkRoot.empty())
            sdkRoot = exeDir / "SDK";
        if (runtimeRoot.empty())
            runtimeRoot = exeDir;
    }

#if defined(GE_HAVE_STB)
    const auto useIfQuality = [&](const fs::path& path) -> fs::path {
        const fs::path file = tryFile(path);
        if (!file.empty() && IconMeetsPreviewQuality(file))
            return file;
        return {};
    };

    if (auto icon = useIfQuality(sdkRoot / "AppIcon.png"); !icon.empty())
        return icon;
    if (auto icon = useIfQuality(runtimeRoot / "AppIcon.png"); !icon.empty())
        return icon;
#else
    if (auto icon = tryFile(sdkRoot / "AppIcon.png"); !icon.empty())
        return icon;
    if (auto icon = tryFile(runtimeRoot / "AppIcon.png"); !icon.empty())
        return icon;
#endif

#if defined(__APPLE__)
    const fs::path bundleIcns = runtimeRoot / ".." / "Resources" / "AppIcon.icns";
    if (auto png = ExportMacIcnsToCachedPng(bundleIcns); !png.empty())
        return png;
#endif

    if (auto icon = tryFile(sdkRoot / "AppIcon.png"); !icon.empty())
        return icon;
    if (auto icon = tryFile(runtimeRoot / "AppIcon.png"); !icon.empty())
        return icon;

    return {};
}

fs::path ResolveApplicationIconSourcePath(const BuildSettings& settings)
{
    std::error_code ec;

    if (!settings.applicationIconPath.empty())
    {
        const fs::path candidate(settings.applicationIconPath);
        if (candidate.is_absolute() && fs::is_regular_file(candidate, ec))
            return candidate;

        if (!settings.projectRoot.empty())
        {
            const fs::path fromProject = settings.projectRoot / candidate;
            if (fs::is_regular_file(fromProject, ec))
                return fromProject;
        }

        if (EngineCore::GetInstance().IsInitialized())
        {
            const fs::path fromAssets = EngineCore::GetInstance().GetResolvedAssetRoot() / candidate;
            if (fs::is_regular_file(fromAssets, ec))
                return fromAssets;
        }
    }

    return ResolveDefaultEditorApplicationIconPath(settings.editorSDKPath, settings.runtimeDepsPath);
}

AppIconGenerationResult GenerateApplicationIcons(const BuildSettings& settings, const fs::path& playerProjectDir)
{
    AppIconGenerationResult result;
    result.cmakeIconSection = "# No application icon configured\n";

    const fs::path sourcePath = ResolveApplicationIconSourcePath(settings);
    if (sourcePath.empty())
        return result;

#if !defined(GE_HAVE_STB)
    result.warnings.push_back("stb_image is unavailable; application icon generation was skipped");
    return result;
#else
    RgbaImage source{};
    if (!LoadRgbaImage(sourcePath, source, result.error))
    {
        result.success = false;
        return result;
    }

    const IconBundleKind kind = IconKindForPlatform(settings.platformName);
    const fs::path iconsRoot = playerProjectDir / "icons";
    std::error_code ec;

    switch (kind)
    {
        case IconBundleKind::Windows:
        {
            const fs::path winDir = iconsRoot / "Windows";
            fs::create_directories(winDir, ec);
            if (!WriteWindowsIco(winDir / "AppIcon.ico", source, result.error))
                return result;
            if (!WriteWindowsResourceScript(winDir / "AppIcon.rc"))
            {
                result.error = "Failed to write AppIcon.rc";
                return result;
            }
            break;
        }
        case IconBundleKind::Mac:
        {
            const fs::path macDir = iconsRoot / "Mac";
            const fs::path iconsetDir = macDir / "AppIcon.iconset";
            if (!WriteMacIconset(iconsetDir, source, result.error))
                return result;
            std::string icnsError;
            if (!ConvertIconsetToIcns(iconsetDir, macDir / "AppIcon.icns", result.warnings, icnsError))
            {
                result.warnings.push_back(icnsError);
                result.warnings.push_back(
                    "macOS .icns was not embedded; build the Mac target on macOS or copy AppIcon.icns manually.");
            }
            break;
        }
        case IconBundleKind::Linux:
        {
            std::string safeName = settings.playerConfig.gameName;
            for (char& c : safeName)
            {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                    c = '_';
            }
            if (safeName.empty())
                safeName = "player";
            const fs::path linuxDir = iconsRoot / "Linux";
            if (!WriteLinuxIconTree(linuxDir, ToLowerAscii(safeName), source, result.error))
                return result;
            break;
        }
    }

    result.cmakeIconSection = BuildCmakeIconSection();
    result.success = true;
    Logger::Log::Info("Build: Generated application icons for platform '{}' from '{}'",
                      settings.platformName, sourcePath.string());
    return result;
#endif
}

bool PackageLinuxIconsToStaging(const BuildSettings& settings, const fs::path& stagingDir)
{
#if !defined(GE_HAVE_STB)
    (void)settings;
    (void)stagingDir;
    return true;
#else
    const IconBundleKind kind = IconKindForPlatform(settings.platformName);
    if (kind != IconBundleKind::Linux)
        return true;

    const fs::path linuxIcons = settings.projectRoot / ".Build" / "Player" / "icons" / "Linux" / "hicolor";
    std::error_code ec;
    if (!fs::is_directory(linuxIcons, ec))
        return true;

    const fs::path dstRoot = stagingDir / "share" / "icons";
    for (const auto& entry : fs::recursive_directory_iterator(linuxIcons, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const fs::path rel = fs::relative(entry.path(), linuxIcons, ec);
        const fs::path dst = dstRoot / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
    }
    Logger::Log::Info("Build: Packaged Linux application icons into staging");
    return true;
#endif
}

} // namespace GameEngine
