#include "Video/VideoWriter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace GameEngine::Video
{

namespace
{
std::string LowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
}

VideoCodec GuessCodecForMoviePath(const std::string& path)
{
    const std::string ext = LowerCopy(std::filesystem::path(path).extension().string());
    if (ext == ".hevc" || ext == ".h265")
        return VideoCodec::HEVC;
    if (ext == ".mov")
        return VideoCodec::ProRes422;
    return VideoCodec::H264;
}

VideoCodec ParseVideoCodecName(const std::string& name, VideoCodec fallback)
{
    const std::string n = LowerCopy(name);
    if (n == "auto")
        return VideoCodec::Auto;
    if (n == "h264" || n == "avc")
        return VideoCodec::H264;
    if (n == "h265" || n == "hevc")
        return VideoCodec::HEVC;
    if (n == "prores" || n == "prores422" || n == "prores_422")
        return VideoCodec::ProRes422;
    if (n == "prores4444" || n == "prores_4444")
        return VideoCodec::ProRes4444;
    return fallback;
}

const char* ToString(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::Auto: return "auto";
    case VideoCodec::H264: return "h264";
    case VideoCodec::HEVC: return "h265";
    case VideoCodec::ProRes422: return "prores422";
    case VideoCodec::ProRes4444: return "prores4444";
    }
    return "unknown";
}

} // namespace GameEngine::Video
