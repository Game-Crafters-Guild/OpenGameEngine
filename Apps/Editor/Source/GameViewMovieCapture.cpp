#include "GameViewMovieCapture.h"

#include "Audio/AudioSystem.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"
#include "Video/VideoWriter.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace GameEngine::Editor
{

namespace
{
// Readback completion can lag several submitted frames under Metal/Vulkan,
// especially while the editor UI is active. Once this fills, movie capture
// applies backpressure to the frame loop instead of skipping the current frame.
constexpr size_t kMaxPendingMovieReadbacks = 16;
constexpr auto kMovieBackpressureTimeout = std::chrono::seconds(60);
constexpr uint32_t kMovieStartupWarmupFrames = 4;
// ~20s of 48 kHz stereo float audio: enough to ride out an encoder stall without
// unbounded growth. Past this the oldest buffered audio is dropped.
constexpr size_t kMaxPendingMovieAudioBytes = 8 * 1024 * 1024;

template <typename T>
void ReleaseVectorMemory(std::vector<T>& values)
{
    std::vector<T>().swap(values);
}

template <typename T>
void ReleaseDequeMemory(std::deque<T>& values)
{
    std::deque<T>().swap(values);
}

} // namespace

using GameEngine::Rendering::TextureFormat;

GameViewMovieCapture::GameViewMovieCapture(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
}

bool GameViewMovieCapture::StartRecording(Video::VideoWriterOptions options, uint64_t frameLimit, std::string* outError)
{
    if (options.path.empty())
    {
        if (outError)
        {
            *outError = "Choose an output path before recording.";
        }
        return false;
    }
    if (options.width == 0 || options.height == 0)
    {
        if (outError)
        {
            *outError = "Choose a valid recording resolution.";
        }
        return false;
    }
    if (options.fps <= 0.0)
    {
        if (outError)
        {
            *outError = "Choose a valid frame rate.";
        }
        return false;
    }
    if (options.codec == Video::VideoCodec::Auto)
    {
        options.codec = Video::GuessCodecForMoviePath(options.path);
    }
    if (!Video::VideoWriter::IsCodecSupported(options.codec))
    {
        if (outError)
        {
            *outError = std::string("Codec is not supported: ") + Video::ToString(options.codec);
        }
        return false;
    }

    StopRecording({}, true);

    m_MovieOptions = options;
    m_MovieFrameLimit = frameLimit;
    m_MovieFramesScheduled = 0;
    m_MovieFramesWritten = 0;
    m_MovieWarmupFramesRemaining = kMovieStartupWarmupFrames;
    m_MovieAudioCaptureArmed = false;
    m_PendingMovieEncodeFrame.reset();
    m_MovieFinishRequested = false;
    m_MovieFinishWaitRequested = false;
    m_MovieFinishMessage.clear();
    ClearPendingMovieAudio();
    ReleaseDequeMemory(m_MovieReadbacks);
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        m_MovieRecorder = std::make_shared<Video::AsyncVideoRecorder>();
        m_MovieQueueExhausted = false;

        Video::AsyncVideoRecorderOptions recorderOpts;
        recorderOpts.OnQueueExhausted = [this]() {
            m_MovieQueueExhausted = true;
            Logger::Log::Warning("GameView: movie encoder queue exhausted; applying recording backpressure");
        };

        std::string error;
        if (!m_MovieRecorder->Start(options, recorderOpts, &error))
        {
            m_MovieRecorder.reset();
            if (outError)
            {
                *outError = "Movie capture failed: " + error;
            }
            return false;
        }
    }
    ReleaseVectorMemory(m_LastMovieFramePixels);
    m_LastMovieFrameWidth = 0;
    m_LastMovieFrameHeight = 0;
    m_LastMovieFrameStrideBytes = 0;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        m_MovieAudioChannels = options.audioChannels;
        m_MovieAudioSampleRate = options.audioSampleRate;
    }
    const bool bitrateCodec = options.codec == Video::VideoCodec::H264 || options.codec == Video::VideoCodec::HEVC;
    const std::string bitrateText = bitrateCodec
                                        ? (options.bitrateKbps > 0 ? std::to_string(options.bitrateKbps) + " Kbps" : "encoder-default")
                                        : "profile-driven";
    Logger::Log::Info(
        "GameView: started play-mode movie capture '{}' ({}x{} @ {} fps, codec={}, bitrate={}, audio={}, hardware={})",
        options.path,
        options.width,
        options.height,
        options.fps,
        Video::ToString(options.codec),
        bitrateText,
        options.recordAudio ? "on" : "off",
        options.requireHardwareAcceleration ? "required" : "allowed-software-fallback");
    return true;
}

void GameViewMovieCapture::StopRecording(const std::string& message, bool waitForFinalization)
{
    if (!m_MovieOptions)
    {
        std::shared_ptr<Video::AsyncVideoRecorder> recorder;
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            recorder = m_MovieRecorder;
        }
        if (waitForFinalization && recorder)
        {
            recorder->WaitForFinalization();
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            if (m_MovieRecorder == recorder && recorder->IsFinalized())
            {
                m_MovieRecorder.reset();
            }
        }
        else if (recorder && recorder->IsFinalized())
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            if (m_MovieRecorder == recorder)
            {
                m_MovieRecorder.reset();
            }
        }
        return;
    }

    RequestMovieFinish(message.empty() ? std::string("Recording stopped.") : message);
    if (waitForFinalization)
    {
        m_MovieFinishWaitRequested = true;
        WaitForPendingMovieData();
    }
    else
    {
        Poll();
    }
}

void GameViewMovieCapture::StopForShutdown()
{
    if (!m_MovieOptions)
    {
        std::shared_ptr<Video::AsyncVideoRecorder> recorder;
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            recorder = m_MovieRecorder;
        }
        if (recorder)
        {
            recorder->WaitForFinalization();
        }
        return;
    }

    StopMovieAudioCapture();
    if (auto* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr)
    {
        device->WaitForIdle();
    }
    // Everything the GPU has actually produced is retrievable after the idle;
    // drain it once, then drop the rest — with the render loop gone, pending
    // tickets can never resolve and waiting on them stalls the whole quit.
    // Route through RequestMovieFinish so an in-flight finish keeps its own
    // message instead of being reported as a clean stop.
    RequestMovieFinish("Recording stopped: editor closing.");
    m_MovieFinishWaitRequested = true; // join the encoder so the container closes
    Poll();
    if (!m_MovieOptions)
    {
        return; // the poll completed finalization
    }
    ReleaseDequeMemory(m_MovieReadbacks);
    m_PendingMovieEncodeFrame.reset();
    ClearPendingMovieAudio();
    FinishMovieRecording(m_MovieFinishMessage);
}

Video::SubmitResult GameViewMovieCapture::TrySubmitMovieFrame(Video::OwnedVideoFrame& frame)
{
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    if (!recorder)
        return Video::SubmitResult::Rejected;
    return recorder->SubmitVideoFrame(frame);
}

// A rejected submit is terminal; the recorder's failure text carries the cause
// and the fix (e.g. which encoder refused and why). Surfacing it is what makes
// the refusal actionable — "encoder stopped" alone is not.
std::string GameViewMovieCapture::MovieFailureMessage()
{
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    std::string lastError = recorder ? recorder->GetStats().LastError : std::string();
    if (lastError.empty())
        return "Movie capture failed: encoder stopped.";
    // The writer speaks API-level; translate the hardware-encoder advice into
    // this host's own control.
    if (lastError.find("requireHardwareAcceleration") != std::string::npos)
        lastError += " (Movie Recorder settings: set Encoder mode to software-ok)";
    return lastError;
}

void GameViewMovieCapture::Poll()
{
    if (!m_MovieOptions)
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        if (m_MovieRecorder && m_MovieRecorder->IsFinalized())
        {
            m_MovieRecorder.reset();
        }
        return;
    }

    // Resubmit a frame the encoder couldn't accept last poll before draining new
    // readbacks, so encode order matches capture order.
    if (m_PendingMovieEncodeFrame)
    {
        switch (TrySubmitMovieFrame(*m_PendingMovieEncodeFrame))
        {
        case Video::SubmitResult::Accepted:
            ++m_MovieFramesWritten;
            m_PendingMovieEncodeFrame.reset();
            break;
        case Video::SubmitResult::QueueFull:
            return; // still backed up; retry next poll
        case Video::SubmitResult::Rejected:
            m_PendingMovieEncodeFrame.reset();
            FinishMovieRecording(MovieFailureMessage());
            return;
        }
    }

    for (auto it = m_MovieReadbacks.begin(); it != m_MovieReadbacks.end();)
    {
        std::shared_ptr<Video::AsyncVideoRecorder> recorder;
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            recorder = m_MovieRecorder;
        }
        if (recorder && !recorder->CanAcceptVideoFrame())
        {
            break;
        }

        // RenderGraph ghost entries: a ticket consumed without resolving here was
        // cancelled by a re-begun frame incarnation. Erase it or the pending
        // backpressure gate counts it forever and scheduling wedges.
        if (it->Rg && it->Rg->IsConsumed())
        {
            it = m_MovieReadbacks.erase(it);
            continue;
        }
        if (!it->Rg)
        {
            it = m_MovieReadbacks.erase(it);
            continue;
        }

        Rendering::ViewReadbackResult result;
        const bool ready = it->Rg->TryGet(result);
        if (!ready)
        {
            // Preserve capture order. Later GPU readbacks can complete before
            // older ones; encoding them ahead of the older frame causes visible
            // jumps in the movie timeline.
            break;
        }

        if (!EnsureMovieRecorder(result.width, result.height))
        {
            return;
        }

        const uint32_t bytesPerPixel = Rendering::BytesPerPixel(result.format);
        if (bytesPerPixel != 4u)
        {
            FinishMovieRecording("Movie capture failed: expected 4-byte color pixels.");
            return;
        }

        Video::OwnedVideoFrame frame{};
        frame.Pixels = std::move(result.pixels);
        frame.Width = result.width;
        frame.Height = result.height;
        frame.StrideBytes = result.width * 4u;
        frame.Format = result.format == TextureFormat::RGB10A2_UNORM
                           ? Video::VideoPixelFormat::RGB10A2
                       : (result.format == TextureFormat::BGRA8_UNORM || result.format == TextureFormat::BGRA8_SRGB)
                           ? Video::VideoPixelFormat::BGRA8
                           : Video::VideoPixelFormat::RGBA8;
        // Unity Recorder-style timeline: one rendered engine frame becomes one
        // movie frame at the requested FPS. Leave TimestampSeconds negative so
        // VideoWriter stamps frameIndex / fps instead of wall-clock time.
        frame.TimestampSeconds = -1.0;

        m_LastMovieFramePixels = frame.Pixels;
        m_LastMovieFrameWidth = frame.Width;
        m_LastMovieFrameHeight = frame.Height;
        m_LastMovieFrameStrideBytes = frame.StrideBytes;
        m_LastMovieFrameFormat = frame.Format;

        switch (TrySubmitMovieFrame(frame))
        {
        case Video::SubmitResult::Accepted:
            ++m_MovieFramesWritten;
            it = m_MovieReadbacks.erase(it);
            break;
        case Video::SubmitResult::QueueFull:
            // Encoder is behind: hold this frame, stop draining, retry next poll.
            // Backpressure stalls scheduling rather than ending the recording.
            m_PendingMovieEncodeFrame = std::move(frame);
            it = m_MovieReadbacks.erase(it);
            return;
        case Video::SubmitResult::Rejected:
            FinishMovieRecording(MovieFailureMessage());
            return;
        }
    }

    if (m_MovieOptions &&
        m_MovieFrameLimit > 0 &&
        m_MovieFramesScheduled >= m_MovieFrameLimit &&
        m_MovieFramesWritten >= m_MovieFrameLimit &&
        m_MovieReadbacks.empty())
    {
        const std::string filename = std::filesystem::path(m_MovieOptions->path).filename().string();
        RequestMovieFinish("Recording finished: " + filename);
    }

    if (m_MovieOptions && m_MovieFinishRequested && !HasPendingMovieVideoFrames())
    {
        if (!DrainPendingMovieAudio())
        {
            return;
        }
        FinishMovieRecording(m_MovieFinishMessage);
    }
}

void GameViewMovieCapture::DeclareCaptureRG(Rendering::RenderGraph::RGFrame& frame,
                                            Rendering::RenderGraph::RGTexture source,
                                            uint32_t width, uint32_t height,
                                            Rendering::Passes::FinalizeInputSpace sourceSpace,
                                            float debandLsb,
                                            Rendering::TextureFormat presentedFormat)
{
    namespace RenderGraph = Rendering::RenderGraph;

    // Encode this frame's output into a transient BGRA8 target and ticket the
    // readback. If the recorder falls behind, stall here so the recording
    // remains gapless instead of dropping the current rendered frame.
    if (!IsCapturing())
        return;
    if (DelayMovieCaptureFrame())
        return;
    StartMovieAudioCapture();
    if (m_MovieFrameLimit > 0 && m_MovieFramesScheduled >= m_MovieFrameLimit)
        return;
    if (!WaitForMovieBackpressure())
        return;

    // HDR recording captures a 10-bit PQ/HLG target; SDR keeps the 8-bit
    // sRGB target. Which pass owns the OETF depends on `sourceSpace`: on a
    // linear source this encode applies it, on an encoded one the view's
    // finalize already did and this pass only moves (and possibly requantizes)
    // the code values.
    const Video::VideoHdrMode movieHdr =
        m_MovieOptions ? m_MovieOptions->hdrMode : Video::VideoHdrMode::Off;
    std::optional<Rendering::HdrOutputMode> encodeOverride;
    if (movieHdr == Video::VideoHdrMode::HDR10_PQ)
        encodeOverride = Rendering::HdrOutputMode::HDR10_PQ;
    else if (movieHdr == Video::VideoHdrMode::HLG)
        encodeOverride = Rendering::HdrOutputMode::HLG;
    const TextureFormat movieFmt =
        encodeOverride ? TextureFormat::RGB10A2_UNORM : TextureFormat::BGRA8_UNORM;
    const char* movieDstName = encodeOverride ? "GameView.MovieReadbackRGB10A2" : "GameView.MovieReadbackBGRA8";
    // Transient is fine here: the readback copy is in-frame and nothing
    // old-graph samples it (unlike the PP output the Game View stashes).
    Rendering::TextureDesc movieDesc{};
    movieDesc.width = std::max(1u, width);
    movieDesc.height = std::max(1u, height);
    movieDesc.mipLevels = 1;
    movieDesc.arrayLayers = 1;
    movieDesc.sampleCount = 1;
    movieDesc.format = static_cast<uint32_t>(movieFmt);
    movieDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    movieDesc.debugName = movieDstName;
    const RenderGraph::RGTexture movieDst =
        frame.CreateTexture(movieDstName, movieDesc);
    if (!movieDst.IsValid())
        return;
    // The movie target is 8-bit (10-bit under an HDR override) whatever the
    // screen runs at, and who owns its quantization step is the shared decision
    // in SRGBEncodePass.h — the Player's recording takes the same one, which is
    // what keeps the two hosts' movies from diverging. Whenever this pass owns
    // the step it owns the deband at that step too (GE_DEBAND env overrides
    // applied inside the pass), so a recording keeps matching the display.
    // `presentedFormat` is the step the finalize consumed this frame, threaded
    // from the window owner — the movie's source step by construction, not a
    // second read of whichever window target is active.
    const auto movieQuantizer = Rendering::Passes::SelectTransferQuantizer(
        sourceSpace == Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
        presentedFormat, movieFmt);
    const bool movieOwnsStep = movieQuantizer != Rendering::Passes::FinalizeQuantizer::None;
    // Stated once per recording, because "which arm did this file come from" is
    // otherwise unanswerable after the fact: temporal movie dither reaches the
    // image only where this pass owns the step (a 10-bit screen recorded to
    // 8-bit, or a linear source); at equal depth it is a no-op by construction.
    if (m_MovieFramesScheduled == 0)
    {
        Logger::Log::Info(
            "GameViewMovieCapture: recording source={} movie={} quantizer={} temporalDither={}",
            Rendering::ToString(presentedFormat), Rendering::ToString(movieFmt),
            movieOwnsStep ? "Destination" : "None",
            Rendering::Passes::IsTemporalMovieDitherEnabled() ? "on" : "off");
    }
    if (!Rendering::Passes::AddSRGBEncodePassRG(
             frame, source, movieDst,
             {.InputSpace = sourceSpace,
              .Quantizer = movieQuantizer,
              .EncodeOverride = encodeOverride,
              .VolumeDebandThresholdLsb = movieOwnsStep ? debandLsb : 0.0f,
              // Seeded on the ACCEPTED-capture count, never the render frame
              // index: render frames are dropped by back-pressure and by capture
              // warm-up, so only the ordinal makes movie frame N carry
              // realisation N in both hosts (TemporalDither.h).
              .DitherPhase = Rendering::Passes::MovieDitherPhase(m_MovieFramesScheduled)},
             "GameView.MovieSRGBEncode")
             .IsValid())
        return;
    if (auto ticket = Rendering::RequestTextureReadbackRG(frame.Device(), frame, movieDst,
                                                          "GameViewMovieFrame"))
    {
        PendingMovieFrame pending{};
        pending.Rg = std::move(ticket);
        m_MovieReadbacks.push_back(std::move(pending));
        ++m_MovieFramesScheduled;
    }
}

bool GameViewMovieCapture::EnsureMovieRecorder(uint32_t width, uint32_t height)
{
    if (!m_MovieOptions)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        if (m_MovieRecorder)
        {
            m_MovieOptions->width = width;
            m_MovieOptions->height = height;
            return true;
        }
    }

    Video::VideoWriterOptions options = *m_MovieOptions;
    options.width = width;
    options.height = height;
    if (options.codec == Video::VideoCodec::Auto)
    {
        options.codec = Video::GuessCodecForMoviePath(options.path);
    }

    *m_MovieOptions = options;
    return true;
}

bool GameViewMovieCapture::DelayMovieCaptureFrame()
{
    if (m_MovieWarmupFramesRemaining == 0)
    {
        return false;
    }
    --m_MovieWarmupFramesRemaining;
    return true;
}

bool GameViewMovieCapture::HasMovieBackpressure() const
{
    return m_PendingMovieEncodeFrame.has_value() || m_MovieReadbacks.size() >= kMaxPendingMovieReadbacks;
}

bool GameViewMovieCapture::WaitForOldestMovieReadback(std::chrono::steady_clock::time_point deadline)
{
    // Poll drains strictly in capture order and stops at the first unresolved
    // ticket, so the head is the only readback whose completion can unblock it.
    if (m_MovieReadbacks.empty() || !m_MovieReadbacks.front().Rg)
    {
        return false;
    }
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
    {
        return false;
    }
    const auto remainingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
    return m_MovieReadbacks.front().Rg->WaitUntilReady(static_cast<uint64_t>(remainingNs));
}

bool GameViewMovieCapture::WaitForMovieBackpressure()
{
    if (!HasMovieBackpressure())
    {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + kMovieBackpressureTimeout;
    while (m_MovieOptions && !m_MovieFinishRequested && HasMovieBackpressure())
    {
        Poll();
        if (!m_MovieOptions || m_MovieFinishRequested || !HasMovieBackpressure())
        {
            break;
        }

        // Declare-phase code: the wait stays scoped to the readback's own
        // submission because a device drain here stalls every queue mid-declare.
        // When the encoder rather than the GPU is the blocker there is nothing
        // to wait on and the sleep below paces the retry.
        if (WaitForOldestMovieReadback(deadline))
        {
            Poll();
            if (!m_MovieOptions || m_MovieFinishRequested || !HasMovieBackpressure())
            {
                break;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            const std::string path = m_MovieOptions ? m_MovieOptions->path : std::string{};
            Logger::Log::Error("GameView: timed out waiting for movie recorder backpressure for '{}'", path);
            FinishMovieRecording("Movie capture timed out waiting for the video recorder to catch up.");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return m_MovieOptions.has_value() && !m_MovieFinishRequested;
}

void GameViewMovieCapture::StartMovieAudioCapture()
{
    if (m_MovieAudioCaptureArmed || !m_MovieOptions || !m_MovieOptions->recordAudio)
    {
        return;
    }

    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
    {
        audio->SetOutputCaptureCallback([this](const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate) {
            AppendMovieAudio(samples, frameCount, channels, sampleRate);
        });
        m_MovieAudioCaptureArmed = true;
    }
}

void GameViewMovieCapture::StopMovieAudioCapture()
{
    if (!m_MovieAudioCaptureArmed)
    {
        return;
    }
    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
    {
        audio->SetOutputCaptureCallback(nullptr);
    }
    m_MovieAudioCaptureArmed = false;
}

void GameViewMovieCapture::AppendMovieAudio(const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate)
{
    if (!samples || frameCount == 0 || channels == 0 || sampleRate == 0)
    {
        return;
    }
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        if (m_MovieAudioChannels == 0 || m_MovieAudioSampleRate == 0)
        {
            m_MovieAudioChannels = channels;
            m_MovieAudioSampleRate = sampleRate;
        }
        if (channels != m_MovieAudioChannels || sampleRate != m_MovieAudioSampleRate)
        {
            return;
        }
        recorder = m_MovieRecorder;
    }

    const size_t sampleCount = static_cast<size_t>(frameCount) * channels;
    Video::OwnedAudioSamples audio{};
    audio.Samples.assign(samples, samples + sampleCount);
    audio.FrameCount = frameCount;
    audio.Channels = channels;
    audio.SampleRate = sampleRate;
    if (recorder)
    {
        EnqueueMovieAudio(recorder, std::move(audio));
    }
}

void GameViewMovieCapture::EnqueueMovieAudio(const std::shared_ptr<Video::AsyncVideoRecorder>& recorder,
                                             Video::OwnedAudioSamples&& audio)
{
    const auto chunkBytes = [](const Video::OwnedAudioSamples& a) { return a.Samples.size() * sizeof(float); };

    std::lock_guard<std::mutex> lock(m_MovieAudioBufferMutex);

    // Drain anything held from earlier backpressure first, in capture order.
    while (!m_PendingMovieAudio.empty())
    {
        const Video::SubmitResult result = recorder->SubmitAudioSamples(m_PendingMovieAudio.front());
        if (result == Video::SubmitResult::QueueFull)
        {
            break; // still backed up; keep the rest buffered for the next callback
        }
        // Accepted (consumed) or Rejected (recorder stopped) — either way it leaves.
        m_PendingMovieAudioBytes -= std::min(m_PendingMovieAudioBytes, chunkBytes(m_PendingMovieAudio.front()));
        m_PendingMovieAudio.pop_front();
    }

    // Submit the new chunk directly only if nothing is buffered ahead of it; otherwise
    // buffer it to preserve order (audio PTS is monotonic, so reordering corrupts sync).
    if (m_PendingMovieAudio.empty())
    {
        if (recorder->SubmitAudioSamples(audio) != Video::SubmitResult::QueueFull)
        {
            return;
        }
    }
    m_PendingMovieAudioBytes += chunkBytes(audio);
    m_PendingMovieAudio.push_back(std::move(audio));

    // Bound the buffer so a wedged encoder can't grow it without limit. Dropping the
    // oldest is unavoidable audio loss, but only after a multi-second encoder stall.
    while (m_PendingMovieAudioBytes > kMaxPendingMovieAudioBytes && !m_PendingMovieAudio.empty())
    {
        m_PendingMovieAudioBytes -= std::min(m_PendingMovieAudioBytes, chunkBytes(m_PendingMovieAudio.front()));
        m_PendingMovieAudio.pop_front();
    }
}

void GameViewMovieCapture::ClearPendingMovieAudio()
{
    std::lock_guard<std::mutex> lock(m_MovieAudioBufferMutex);
    m_PendingMovieAudio.clear();
    m_PendingMovieAudioBytes = 0;
}

bool GameViewMovieCapture::DrainPendingMovieAudio()
{
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    if (!recorder)
    {
        ClearPendingMovieAudio();
        return true;
    }

    const auto chunkBytes = [](const Video::OwnedAudioSamples& a) { return a.Samples.size() * sizeof(float); };
    std::lock_guard<std::mutex> lock(m_MovieAudioBufferMutex);
    while (!m_PendingMovieAudio.empty())
    {
        const Video::SubmitResult result = recorder->SubmitAudioSamples(m_PendingMovieAudio.front());
        if (result == Video::SubmitResult::QueueFull)
        {
            return false;
        }
        m_PendingMovieAudioBytes -= std::min(m_PendingMovieAudioBytes, chunkBytes(m_PendingMovieAudio.front()));
        m_PendingMovieAudio.pop_front();
    }
    return true;
}

void GameViewMovieCapture::RequestMovieFinish(std::string message)
{
    if (!m_MovieOptions)
    {
        return;
    }

    StopMovieAudioCapture();
    if (!m_MovieFinishRequested)
    {
        m_MovieFinishRequested = true;
        m_MovieFinishMessage = std::move(message);
        return;
    }

    if (m_MovieFinishMessage.empty())
    {
        m_MovieFinishMessage = std::move(message);
    }
}

bool GameViewMovieCapture::HasPendingMovieVideoFrames() const
{
    return m_PendingMovieEncodeFrame.has_value() || !m_MovieReadbacks.empty();
}

void GameViewMovieCapture::WaitForPendingMovieData()
{
    const auto deadline = std::chrono::steady_clock::now() + kMovieBackpressureTimeout;
    while (m_MovieOptions && m_MovieFinishRequested)
    {
        Poll();
        if (!m_MovieOptions || !m_MovieFinishRequested)
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            const std::string path = m_MovieOptions ? m_MovieOptions->path : std::string{};
            Logger::Log::Warning("GameView: timed out while draining pending movie frames for '{}'", path);
            ReleaseDequeMemory(m_MovieReadbacks);
            m_PendingMovieEncodeFrame.reset();
            ClearPendingMovieAudio();
            FinishMovieRecording("Movie finalization timed out while waiting for pending frames.");
            break;
        }
        // Retire the head readback's own submission rather than the device; the
        // encoder-bound case has nothing to wait on and falls through to the sleep.
        if (!WaitForOldestMovieReadback(deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void GameViewMovieCapture::FinishMovieRecording(std::string message)
{
    const bool hadOptions = m_MovieOptions.has_value();
    StopMovieAudioCapture();
    const std::string path = m_MovieOptions ? m_MovieOptions->path : std::string{};
    const uint64_t framesWritten = m_MovieFramesWritten;
    const bool waitForFinalization = m_MovieFinishWaitRequested;
    auto statusCallback = m_MovieStatusCallback;
    Video::OwnedVideoFrame endFadeFrame{};
    endFadeFrame.Pixels = std::move(m_LastMovieFramePixels);
    endFadeFrame.Width = m_LastMovieFrameWidth;
    endFadeFrame.Height = m_LastMovieFrameHeight;
    endFadeFrame.StrideBytes = m_LastMovieFrameStrideBytes;
    endFadeFrame.Format = m_LastMovieFrameFormat;
    endFadeFrame.TimestampSeconds = -1.0;
    ReleaseVectorMemory(m_LastMovieFramePixels);

    ReleaseDequeMemory(m_MovieReadbacks);
    m_MovieOptions.reset();
    m_MovieFrameLimit = 0;
    m_MovieFramesScheduled = 0;
    m_MovieWarmupFramesRemaining = 0;
    m_PendingMovieEncodeFrame.reset();
    ClearPendingMovieAudio();
    m_MovieFinishRequested = false;
    m_MovieFinishWaitRequested = false;
    m_MovieFinishMessage.clear();
    m_LastMovieFrameWidth = 0;
    m_LastMovieFrameHeight = 0;
    m_LastMovieFrameStrideBytes = 0;

    if (hadOptions)
    {
        Logger::Log::Info("GameView: {}", message);
    }
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
        m_MovieAudioChannels = 0;
        m_MovieAudioSampleRate = 0;
    }
    if (recorder)
    {
        recorder->Finish(
            std::move(endFadeFrame),
            message,
            [path, framesWritten, statusCallback = std::move(statusCallback)](bool ok, const std::string& finalMessage) {
                if (ok)
                {
                    Logger::Log::Info("GameView: Finished play-mode movie capture '{}' ({} frames)", path, framesWritten);
                }
                else
                {
                    Logger::Log::Error("GameView: Failed to finish movie '{}': {}", path, finalMessage);
                }
                if (statusCallback)
                {
                    statusCallback(finalMessage);
                }
            },
            waitForFinalization);
        if (waitForFinalization || recorder->IsFinalized())
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            if (m_MovieRecorder == recorder)
            {
                m_MovieRecorder.reset();
            }
        }
    }
    else if (statusCallback)
    {
        statusCallback(message);
    }
}

} // namespace GameEngine::Editor
