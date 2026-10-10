#pragma once

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Video/AsyncVideoRecorder.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{
enum class TextureFormat : uint32_t;
namespace RenderGraph
{
class RGFrame;
}
} // namespace Rendering

namespace Engine::Renderer
{
class RenderServices;
}

namespace Editor
{

// The GPU capture half of a movie recording inside the Game View: per-frame
// encode/readback declares, readback draining into the encoder, audio capture,
// backpressure and finalization. Owned by GameViewController. Session
// orchestration (what to record, play-mode arming, view events) lives in
// MovieRecorderController.
class GameViewMovieCapture
{
public:
    explicit GameViewMovieCapture(Engine::Renderer::RenderServices* renderServices);

    GameViewMovieCapture(const GameViewMovieCapture&) = delete;
    GameViewMovieCapture& operator=(const GameViewMovieCapture&) = delete;

    bool StartRecording(Video::VideoWriterOptions options, uint64_t frameLimit, std::string* outError = nullptr);
    void StopRecording(const std::string& message = {}, bool waitForFinalization = false);
    // Shutdown-time stop: the render loop is gone, so readbacks that have not
    // completed can never resolve. Drains what is ready, drops the rest, and
    // joins the encoder so the container always finalizes — never waits on the
    // GPU pipeline like StopRecording(…, true) does.
    void StopForShutdown();

    bool IsRecording() const { return m_MovieOptions.has_value(); }
    // Actively capturing new frames. False from the moment a stop is requested,
    // while IsRecording stays true until pending readbacks/audio finish
    // draining into the encoder — key UI state off this, not IsRecording.
    bool IsCapturing() const { return m_MovieOptions.has_value() && !m_MovieFinishRequested; }
    // Capturing an HDR arm, so this frame's encode carries a PQ/HLG override.
    // A view asks before finalizing itself: the encode refuses an already-encoded
    // source under an HDR override (FinalizeContract.h), so a finalized view
    // would cost the recording every frame, not just its own flip.
    bool IsCapturingHdr() const
    {
        return IsCapturing() && m_MovieOptions->hdrMode != Video::VideoHdrMode::Off;
    }
    uint32_t GetWidth() const { return m_MovieOptions ? m_MovieOptions->width : 0u; }
    uint32_t GetHeight() const { return m_MovieOptions ? m_MovieOptions->height : 0u; }
    double GetFps() const { return m_MovieOptions ? m_MovieOptions->fps : 0.0; }
    uint64_t GetFramesWritten() const { return m_MovieFramesWritten; }
    void SetStatusCallback(std::function<void(const std::string&)> callback) { m_MovieStatusCallback = std::move(callback); }

    // Drains finished readbacks into the encoder. The Game View's RenderGraph
    // declare calls this once per frame; RenderGraph frames have no other poll
    // site.
    void Poll();
    // Declares this frame's encode+readback passes for `source` (the texture
    // the Game View presents). Applies capture backpressure so the recording
    // stays gapless when the encoder falls behind. No-op unless capturing.
    //
    // `sourceSpace` states what `source` holds: Linear leaves this encode
    // owning the OETF, EncodedSrgb means the view's own finalize already
    // applied it and this transfer only moves — and where the target's step is
    // coarser, requantizes — the code values.
    //
    // `presentedFormat` is the step an encoded source's code values were
    // quantized to — the SAME value the view's finalize consumed this frame
    // (one resolution per frame, threaded from the window owner), so the
    // movie's requantize decision and the screen's step are one dataflow
    // instead of two parallel device reads.
    void DeclareCaptureRG(Rendering::RenderGraph::RGFrame& frame,
                          Rendering::RenderGraph::RGTexture source,
                          uint32_t width, uint32_t height,
                          Rendering::Passes::FinalizeInputSpace sourceSpace, float debandLsb,
                          Rendering::TextureFormat presentedFormat);

private:
    Video::SubmitResult TrySubmitMovieFrame(Video::OwnedVideoFrame& frame);
    // The recorder's terminal failure text, or the generic encoder-stopped
    // message when none exists. For the rejected-submit paths.
    std::string MovieFailureMessage();
    bool EnsureMovieRecorder(uint32_t width, uint32_t height);
    // By value: callers pass m_MovieFinishMessage, which this function clears.
    void FinishMovieRecording(std::string message);
    bool DelayMovieCaptureFrame();
    bool HasMovieBackpressure() const;
    bool WaitForMovieBackpressure();
    // Block on the oldest pending movie readback's own submission until
    // `deadline`. False when there is nothing to wait on, the head can never
    // resolve, or the deadline passed — the caller then sleeps and re-polls.
    bool WaitForOldestMovieReadback(std::chrono::steady_clock::time_point deadline);
    void StartMovieAudioCapture();
    void StopMovieAudioCapture();
    void AppendMovieAudio(const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate);
    void EnqueueMovieAudio(const std::shared_ptr<Video::AsyncVideoRecorder>& recorder, Video::OwnedAudioSamples&& audio);
    void ClearPendingMovieAudio();
    bool DrainPendingMovieAudio();
    // The first requested message wins; a later request only fills an empty one.
    void RequestMovieFinish(std::string message);
    bool HasPendingMovieVideoFrames() const;
    void WaitForPendingMovieData();

    Engine::Renderer::RenderServices* m_RenderServices = nullptr; // non-owning

    std::optional<Video::VideoWriterOptions> m_MovieOptions;
    std::shared_ptr<Video::AsyncVideoRecorder> m_MovieRecorder;
    // Strict encoder order: tickets drain in submission order.
    struct PendingMovieFrame
    {
        std::shared_ptr<Rendering::RGReadbackTicket> Rg; // RenderGraph entries
    };
    std::deque<PendingMovieFrame> m_MovieReadbacks;
    // A frame the encoder couldn't accept yet (queue full); resubmitted next poll
    // before draining new readbacks so frame order is preserved.
    std::optional<Video::OwnedVideoFrame> m_PendingMovieEncodeFrame;
    uint64_t m_MovieFrameLimit = 0;
    uint64_t m_MovieFramesScheduled = 0;
    uint64_t m_MovieFramesWritten = 0;
    uint32_t m_MovieWarmupFramesRemaining = 0;
    bool m_MovieAudioCaptureArmed = false;
    bool m_MovieQueueExhausted = false;
    bool m_MovieFinishRequested = false;
    bool m_MovieFinishWaitRequested = false;
    std::string m_MovieFinishMessage;
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
    std::function<void(const std::string&)> m_MovieStatusCallback;
};

} // namespace Editor
} // namespace GameEngine
