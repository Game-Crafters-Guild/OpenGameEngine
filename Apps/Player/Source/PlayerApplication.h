#pragma once

#include "Core/Application.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "UI/UITextureSpace.h"
#if GE_PLAYER_MOVIE_RECORDER
#include "Video/AsyncVideoRecorder.h"
#endif

#include <chrono>
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace GameEngine {

namespace ECS { class World; }
namespace Rendering::RenderGraph { struct RGResourceDesc; }
class RuntimeFrameReadback;
class RuntimeHost;
struct RuntimeHostDesc;

class PlayerApplication : public Application
{
public:
    PlayerApplication(const ApplicationConfig& config, const GameConfig& gameConfig,
                      bool tickManagedSystems = false,
#if GE_PLAYER_MOVIE_RECORDER
                      std::optional<Video::VideoWriterOptions> movieOptions = std::nullopt,
                      uint64_t movieFrameLimit = 0,
#endif
                      bool allowRuntimeHdrToggle = false,
                      std::string screenshotPath = {});
    ~PlayerApplication() override;

    bool Initialize() override;
    void Shutdown() override;

protected:
    void Update(float64 deltaTime) override;
    void Render() override;
    void OnShutdown() override;

private:
    // Terminal device-loss handling for the Player (no in-app recovery UI): log, run a
    // save-state hook, and request a clean exit. One-shot per process.
    void HandleFatalDeviceLoss();
    bool m_FatalDeviceLossHandled = false;

    // The host's settings from game.config; the window size from the application
    // config, which the movie recorder may have clamped.
    RuntimeHostDesc MakeRuntimeHostDesc() const;
    // Once the startup scene's resolve batch completes: logs it and starts the
    // scene's autoplay animators. Each frame until then.
    void FinishStartupResolve(ECS::World& world);
    // The host's composite hook: the movie frame and the screenshot read the
    // composited frame back through it.
    void OnFrameComposited(RuntimeFrameReadback& readback);
    void DeclareScreenshot(RuntimeFrameReadback& readback);
    // Writes the PNG once the screenshot readback lands, then requests exit.
    void PollScreenshot();
    void RegisterManagedSystemBridge();
    void PollRuntimeDisplayControls();
    bool ToggleRuntimeHdrOutput();
    bool ApplyRuntimeHdrOutput(Rendering::HdrOutputMode mode);
#if GE_PLAYER_MOVIE_RECORDER
    void DeclareMovieFrame(RuntimeFrameReadback& readback);
    // Drains the readbacks that resolved and finishes the file. Call with the device idle.
    void FinishMovieCapture();
    void PollMovieFrames();
    Video::SubmitResult TrySubmitMovieFrame(Video::OwnedVideoFrame& frame);
    // The recorder's terminal failure text, or "encoder stopped" when none
    // exists. For the rejected-submit log lines.
    std::string MovieEncoderFailureReason();
    // Capture always records at the source's extent. A requested --movie-width/
    // --movie-height that differs ends the run with an actionable error instead of
    // resampling an already-dithered frame. False means the capture is refused.
    bool AcceptMovieSourceExtent(const Rendering::RenderGraph::RGResourceDesc& sourceDesc);
    bool EnsureMovieRecorder(uint32_t width, uint32_t height);
    bool DelayMovieCaptureFrame();
    void StartMovieAudioCapture();
    void StopMovieAudioCapture();
    void AppendMovieAudio(const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate);
    void EnqueueMovieAudio(const std::shared_ptr<Video::AsyncVideoRecorder>& recorder, Video::OwnedAudioSamples&& audio);
    void ClearPendingMovieAudio();
#endif

    GameConfig m_GameConfig;
    // True when C# GameSystems must tick each frame (NativeAOT library staged, or
    // CoreCLR with a loadable script assembly). See PlayerNeedsManagedSystemBridge.
    bool m_TickManagedSystems = false;
    Rendering::HdrOutputMode m_RuntimeHdrRequestedMode = Rendering::HdrOutputMode::Auto;
    Rendering::HdrSwapchainBitDepth m_RuntimeHdrBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
    bool m_AllowRuntimeHdrToggle = false;
#if GE_PLAYER_MOVIE_RECORDER
    std::optional<Video::VideoWriterOptions> m_MovieOptions;
    std::shared_ptr<Video::AsyncVideoRecorder> m_MovieRecorder;
    // Movie-frame readback tickets. The RenderGraph path captures the final
    // rendered output and drains these in order before submitting to the encoder.
    // Each entry carries the space its declaring pass produced, so the drain
    // converts under a stated space instead of one read off the readback format.
    struct MovieReadback
    {
        std::shared_ptr<Rendering::RGReadbackTicket> Ticket;
        UI::UITextureSpace Space; // no default: the declaration states it
    };
    std::deque<MovieReadback> m_MovieReadbacks;
    // A frame the encoder couldn't accept yet (queue full); resubmitted next poll
    // before draining new readbacks so frame order is preserved.
    std::optional<Video::OwnedVideoFrame> m_PendingMovieEncodeFrame;
    uint64_t m_MovieFrameLimit = 0;
    uint64_t m_MovieFramesScheduled = 0;
    uint64_t m_MovieFramesWritten = 0;
    uint32_t m_MovieWarmupFramesRemaining = 0;
    bool m_MovieAudioCaptureArmed = false;
#endif

    // --screenshot: read back the pipeline FinalColor once (after a short warmup so
    // shaders are compiled and the C++ OnStart spawn has run), write a PNG, and exit.
    // A headless way to see what the shipped Player actually renders (the swapchain
    // has no movie/screen-capture path on this branch).
    std::string m_ScreenshotPath;
    // The startup scene's resolve batch on the engine's resolve service, pending
    // until every entity and standalone material of the scene has bound or missed.
    Engine::Renderer::SceneResolveBatch m_StartupResolve = Engine::Renderer::SceneResolveBatch::None;
    bool m_StartupResolvePending = false;
    std::chrono::steady_clock::time_point m_StartupResolveStart{};
    uint32_t m_StartupResolveFrames = 0;
    bool m_ScreenshotWritten = false;
    uint32_t m_ScreenshotWarmupFrames = 0;
    std::shared_ptr<Rendering::RGReadbackTicket> m_ScreenshotTicket;
    // The pipeline's stamp for the pixels m_ScreenshotTicket is copying, taken at
    // declaration: the ticket can resolve frames later, and a mode flip in between
    // must not retroactively change what the captured bytes are claimed to be.
    // Engaged exactly while m_ScreenshotTicket is.
    std::optional<UI::UITextureSpace> m_ScreenshotSpace;
#if GE_PLAYER_MOVIE_RECORDER
    std::mutex m_MovieRecorderMutex;
    uint32_t m_MovieAudioChannels = 0;
    uint32_t m_MovieAudioSampleRate = 0;
    // Audio the encoder couldn't accept yet (queue full); drained in order on the
    // next audio callback before new samples, so backpressure buffers rather than
    // dropping. Guarded by its own mutex (touched on the audio-capture thread).
    std::deque<Video::OwnedAudioSamples> m_PendingMovieAudio;
    size_t m_PendingMovieAudioBytes = 0;
    std::mutex m_MovieAudioBufferMutex;
    std::vector<uint8_t> m_LastMovieFramePixels;
    uint32_t m_LastMovieFrameWidth = 0;
    uint32_t m_LastMovieFrameHeight = 0;
    uint32_t m_LastMovieFrameStrideBytes = 0;
    Video::VideoPixelFormat m_LastMovieFrameFormat = Video::VideoPixelFormat::RGBA8;
#endif
    // The window, the render device, the pipeline, the scene and the frame.
    std::unique_ptr<RuntimeHost> m_Host;
    float m_LastRenderDeltaTime = 1.0f / 60.0f;
};

} // namespace GameEngine
