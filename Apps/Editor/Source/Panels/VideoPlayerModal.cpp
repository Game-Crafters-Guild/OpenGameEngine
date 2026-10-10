#include "Panels/VideoPlayerModal.h"

#include "Platform/SystemMetrics.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Text/FontAtlas.h"
#include "Input/KeyCodes.h"
#include "Platform/Shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <cstring>

namespace GameEngine
{

namespace {
constexpr int kRetiredTextureFrames = 4;
constexpr const char* kVideoPlayerFrameTextureName = "video_player_frame";

Rendering::TextureFormat TextureFormatForVideo(Video::VideoPixelFormat format)
{
    return format == Video::VideoPixelFormat::BGRA8
        ? Rendering::TextureFormat::BGRA8_SRGB
        : Rendering::TextureFormat::RGBA8_SRGB;
}

// Backends differ in the row pitch a texture upload requires. The copy's source
// row pitch must match what the staging buffer was filled with, so both the
// staging fill and the copy pass derive it from this single function.
size_t UploadRowPitch(Rendering::IDevice* device, size_t tightRowPitchBytes)
{
    if (device && device->GetAPI() == Rendering::GraphicsAPI::DirectX12)
    {
        constexpr size_t kD3D12TextureDataPitchAlignment = 256;
        return (tightRowPitchBytes + kD3D12TextureDataPitchAlignment - 1u) &
               ~(kD3D12TextureDataPitchAlignment - 1u);
    }
    return tightRowPitchBytes;
}
}

// Format seconds as "M:SS".
static std::string FormatTime(float seconds)
{
    if (seconds < 0.0f) seconds = 0.0f;
    const int total = static_cast<int>(seconds);
    const int m = total / 60;
    const int s = total % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

VideoPlayerModal::VideoPlayerModal()
{
    BuildUI();
}

VideoPlayerModal::~VideoPlayerModal()
{
    // The modal lives under its UIManager's root, so it is destroyed either
    // when that root is replaced (the manager is alive and must drop the frame
    // texture, or it keeps a registration for a texture destroyed below) or
    // inside ~UIManager (being destroyed; its texture maps die with it).
    if (m_UIManager && m_UIManager->IsBeingDestroyed())
        m_UIManager = nullptr;
    if (m_Open)
        Close();
    RetireFrameTexture();
    DrainRetiredTextures(true);
}

void VideoPlayerModal::BuildUI()
{
    // The root uses edge anchoring in CSS because percentage sizing can
    // collapse when its containing block also derives its size by percentage.
    AddClass("video-player-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    SetFocusable(true);
    RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Open)
            return;
        if (e.Key == Input::kKeyCode_Space)
        {
            TogglePlayback();
            e.Stop();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            if (m_Fullscreen)
                SetFullscreen(false);
            else
                Close();
            e.Stop();
        }
    });

    // Backdrop (semi-transparent dimmer + centering container).
    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("vpm-backdrop");

    // Window card.
    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("vpm-window");

    // ── Header ──────────────────────────────────────────────────
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("vpm-header");

        BindDragHandle(header.get());

        auto titleLabel = std::make_unique<Label>();
        m_TitleLabel = titleLabel.get();
        m_TitleLabel->SetText("Video");
        m_TitleLabel->AddClass("vpm-title");
        m_TitleLabel->AddClass("vpm-title-action");
        m_TitleLabel->SetTooltip("Reveal in file manager");
        m_TitleLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
        {
            if (e.Button != 0 || !m_Open)
                return;
            if (!IsTitleTextHit(e.X, e.Y))
                return;
            RevealVideoInFileManager();
            e.Stop();
        });
        header->AddChild(std::move(titleLabel));

        auto titleSpacer = std::make_unique<UIElement>();
        titleSpacer->AddClass("vpm-title-spacer");
        header->AddChild(std::move(titleSpacer));

        auto closeBtn = std::make_unique<Button>();
        m_CloseBtn = closeBtn.get();
        m_CloseBtn->AddClass("vpm-close-btn");
        m_CloseBtn->AddClass("icon-button");
        m_CloseBtn->AddClass("xclose-icon");
        m_CloseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Close(); });
        header->AddChild(std::move(closeBtn));

        m_Window->AddChild(std::move(header));
    }

    // ── Video area ───────────────────────────────────────────────
    {
        auto videoArea = std::make_unique<UIElement>();
        m_VideoArea = videoArea.get();
        m_VideoArea->AddClass("vpm-video-area");
        // Background-image bound at runtime via UIManager external texture.
        m_VideoArea->Styles()
            .SetBackgroundResourceName(kVideoPlayerFrameTextureName)
            .SetBackgroundSizeContain();
        m_VideoArea->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
        {
            if (!m_Open || e.Button != 0)
                return;

            constexpr float kDoubleClickTolerancePx = 8.0f;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - m_LastVideoClickTime;
            const bool isDoubleClick =
                elapsed >= std::chrono::steady_clock::duration::zero() &&
                elapsed < GameEngine::Platform::GetDoubleClickInterval() &&
                std::abs(e.X - m_LastVideoClickX) <= kDoubleClickTolerancePx &&
                std::abs(e.Y - m_LastVideoClickY) <= kDoubleClickTolerancePx;

            m_LastVideoClickTime = now;
            m_LastVideoClickX = e.X;
            m_LastVideoClickY = e.Y;

            if (isDoubleClick)
            {
                TriggerFullscreenButton();
                m_LastVideoClickTime = {};
                e.Stop();
                return;
            }

            TogglePlayback();
            e.Stop();
        });

        m_Window->AddChild(std::move(videoArea));
    }

    // ── Seek bar ─────────────────────────────────────────────────
    {
        auto seekBar = std::make_unique<UIElement>();
        m_SeekBar = seekBar.get();
        m_SeekBar->AddClass("vpm-seek-bar");

        auto seekFill = std::make_unique<UIElement>();
        m_SeekFill = seekFill.get();
        m_SeekFill->AddClass("vpm-seek-fill");
        seekBar->AddChild(std::move(seekFill));

        // Click / drag to seek.
        m_SeekBar->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
        {
            if (!m_Player.IsLoaded()) return;
            m_SeekDragging = true;
            e.Capture(m_SeekBar);
            const float barW = m_SeekBar->GetLayoutWidth();
            if (barW > 0.0f)
            {
                const float ratio = std::clamp((e.X - m_SeekBar->GetLayoutX()) / barW, 0.0f, 1.0f);
                m_Player.Seek(ratio * m_Player.GetDuration());
            }
            e.Stop();
        });

        m_SeekBar->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
        {
            if (!m_SeekDragging) return;
            const float barW = m_SeekBar->GetLayoutWidth();
            if (barW > 0.0f)
            {
                const float ratio = std::clamp((e.X - m_SeekBar->GetLayoutX()) / barW, 0.0f, 1.0f);
                m_Player.Seek(ratio * m_Player.GetDuration());
            }
        });

        m_SeekBar->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
        {
            m_SeekDragging = false;
            (void)e;
        });

        m_Window->AddChild(std::move(seekBar));
    }

    // ── Controls row ─────────────────────────────────────────────
    {
        auto controls = std::make_unique<UIElement>();
        controls->AddClass("vpm-controls");

        auto timeLabel = std::make_unique<Label>();
        m_TimeLabel = timeLabel.get();
        m_TimeLabel->SetText("0:00 / 0:00");
        m_TimeLabel->AddClass("vpm-time-label");
        controls->AddChild(std::move(timeLabel));

        auto controlGroup = std::make_unique<UIElement>();
        controlGroup->AddClass("vpm-control-group");

        auto stop = std::make_unique<Button>();
        m_StopBtn = stop.get();
        m_StopBtn->AddClass("vpm-ctrl-btn");
        m_StopBtn->AddClass("icon-button");
        m_StopBtn->AddClass("stop-icon");
        m_StopBtn->SetTooltip("Stop");
        m_StopBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { StopPlayback(); });
        controlGroup->AddChild(std::move(stop));

        auto playPause = std::make_unique<Button>();
        m_PlayPause = playPause.get();
        m_PlayPause->AddClass("vpm-ctrl-btn");
        m_PlayPause->AddClass("icon-button");
        m_PlayPause->AddClass("play-icon");
        m_PlayPause->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePlayback(); });
        controlGroup->AddChild(std::move(playPause));

        controls->AddChild(std::move(controlGroup));

        auto rightSpacer = std::make_unique<UIElement>();
        rightSpacer->AddClass("vpm-controls-right-spacer");
        controls->AddChild(std::move(rightSpacer));

        auto fullscreen = std::make_unique<Button>();
        m_FullscreenBtn = fullscreen.get();
        m_FullscreenBtn->AddClass("vpm-ctrl-btn");
        m_FullscreenBtn->AddClass("icon-button");
        m_FullscreenBtn->AddClass("fullscreen-icon");
        m_FullscreenBtn->SetTooltip("Fullscreen");
        m_FullscreenBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ToggleFullscreen(); });
        controls->AddChild(std::move(fullscreen));

        m_Window->AddChild(std::move(controls));
    }

    m_Backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void VideoPlayerModal::Open(const std::string& videoPath, const std::string& title)
{
    const bool wasOpen = m_Open;
    if (!wasOpen)
        ResetFrameTextureBinding();

    m_VideoPath = videoPath;
    m_Player.Unload();
    m_Player.SetLoop(false);
    m_Player.Load(videoPath);
    m_Player.Play();
    m_Open = true;
    if (!wasOpen)
    {
        PlaceWindow(0.0f, 0.0f);
        SetFullscreen(false);
    }
    CancelWindowDrag();
    m_LastVideoClickTime = {};

    if (!title.empty())
    {
        m_Title = title;
    }
    else
    {
        const auto sep = videoPath.find_last_of("/\\");
        m_Title = (sep == std::string::npos) ? videoPath : videoPath.substr(sep + 1);
    }
    if (m_TitleLabel)
        m_TitleLabel->SetText(m_Title);
    if (m_SeekFill)
        m_SeekFill->Overrides().Set(Style::Width, StyleLength::Percent(0.0f));

    AddClass("visible");
    if (m_UIManager)
    {
        UIManager* manager = m_UIManager;
        manager->PostToUI([manager, this]() { manager->FocusElement(this); });
    }

    UpdateControlState();
}

void VideoPlayerModal::Close()
{
    m_Open = false;
    SetFullscreen(false);
    m_Player.Stop();
    m_Player.Unload();
    m_VideoPath.clear();
    m_Title.clear();
    m_LastVideoClickTime = {};
    if (m_TitleLabel)
        m_TitleLabel->SetText("Video");

    ResetFrameTextureBinding();

    RemoveClass("visible");

    if (m_OnClose)
        m_OnClose();
}

void VideoPlayerModal::Update()
{
    DrainRetiredTextures();
    m_PendingUploadSlot = kNoPendingUpload;

    if (!m_Open || !m_Player.IsLoaded())
        return;

    // The player paces itself; this tick only samples what is already decoded.
    if (m_Player.PollNewFrame())
    {
        const int w = m_Player.GetWidth();
        const int h = m_Player.GetHeight();
        if (w > 0 && h > 0)
        {
            const Video::VideoPixelFormat frameFormat = m_Player.GetFramePixelFormat();
            EnsureTexture(w, h, frameFormat);

            auto& slot = m_FrameTextures[m_FrameTextureCursor];
            if (slot.handle.IsValid() && slot.staging.IsValid())
            {
                const uint8_t* pixels = m_Player.AcquireFramePointer();
                if (!pixels)
                    return;

                const size_t tightRowPitch = static_cast<size_t>(w) * 4;
                const size_t uploadRowPitch = UploadRowPitch(m_Device, tightRowPitch);

                // Stage the pixels now; the GPU copy is declared in the
                // render-graph frame by TickRenderRG so the graph orders it
                // before the UI samples the texture this frame.
                if (uploadRowPitch == tightRowPitch)
                {
                    m_Device->UpdateBuffer(slot.staging, 0,
                        static_cast<size_t>(h) * uploadRowPitch, pixels);
                }
                else
                {
                    std::vector<unsigned char> padded(static_cast<size_t>(h) * uploadRowPitch, 0);
                    for (int y = 0; y < h; ++y)
                        std::memcpy(padded.data() + static_cast<size_t>(y) * uploadRowPitch,
                                    pixels + static_cast<size_t>(y) * tightRowPitch,
                                    tightRowPitch);
                    m_Device->UpdateBuffer(slot.staging, 0, padded.size(), padded.data());
                }

                m_PendingUploadSlot = m_FrameTextureCursor;
                m_PendingUploadWidth = static_cast<uint32_t>(w);
                m_PendingUploadHeight = static_cast<uint32_t>(h);
                m_PendingUploadRowPitch = uploadRowPitch;

                m_FrameTextureCursor = (m_FrameTextureCursor + 1u) % m_FrameTextures.size();
            }
        }
    }

    UpdateControlState();
}

void VideoPlayerModal::TickRenderRG(Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_UIManager || !m_Device)
        return;

    // The slot whose texture the UI should sample this frame. When a frame was
    // staged this Update it is that slot; otherwise re-publish the most recent
    // upload so the UI's sampled-read keeps ordering the (already-copied)
    // texture even on frames that decoded nothing.
    size_t publishSlot = m_PendingUploadSlot;
    if (publishSlot == kNoPendingUpload)
    {
        const size_t mostRecent =
            (m_FrameTextureCursor + m_FrameTextures.size() - 1u) % m_FrameTextures.size();
        if (!m_FrameTextures[mostRecent].uploaded)
            return; // nothing has ever been copied — nothing to show yet
        publishSlot = mostRecent;
    }

    FrameTextureSlot& slot = m_FrameTextures[publishSlot];
    if (!slot.handle.IsValid())
        return;

    const Rendering::TextureFormat format = TextureFormatForVideo(m_TextureFormat);

    // Declare the staged-frame copy as a graph pass. The graph imports the
    // texture at its tracked state and inserts the layout transitions; the UI
    // pass's sampled read of the published texture is the producer->consumer
    // barrier that orders this copy before the sample (the same mechanism the
    // scene viewport and thumbnails use).
    if (m_PendingUploadSlot != kNoPendingUpload && slot.staging.IsValid())
    {
        const std::string texName =
            std::string("Editor.VideoPlayer.Slot") + std::to_string(publishSlot);
        const Rendering::RenderGraph::RGTexture tex = frame.ImportExternalTexture(
            texName.c_str(), slot.handle,
            slot.uploaded ? Rendering::ResourceState::ShaderResource
                          : Rendering::ResourceState::Undefined,
            format);
        const Rendering::RenderGraph::RGBuffer staging =
            frame.ImportExternalBuffer((texName + ".Staging").c_str(), slot.staging);

        const uint32_t width = m_PendingUploadWidth;
        const uint32_t height = m_PendingUploadHeight;
        const size_t rowPitch = m_PendingUploadRowPitch;
        const Rendering::TextureHandle dstTex = slot.handle;
        const Rendering::BufferHandle srcBuf = slot.staging;

        // Phase < kUI so the copy is scheduled ahead of the UI pass; the UI's
        // sampled read of the published texture is the hard dependency edge.
        frame.AddPass(
            "VideoPlayer Upload", Rendering::PassPhase::kOverlay,
            [&](Rendering::RenderGraph::RGPassBuilder& p)
            {
                p.Read(staging, Rendering::RenderGraph::RGBufferRead::CopySrc);
                p.Write(tex, Rendering::RenderGraph::RGTextureWrite::CopyDst);
            },
            [dstTex, srcBuf, width, height, rowPitch](Rendering::RenderGraph::RGContext& ctx)
            {
                ctx.Cmd->CopyBufferToTextureSubresource(srcBuf, dstTex, 0, 0, width, height,
                                                        0, rowPitch);
            });

        slot.uploaded = true;
        m_UIManager->PublishExternalTextureRG(kVideoPlayerFrameTextureName, frame, tex);
    }
    else
    {
        // No new frame this tick — import the already-copied texture so the UI
        // can sample it (the import carries no copy pass; the publish + UI read
        // still establish the resource and its tracked state).
        const std::string texName =
            std::string("Editor.VideoPlayer.Slot") + std::to_string(publishSlot);
        const Rendering::RenderGraph::RGTexture tex = frame.ImportExternalTexture(
            texName.c_str(), slot.handle, Rendering::ResourceState::ShaderResource, format);
        m_UIManager->PublishExternalTextureRG(kVideoPlayerFrameTextureName, frame, tex);
    }
}

void VideoPlayerModal::TogglePlayback()
{
    if (!m_Open || !m_Player.IsLoaded())
        return;

    if (m_Player.IsPlaying())
    {
        m_Player.Pause();
    }
    else
    {
        const float dur = m_Player.GetDuration();
        if (dur > 0.0f && m_Player.GetCurrentTime() >= dur - 0.05f)
        {
            Open(m_VideoPath, m_Title);
            return;
        }
        else
        {
            m_Player.Play();
        }
    }

    UpdateControlState();
}

void VideoPlayerModal::StopPlayback()
{
    if (!m_Open || !m_Player.IsLoaded())
        return;

    m_Player.Pause();
    m_Player.Seek(0.0f);
    UpdateControlState();
}

void VideoPlayerModal::RevealVideoInFileManager() const
{
    if (m_VideoPath.empty())
        return;
    Platform::ShowInFileManager(std::filesystem::path(m_VideoPath));
}

bool VideoPlayerModal::IsTitleTextHit(float x, float y) const
{
    if (!m_TitleLabel)
        return false;

    const float labelX = m_TitleLabel->GetLayoutX();
    const float labelY = m_TitleLabel->GetLayoutY();
    const float labelW = m_TitleLabel->GetLayoutWidth();
    const float labelH = m_TitleLabel->GetLayoutHeight();
    if (x < labelX || y < labelY || x >= labelX + labelW || y >= labelY + labelH)
        return false;

    float textW = 0.0f;
    if (m_UIManager)
    {
        const ResolvedStyle& style = m_TitleLabel->GetResolvedStyle();
        if (auto* font = m_UIManager->ResolveFontForStyle(style))
        {
            const float scale = std::max(0.01f, m_UIManager->GetContentScale());
            const float pixelSize = std::max(1.0f, style.Visual.FontSize * scale);
            textW = font->MeasureUtf8(m_TitleLabel->GetText(), pixelSize).width / scale;
        }
    }

    if (textW <= 0.0f)
    {
        const float fontSize = std::max(1.0f, m_TitleLabel->GetResolvedStyle().Visual.FontSize);
        textW = static_cast<float>(m_TitleLabel->GetText().size()) * fontSize * 0.6f;
    }

    constexpr float kHitSlopPx = 4.0f;
    const float titleHitW = std::min(labelW, textW + kHitSlopPx);
    return x < labelX + titleHitW;
}

void VideoPlayerModal::SetFullscreen(bool fullscreen)
{
    if (m_Fullscreen == fullscreen)
        return;

    m_Fullscreen = fullscreen;
    if (fullscreen)
    {
        m_PreFullscreenOffsetX = m_WindowOffsetX;
        m_PreFullscreenOffsetY = m_WindowOffsetY;
        SetWindowDragEnabled(false);
        PlaceWindow(0.0f, 0.0f);
    }
    else
    {
        SetWindowDragEnabled(true);
        PlaceWindow(m_PreFullscreenOffsetX, m_PreFullscreenOffsetY);
    }
    if (m_Window)
    {
        if (fullscreen)
        {
            m_Window->AddClass("fullscreen");
        }
        else
        {
            m_Window->RemoveClass("fullscreen");
        }
        m_Window->MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty | UIElement::VisualDirty);
    }
    if (m_FullscreenBtn)
    {
        if (fullscreen)
        {
            m_FullscreenBtn->AddClass("pressed");
        }
        else
        {
            m_FullscreenBtn->RemoveClass("pressed");
        }
        m_FullscreenBtn->SetTooltip(fullscreen ? "Exit fullscreen" : "Fullscreen");
    }
}

void VideoPlayerModal::ToggleFullscreen()
{
    SetFullscreen(!m_Fullscreen);
}

void VideoPlayerModal::TriggerFullscreenButton()
{
    if (m_FullscreenBtn)
        m_FullscreenBtn->TriggerClick();
    else
        ToggleFullscreen();
}

void VideoPlayerModal::UpdateControlState()
{
    const bool playing = m_Player.IsPlaying();

    if (m_PlayPause)
    {
        if (playing)
        {
            m_PlayPause->RemoveClass("play-icon");
            m_PlayPause->AddClass("pause-icon");
        }
        else
        {
            m_PlayPause->RemoveClass("pause-icon");
            m_PlayPause->AddClass("play-icon");
        }
    }

    const float cur = m_Player.GetCurrentTime();
    const float dur = m_Player.GetDuration();

    if (m_SeekFill && dur > 0.0f)
    {
        const float pct = std::clamp(cur / dur, 0.0f, 1.0f) * 100.0f;
        m_SeekFill->Overrides().Set(Style::Width, StyleLength::Percent(pct));
    }

    if (m_TimeLabel)
        m_TimeLabel->SetText(FormatTime(cur) + " / " + FormatTime(dur));
}

void VideoPlayerModal::ResetFrameTextureBinding()
{
    if (m_UIManager)
        m_UIManager->RemoveExternalTexture(kVideoPlayerFrameTextureName);

    RetireFrameTexture();

    if (m_VideoArea)
        m_VideoArea->MarkDirty(UIElement::VisualDirty | UIElement::StyleDirty);
}

void VideoPlayerModal::EnsureTexture(int w, int h, Video::VideoPixelFormat format)
{
    if (!m_Device) return;
    if (m_FrameTextures[0].handle.IsValid() && w == m_TextureW && h == m_TextureH && format == m_TextureFormat)
        return;

    if (m_FrameTextures[0].handle.IsValid())
    {
        if (m_UIManager)
            m_UIManager->RemoveExternalTexture(kVideoPlayerFrameTextureName);
        RetireFrameTexture();
    }

    Rendering::TextureDesc desc{};
    desc.width       = static_cast<uint32_t>(w);
    desc.height      = static_cast<uint32_t>(h);
    desc.depth       = 1;
    desc.mipLevels   = 1;
    desc.arrayLayers = 1;
    desc.format      = static_cast<uint32_t>(TextureFormatForVideo(format));
    desc.usage       = static_cast<uint32_t>(
        Rendering::TextureUsage::ShaderResource | Rendering::TextureUsage::TransferDst);
    desc.sampleCount = 1;
    desc.persistent  = true;
    desc.debugName   = "VideoPlayerModalFrame";

    // Each slot owns its own staging buffer so a slot's pixels are never
    // overwritten while its copy is still pending in a render-graph frame.
    const size_t tightRowPitch = static_cast<size_t>(w) * 4;
    const size_t stagingSize = static_cast<size_t>(h) * UploadRowPitch(m_Device, tightRowPitch);
    for (auto& slot : m_FrameTextures)
    {
        slot.handle = m_Device->CreateTexture(desc);
        slot.staging = m_Device->CreateUploadBuffer(stagingSize, "VideoPlayerModalUpload");
        slot.stagingSize = stagingSize;
        slot.uploaded = false;
    }
    m_FrameTextureCursor = 0;
    m_TextureW = w;
    m_TextureH = h;
    m_TextureFormat = format;
    m_PendingUploadSlot = kNoPendingUpload;

    // Persistent RG registration makes the name resolvable for primitive
    // generation and routes the binding through the per-frame publish map
    // (TickRenderRG publishes the current frame's texture). A decoded video
    // frame is authored sRGB in an _SRGB view; stamping it linear would skip
    // the SDR-UI premultiply and dim the video in HDR output.
    if (m_UIManager)
        m_UIManager->SetExternalTextureRG(kVideoPlayerFrameTextureName,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            UI::UITextureSpace::SrgbAuthored(), TextureFormatForVideo(format));
}

void VideoPlayerModal::RetireFrameTexture()
{
    bool anyValid = false;
    for (const auto& slot : m_FrameTextures)
    {
        if (slot.handle.IsValid())
        {
            anyValid = true;
            break;
        }
    }
    if (!anyValid)
        return;

    for (auto& slot : m_FrameTextures)
    {
        if (slot.handle.IsValid() || slot.staging.IsValid())
            m_RetiredTextures.push_back({slot.handle, slot.staging, kRetiredTextureFrames});
        slot = {};
    }
    m_FrameTextureCursor = 0;
    m_TextureW = 0;
    m_TextureH = 0;
    m_PendingUploadSlot = kNoPendingUpload;
}

void VideoPlayerModal::DrainRetiredTextures(bool force)
{
    if (!m_Device)
        return;

    for (auto it = m_RetiredTextures.begin(); it != m_RetiredTextures.end();)
    {
        if (force || --it->framesRemaining <= 0)
        {
            if (it->handle.IsValid())
                m_Device->DestroyTexture(it->handle);
            if (it->staging.IsValid())
                m_Device->DestroyBuffer(it->staging);
            it = m_RetiredTextures.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_RetiredTextures.empty())
    {
        m_RetiredTextures.shrink_to_fit();
    }
}

void VideoPlayerModal::GetWindowOrigin(float& x, float& y) const
{
    x = m_WindowOffsetX;
    y = m_WindowOffsetY;
}

void VideoPlayerModal::PlaceWindow(float x, float y)
{
    m_WindowOffsetX = x;
    m_WindowOffsetY = y;
    if (!m_Window)
    {
        return;
    }
    m_Window->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(m_WindowOffsetX))
        .Set(Style::PositionTop,  StyleLength::Px(m_WindowOffsetY));
    m_Window->MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty);
}

} // namespace GameEngine
