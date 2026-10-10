#include "Video/VideoPlayer.h"

namespace GameEngine::Video
{

struct VideoPlayer::Impl {};

VideoPlayer::VideoPlayer()  : m_Impl(std::make_unique<Impl>()) {}
VideoPlayer::~VideoPlayer() = default;

bool  VideoPlayer::Load(const std::string&) { return false; }
void  VideoPlayer::Unload() {}
bool  VideoPlayer::IsLoaded() const { return false; }
void  VideoPlayer::Play() {}
void  VideoPlayer::Pause() {}
void  VideoPlayer::Stop() {}
bool  VideoPlayer::IsPlaying() const { return false; }
void  VideoPlayer::SetLoop(bool) {}
bool  VideoPlayer::IsLooping() const { return false; }
void  VideoPlayer::SetPlaybackSpeed(float) {}
float VideoPlayer::GetPlaybackSpeed() const { return 1.0f; }
float VideoPlayer::GetDuration() const { return 0.0f; }
float VideoPlayer::GetCurrentTime() const { return 0.0f; }
void  VideoPlayer::Seek(float) {}
int   VideoPlayer::GetWidth() const { return 0; }
int   VideoPlayer::GetHeight() const { return 0; }
VideoPixelFormat VideoPlayer::GetFramePixelFormat() const { return VideoPixelFormat::RGBA8; }
bool  VideoPlayer::PollNewFrame() { return false; }
bool  VideoPlayer::WaitForFrame(float) { return false; }
void  VideoPlayer::GetFramePixels(uint8_t*) {}
const uint8_t* VideoPlayer::AcquireFramePointer(size_t* outSizeBytes) { if (outSizeBytes) *outSizeBytes = 0; return nullptr; }
void  VideoPlayer::GetFrameRGBA(uint8_t*) {}
uint64_t VideoPlayer::GetPublishedFrameCount() const { return 0; }
uint64_t VideoPlayer::GetAcquiredFrameCount() const { return 0; }

} // namespace GameEngine::Video
