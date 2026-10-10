#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "UI/Controls/DockPanel.h"
#include "Video/VideoPlayer.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/Device.h"

namespace GameEngine {

class UIElement;
class UIManager;
class Label;
class Button;
struct EditorContext;
namespace Rendering { class IDevice; }

// Dockable panel that shows a large preview of the currently selected asset.
// The actual preview image is provided by the shared thumbnail system; this
// panel just displays the resolved thumbnail texture.
class AssetViewPanel : public DockPanel
{
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-asset-view-icon"; }

    AssetViewPanel();
    ~AssetViewPanel() override;

    // Optional editor context (provides thumbnail service and workspace info).
    void SetContext(const EditorContext* ctx) { m_Context = ctx; }

    void SetDevice(Rendering::IDevice* device) { m_Device = device; }
    void SetUIManager(UIManager* uiManager) { m_UIManager = uiManager; }

    // Called by the Assets panel when preview state changes.
    void SetAssetPreview(const std::filesystem::path& path, bool enabled);

    // Call each frame to decode and upload video frames.
    void Update(float dt);

    // Re-register external texture + background after list scroll (no selection change).
    void RefreshVideoPreviewBindingIfActive();

    void OnMountVisibilityChanged(bool isVisible) override;

    struct PreviewDebugState
    {
        std::string previewPath;
        bool previewEnabled = false;
        bool isVideoPreview = false;
        bool videoPlayerLoaded = false;
        bool videoTextureUploaded = false;
        bool videoNeedsBackgroundBind = false;
        int videoTextureWidth = 0;
        int videoTextureHeight = 0;
        std::string videoTextureResourceName;
        std::string displayedEngineResource;
    };
    PreviewDebugState GetPreviewDebugState() const;

    // GPU readback for debug/MCP screenshots of the live video frame.
    bool TryGetVideoPreviewReadbackTexture(Rendering::TextureHandle& outTexture,
                                           Rendering::TextureFormat& outFormat) const;

private:
    void EnsureUi();
    void SchedulePreviewRefresh(int remainingAttempts);
    void UpdatePreviewVisual();
    void ApplyAssetPreviewBackground(const std::string& relOrEngine, uint64_t visualGeneration);
    void UpdateAnimationBadge(const std::string& text);
    void StepAnimation(float deltaSteps);
    void UpdateLensFlareNamePill(bool visible);
    void ToggleImageFilterMode();
    void UpdateFilterModeClass();
    void ShowFilterModePill();
    bool IsHdriExposurePreviewPath(const std::filesystem::path& path) const;
    float GetHdriExposureEVForPath(const std::filesystem::path& path) const;
    void SetHdriExposureEVForPath(const std::filesystem::path& path, float exposureEV);
    void AdjustHdriExposure(float scrollY);
    void StepHdriExposure(float deltaEV);
    bool ApplyHdriExposurePreview(uint64_t visualGeneration);
    void UpdateHdriExposureBadge(bool visible);

    UIElement* m_Root = nullptr;
    UIElement* m_Image = nullptr;
    UIElement* m_AnimBadgeRow = nullptr;
    UIElement* m_AnimBadgeBubble = nullptr;
    Button* m_AnimPrevButton = nullptr;
    Label* m_AnimLabel = nullptr;
    Button* m_AnimNextButton = nullptr;
    UIElement* m_HdriExposureBadgeRow = nullptr;
    UIElement* m_HdriExposureBadgeBubble = nullptr;
    Button* m_HdriExposurePrevButton = nullptr;
    Label* m_HdriExposureLabel = nullptr;
    Button* m_HdriExposureNextButton = nullptr;
    UIElement* m_FilterBadgeRow = nullptr;
    Label* m_FilterBadgeLabel = nullptr;
    UIElement* m_LensFlareNameRow = nullptr;
    Label* m_LensFlareNameLabel = nullptr;

    UIElement* m_ShapeRow = nullptr;

    bool m_IsDragging = false;
    float m_LastDragX = 0.0f;
    float m_LastDragDeltaX = 0.0f;
    float m_LastDragOrbitSign = 0.0f;
    bool m_DidDrag = false;

    const EditorContext* m_Context = nullptr; // not owned
    Rendering::IDevice* m_Device = nullptr;   // not owned
    UIManager* m_UIManager = nullptr;         // not owned

    std::filesystem::path m_PreviewPath;
    bool m_PreviewEnabled = false;
    uint64_t m_PreviewVisualGeneration = 0;
    bool m_PreviewIsModel = false;    // true when showing engine model thumbnail (has animation pill)
    bool m_PreviewIsMaterial = false; // true when showing engine material thumbnail (has shape selector)
    bool m_PreviewIsLensFlare = false; // true when showing the live runtime flare preview
    bool m_IsMounted = false;
    bool m_UsePointFilter = false;
    float m_FilterPillSecondsRemaining = 0.0f;
    std::unordered_map<std::string, float> m_HdriExposureByPath;

    // Engine resource currently bound to `m_Image` (e.g. `engine:editor_material_thumb_<guid>`).
    // Tracked so we can avoid swapping the bound resource until the *new*
    // selection's render is ready — keeps the previous content on screen
    // during the transition instead of going blank for several frames.
    std::string m_DisplayedEngineResource;
    // Engine resource we want to switch to but is not yet ready. Polled
    // each Update() until ready.
    std::string m_PendingEngineResource;

    // Video preview state
    Video::VideoPlayer m_VideoPlayer;
    Rendering::TextureHandle m_VideoTexture;
    Rendering::TextureHandle m_StagingVideoTexture;
    int m_VideoTextureW = 0;
    int m_VideoTextureH = 0;
    Video::VideoPixelFormat m_VideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    int m_StagingVideoTextureW = 0;
    int m_StagingVideoTextureH = 0;
    Video::VideoPixelFormat m_StagingVideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    bool m_StagingVideoTextureUploaded = false;
    bool m_IsVideoPreview = false;
    bool m_VideoTextureUploaded = false;
    bool m_VideoNeedsBackgroundBind = false;
    bool m_ForceStagingVideoUpload = false;
    std::filesystem::path m_VideoPreviewPath;
    std::vector<uint8_t> m_VideoPixelScratch;
    // Unique per panel so main Asset View and scene-tab copy do not fight over one slot.
    std::string m_VideoTextureResourceName;

    struct RetiredVideoTexture
    {
        Rendering::TextureHandle handle;
        int framesRemaining = 0;
    };
    std::vector<RetiredVideoTexture> m_RetiredVideoTextures;
    static constexpr int kRetiredVideoTextureFrames = 4;

    // Video interaction
    float m_VideoScrubStartX = 0.0f;
    float m_VideoScrubStartTime = 0.0f;
    std::chrono::steady_clock::time_point m_VideoLastClickTime{};

    static bool IsVideoPath(const std::filesystem::path& p);
    void StartVideoPreview(const std::filesystem::path& path);
    void StopVideoPreview();
    void LeaveVideoPreview();
    void ReleaseVideoPreviewGpuResources();
    void StopVideoDecoder();
    void EnsureStagingVideoTexture(int w, int h, Video::VideoPixelFormat format);
    void PromoteStagingVideoTexture(int w, int h, Video::VideoPixelFormat format);
    void DiscardStagingVideoTexture();
    void RetireVideoTexture(Rendering::TextureHandle handle);
    void DrainRetiredVideoTextures(bool force = false);
    void BindVideoPreviewBackground(bool forceUiRegen = false);
    void EnsureVideoPreviewBackgroundBound(bool forceUiRegen = false);
    bool UploadVideoFrameIfReady();
};

} // namespace GameEngine
