#pragma once

#include "UI/Controls/DraggableModal.h"
#include "Video/VideoPlayer.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Rendering::RenderGraph { class RGFrame; }

namespace GameEngine
{

class UIManager;
class Button;
class Label;

// Modal dialog that plays an .mp4 video file.
// The decoded frames are uploaded to a GPU texture each frame and displayed
// as an engine-named background-image ("video_player_frame") inside the modal.
//
// Usage:
//   modal->Open("/path/to/video.mp4");
//   modal->SetOnClose([](){ ... });
//   // call modal->Update(dt) each frame from the editor update loop
class VideoPlayerModal : public DraggableModal
{
public:
    VideoPlayerModal();
    ~VideoPlayerModal() override;

    void SetDevice(Rendering::IDevice* device)   { m_Device = device; }
    void SetUIManager(UIManager* uiManager)      { m_UIManager = uiManager; }

    // Open the modal and start playing the given file immediately.
    // If title is non-empty it overrides the filename-derived header text.
    void Open(const std::string& videoPath, const std::string& title = {});
    void Close();
    bool IsOpen() const { return m_Open; }

    // Decode the next frame and stage its pixels for upload. Call every frame
    // from the host update loop, before the render-graph frame is declared.
    void Update();

    // Declare the staged-frame upload copy inside the host's render-graph frame
    // and publish the texture for the UI to sample. Routing the copy through the
    // graph (rather than an out-of-band submit) lets the graph order it before
    // the UI samples the texture the same frame. Call after Update, once per
    // render-graph frame, while the frame is being declared.
    void TickRenderRG(Rendering::RenderGraph::RGFrame& frame);

    void SetOnClose(std::function<void()> cb) { m_OnClose = std::move(cb); }

private:
    struct RetiredTexture
    {
        Rendering::TextureHandle handle;
        Rendering::BufferHandle staging;
        int framesRemaining = 0;
    };
    struct FrameTextureSlot
    {
        Rendering::TextureHandle handle;
        Rendering::BufferHandle staging;
        size_t stagingSize = 0;
        bool uploaded = false; // texture holds a copied frame (ShaderResource)
    };

    void BuildUI();
    void TogglePlayback();
    void StopPlayback();
    void RevealVideoInFileManager() const;
    bool IsTitleTextHit(float x, float y) const;
    void SetFullscreen(bool fullscreen);
    void ToggleFullscreen();
    void TriggerFullscreenButton();
    void UpdateControlState();
    void ResetFrameTextureBinding();
    void EnsureTexture(int w, int h, Video::VideoPixelFormat format);
    void RetireFrameTexture();
    void DrainRetiredTextures(bool force = false);
    // DraggableModal: the window rests flex-centred in the backdrop; the origin
    // is the position offset from that rest.
    void GetWindowOrigin(float& x, float& y) const override;
    void PlaceWindow(float x, float y) override;

    Rendering::IDevice* m_Device  = nullptr; // not owned
    UIManager*          m_UIManager = nullptr; // not owned

    Video::VideoPlayer m_Player;

    static constexpr size_t kFrameTextureRingSize = 3;
    std::array<FrameTextureSlot, kFrameTextureRingSize> m_FrameTextures{};
    size_t m_FrameTextureCursor = 0;
    int m_TextureW = 0;
    int m_TextureH = 0;
    Video::VideoPixelFormat m_TextureFormat = Video::VideoPixelFormat::RGBA8;
    std::vector<RetiredTexture> m_RetiredTextures;

    // Slot staged this frame in Update, awaiting its render-graph copy pass.
    // kNoPendingUpload when nothing was decoded this frame.
    static constexpr size_t kNoPendingUpload = ~size_t{0};
    size_t m_PendingUploadSlot = kNoPendingUpload;
    uint32_t m_PendingUploadWidth = 0;
    uint32_t m_PendingUploadHeight = 0;
    size_t m_PendingUploadRowPitch = 0;

    bool m_Open = false;
    std::string m_Title;
    std::string m_VideoPath;

    // Child element pointers (not owned — UIElement tree owns them)
    UIElement* m_Backdrop   = nullptr;
    UIElement* m_Window     = nullptr;
    UIElement* m_VideoArea  = nullptr;
    Label*     m_TitleLabel = nullptr;
    Button*    m_PlayPause  = nullptr;
    Button*    m_StopBtn    = nullptr;
    Button*    m_FullscreenBtn = nullptr;
    Button*    m_CloseBtn   = nullptr;
    UIElement* m_SeekBar    = nullptr;
    UIElement* m_SeekFill   = nullptr;
    Label*     m_TimeLabel  = nullptr;

    bool m_SeekDragging = false;
    bool m_Fullscreen = false;

    float m_WindowOffsetX = 0.0f;
    float m_WindowOffsetY = 0.0f;
    float m_PreFullscreenOffsetX = 0.0f;
    float m_PreFullscreenOffsetY = 0.0f;
    std::chrono::steady_clock::time_point m_LastVideoClickTime{};
    float m_LastVideoClickX = 0.0f;
    float m_LastVideoClickY = 0.0f;

    std::function<void()> m_OnClose;
};

} // namespace GameEngine
