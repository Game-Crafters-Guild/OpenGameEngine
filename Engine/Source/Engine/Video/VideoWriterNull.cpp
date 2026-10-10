#include "Video/VideoWriter.h"

namespace GameEngine::Video
{

struct VideoWriter::Impl
{
    std::string lastError = "Video writing is not available on this platform/build";
};

VideoWriter::VideoWriter()
    : m_Impl(std::make_unique<Impl>())
{
}

VideoWriter::~VideoWriter() = default;

bool VideoWriter::Open(const VideoWriterOptions&)
{
    m_Impl->lastError = "Video writing is not available on this platform/build";
    return false;
}

bool VideoWriter::WriteFrame(const VideoFrameView&)
{
    return false;
}

bool VideoWriter::WriteAudio(const AudioFrameView&)
{
    return false;
}

bool VideoWriter::ExtendToTimestamp(double)
{
    return true;
}

bool VideoWriter::WriteEndFade(const VideoFrameView&)
{
    return false;
}

bool VideoWriter::Close()
{
    return true;
}

bool VideoWriter::IsOpen() const
{
    return false;
}

const std::string& VideoWriter::GetLastError() const
{
    return m_Impl->lastError;
}

bool VideoWriter::IsCodecSupported(VideoCodec)
{
    return false;
}

} // namespace GameEngine::Video
