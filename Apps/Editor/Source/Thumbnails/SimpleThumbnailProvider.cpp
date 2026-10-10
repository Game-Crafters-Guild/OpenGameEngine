#include "Thumbnails/SimpleThumbnailProvider.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <cctype>

namespace GameEngine {

bool SimpleThumbnailProvider::IsImageExtension(const std::filesystem::path& p) {
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif" || ext == ".webp" || ext == ".svg" || ext == ".hdr";
}

// Treat common 3D formats as "model" assets so we can provide a dedicated
// model thumbnail icon without needing full AssetType wiring in this simple
// provider.
static bool IsModelExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".dae" || ext == ".3ds";
}

static bool IsAudioExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".aac";
}

static bool IsVideoExtension(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mkv" || ext == ".m4v";
}

std::string SimpleThumbnailProvider::GetOrRequest(const std::filesystem::path& assetPath,
                                                  int desiredSize,
                                                  std::function<void(const std::string& relPath)> onReady,
                                                  bool StaticModelListThumbnail) {
    (void)StaticModelListThumbnail;
    // Build cache key
    std::string key = assetPath.string() + "|" + std::to_string(desiredSize);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Cache.find(key);
        if (it != m_Cache.end()) {
            // Already cached; return immediately and also notify asynchronously
            auto rel = it->second;
            if (onReady) {
                onReady(rel);
            }
            return rel;
        }
    }

    // For now, if the asset is an image, use the original file as the thumbnail (UI scales it).
    // Later we can generate downscaled images into Assets/Generated/Thumbnails.
    std::error_code ec;
    std::string relStr;

    const bool isImage = IsImageExtension(assetPath);
    const bool isModel = IsModelExtension(assetPath);
    const bool isAudio = IsAudioExtension(assetPath);
    const bool isVideo = IsVideoExtension(assetPath);
    if (isImage && !m_AssetsRoot.empty()) {
        auto rel = std::filesystem::relative(assetPath, m_AssetsRoot, ec);
        if (!ec && !rel.empty()) {
            relStr = rel.generic_string();
        }
    }

    // Fallbacks for image assets when relative() fails or assetsRoot is
    // empty. The goal is to avoid silently returning an empty string
    // for images that live under Assets/ but happen to use slightly
    // different path forms (absolute vs. relative, symlinks, etc.).
    if (isImage && relStr.empty()) {
        std::error_code ecAbs;
        auto rootAbs = std::filesystem::absolute(m_AssetsRoot, ecAbs);
        auto assetAbs = std::filesystem::absolute(assetPath, ecAbs);
        if (!ecAbs) {
            auto rootStr = rootAbs.generic_string();
            auto assetStr = assetAbs.generic_string();
            if (assetStr.rfind(rootStr, 0) == 0) {
                auto trimmed = assetStr.substr(rootStr.size());
                if (!trimmed.empty() && (trimmed[0] == '/' || trimmed[0] == '\\')) {
                    trimmed.erase(trimmed.begin());
                }
                relStr = trimmed;
            }
        }

        // As a last resort, if the asset path is already relative,
        // use it as-is. This still keeps the contract of returning a
        // path relative to the Assets/ root from the editor's point
        // of view.
        if (relStr.empty() && assetPath.is_relative()) {
            relStr = assetPath.generic_string();
        }

        if (relStr.empty()) {
            Logger::Log::Warning("SimpleThumbnailProvider: failed to compute relative path for image '{}' (assetsRoot='{}')",
                                 assetPath.string(), m_AssetsRoot.string());
        }
    }

    // Non-image assets: provide a sensible default for common model types so that
    // FBX/OBJ/GLTF/etc. show a model icon instead of an empty background.
    // The returned path is relative to the Assets/ root, matching the
    // IThumbnailProvider contract. The actual icon file is expected at:
    //   Assets/Icons/ModelThumbnail@64px.png
    if (!isImage && isModel && relStr.empty())
    {
        relStr = "Icons/ModelThumbnail@64px.png";
    }

    // Audio assets: use music note icon.
    if (!isImage && isAudio && relStr.empty())
    {
        relStr = "Icons/music-note-icon@64px.png";
    }

    // Video assets: use film/play icon.
    if (!isImage && isVideo && relStr.empty())
    {
        relStr = "Icons/videocam.png";
    }

    // Cache result (may be empty if not supported)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Cache[key] = relStr;
    }

    // Notify asynchronously
    if (onReady) {
        onReady(relStr);
    }

    return relStr;
}

} // namespace GameEngine
