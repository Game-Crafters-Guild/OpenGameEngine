#include "Assets/Textures/TextureMimeType.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace GameEngine {

std::string_view MimeFromTextureExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".bmp")  return "image/bmp";
    if (ext == ".tga")  return "image/x-tga";
    if (ext == ".hdr")  return "image/vnd.radiance";
    if (ext == ".tif" || ext == ".tiff") return "image/tiff";
    return "image/png";
}

std::string_view TextureExtensionFromMime(std::string_view mimeType)
{
    if (mimeType == "image/jpeg" || mimeType == "image/jpg") return ".jpg";
    if (mimeType == "image/x-tga" || mimeType == "image/tga") return ".tga";
    if (mimeType == "image/bmp") return ".bmp";
    if (mimeType == "image/vnd.radiance") return ".hdr";
    if (mimeType == "image/tiff") return ".tif";
    return ".png";
}

} // namespace GameEngine
