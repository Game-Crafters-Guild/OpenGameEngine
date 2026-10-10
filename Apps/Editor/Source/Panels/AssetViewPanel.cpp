#include "Panels/AssetViewPanel.h"

#include "Editor/EditorPaths.h"
#include "EditorContext.h"
#include "Logger/Logger.h"
#include "Platform/SystemMetrics.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "Input/InputSystem.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Types/StringUtils.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace GameEngine {

namespace
{
constexpr float kAnimationBadgeWidthPx = 300.0f;
constexpr float kAnimationArrowIdleOpacity = 0.28f;
constexpr float kAnimationArrowHoverOpacity = 1.0f;

constexpr const char* kEnginePrefixForReady = "engine:";
constexpr size_t kEnginePrefixForReadyLen = 7;
constexpr float kHdriExposureMinEV = -8.0f;
constexpr float kHdriExposureMaxEV = 8.0f;
constexpr float kHdriExposureScrollStepEV = 0.25f;
// Longest the preview blocks for a newly opened video's first decoded frame.
constexpr float kVideoFirstFrameTimeoutSeconds = 1.0f;

// Strips an optional `engine:` prefix so we can ask the thumbnail provider
// whether the underlying GPU resource is bound for UI sampling.
std::string ResourceNameWithoutEnginePrefix(const std::string& s)
{
    if (s.rfind(kEnginePrefixForReady, 0) == 0)
        return s.substr(kEnginePrefixForReadyLen);
    return s;
}

Rendering::TextureFormat TextureFormatForVideo(Video::VideoPixelFormat format)
{
    return format == Video::VideoPixelFormat::BGRA8
        ? Rendering::TextureFormat::BGRA8_SRGB
        : Rendering::TextureFormat::RGBA8_SRGB;
}

bool UploadVideoFrame(Rendering::IDevice* device,
                      Rendering::TextureHandle texture,
                      const void* pixels,
                      uint32_t width,
                      uint32_t height,
                      size_t rowPitchBytes,
                      bool alreadyShaderReadable)
{
    if (!device || !texture.IsValid() || !pixels || width == 0 || height == 0 || rowPitchBytes == 0)
        return false;

    const bool isD3D12 = device->GetAPI() == Rendering::GraphicsAPI::DirectX12;
    size_t uploadRowPitch = rowPitchBytes;
    std::vector<unsigned char> padded;
    const void* uploadPixels = pixels;
    if (isD3D12)
    {
        constexpr size_t kD3D12TextureDataPitchAlignment = 256;
        uploadRowPitch = (rowPitchBytes + kD3D12TextureDataPitchAlignment - 1u) & ~(kD3D12TextureDataPitchAlignment - 1u);
        if (uploadRowPitch != rowPitchBytes)
        {
            padded.assign(static_cast<size_t>(height) * uploadRowPitch, 0);
            const auto* src = static_cast<const unsigned char*>(pixels);
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(padded.data() + static_cast<size_t>(y) * uploadRowPitch,
                            src + static_cast<size_t>(y) * rowPitchBytes,
                            rowPitchBytes);
            uploadPixels = padded.data();
        }
    }

    Rendering::BufferHandle staging = device->CreateUploadBuffer(static_cast<size_t>(height) * uploadRowPitch, "AssetViewVideoUpload");
    if (!staging.IsValid())
        return false;
    device->UpdateBuffer(staging, 0, static_cast<size_t>(height) * uploadRowPitch, uploadPixels);

    auto cl = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        texture,
        alreadyShaderReadable ? Rendering::ResourceState::ShaderResource : Rendering::ResourceState::Undefined,
        Rendering::ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(staging, texture, 0, 0, width, height, 0, uploadRowPitch);
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        texture,
        Rendering::ResourceState::CopyDest,
        Rendering::ResourceState::ShaderResource));
    cl->End();
    Rendering::CommandList* raw = cl.get();
    device->ExecuteCommandLists({raw});
    device->DestroyBuffer(staging);
    return true;
}

bool VideoPreviewPathsEqual(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty())
        return a.empty() && b.empty();
    if (a == b)
        return true;
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec);
}

bool IsHdrImageExtension(const std::filesystem::path& p)
{
    const auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".hdr";
}

std::string FormatExposureEV(float exposureEV)
{
    if (std::fabs(exposureEV) < 0.005f)
        exposureEV = 0.0f;

    std::ostringstream oss;
    if (exposureEV > 0.0f)
        oss << '+';
    oss << std::fixed << std::setprecision(2) << exposureEV << " EV";
    return oss.str();
}

std::filesystem::path MakeHdriExposurePreviewPath(const std::filesystem::path& assetPath,
                                                   int desiredSize,
                                                   float exposureEV)
{
    const auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.thumbnailsRoot.empty())
        return {};

    std::error_code ec;
    const auto abs = std::filesystem::absolute(assetPath, ec);
    const auto canonical = ec ? assetPath.lexically_normal() : abs.lexically_normal();
    const auto writeTime = std::filesystem::last_write_time(assetPath, ec);
    const auto writeTicks = ec ? 0ll : static_cast<long long>(writeTime.time_since_epoch().count());
    const auto fileSize = std::filesystem::file_size(assetPath, ec);
    const auto sizeBytes = ec ? 0ull : static_cast<unsigned long long>(fileSize);
    const int exposureTicks = static_cast<int>(std::lround(std::clamp(exposureEV, kHdriExposureMinEV, kHdriExposureMaxEV) * 100.0f));

    const std::string key = canonical.generic_string() + "|" +
        std::to_string(writeTicks) + "|" +
        std::to_string(sizeBytes) + "|" +
        std::to_string(std::max(64, desiredSize)) + "|" +
        std::to_string(exposureTicks);
    const std::size_t hash = std::hash<std::string>{}(key);
    return projectPaths.thumbnailsRoot / "HDRIExposure" /
        ("hdripreview_" + std::to_string(hash) + ".png");
}

bool GenerateHdriExposurePreview(const std::filesystem::path& assetPath,
                                 const std::filesystem::path& cachePath,
                                 int desiredSize,
                                 float exposureEV)
{
    std::error_code ec;
    if (cachePath.empty())
        return false;
    if (std::filesystem::exists(cachePath, ec))
        return true;

    int srcW = 0;
    int srcH = 0;
    int srcComp = 0;
    float* src = stbi_loadf(assetPath.string().c_str(), &srcW, &srcH, &srcComp, 3);
    if (!src || srcW <= 0 || srcH <= 0)
    {
        if (src)
            stbi_image_free(src);
        Logger::Log::Warning("AssetViewPanel: failed to load HDRI preview '{}'", assetPath.string());
        return false;
    }

    const int maxDim = std::max(srcW, srcH);
    const int targetMax = std::clamp(desiredSize, 128, 2048);
    const float scale = std::min(1.0f, static_cast<float>(targetMax) / static_cast<float>(maxDim));
    const int outW = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcW) * scale)));
    const int outH = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcH) * scale)));

    std::vector<unsigned char> out(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4u, 255u);
    const float exposure = std::pow(2.0f, std::clamp(exposureEV, kHdriExposureMinEV, kHdriExposureMaxEV));

    auto sample = [&](int x, int y, int c) -> float
    {
        x = std::clamp(x, 0, srcW - 1);
        y = std::clamp(y, 0, srcH - 1);
        return src[(static_cast<size_t>(y) * static_cast<size_t>(srcW) + static_cast<size_t>(x)) * 3u + static_cast<size_t>(c)];
    };

    for (int y = 0; y < outH; ++y)
    {
        const float srcY = (static_cast<float>(y) + 0.5f) * static_cast<float>(srcH) / static_cast<float>(outH) - 0.5f;
        const int y0 = static_cast<int>(std::floor(srcY));
        const int y1 = y0 + 1;
        const float fy = srcY - static_cast<float>(y0);
        for (int x = 0; x < outW; ++x)
        {
            const float srcX = (static_cast<float>(x) + 0.5f) * static_cast<float>(srcW) / static_cast<float>(outW) - 0.5f;
            const int x0 = static_cast<int>(std::floor(srcX));
            const int x1 = x0 + 1;
            const float fx = srcX - static_cast<float>(x0);
            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(outW) + static_cast<size_t>(x)) * 4u;
            for (int c = 0; c < 3; ++c)
            {
                const float a = sample(x0, y0, c) * (1.0f - fx) + sample(x1, y0, c) * fx;
                const float b = sample(x0, y1, c) * (1.0f - fx) + sample(x1, y1, c) * fx;
                const float linear = std::max(0.0f, (a * (1.0f - fy) + b * fy) * exposure);
                const float tonemapped = 1.0f - std::exp(-linear);
                const float srgb = std::pow(std::clamp(tonemapped, 0.0f, 1.0f), 1.0f / 2.2f);
                out[dst + static_cast<size_t>(c)] = static_cast<unsigned char>(std::lround(srgb * 255.0f));
            }
        }
    }

    stbi_image_free(src);

    std::filesystem::create_directories(cachePath.parent_path(), ec);
    if (ec)
    {
        Logger::Log::Warning("AssetViewPanel: failed to create HDRI preview cache '{}': {}",
                             cachePath.parent_path().string(), ec.message());
        return false;
    }

    const bool ok = stbi_write_png(cachePath.string().c_str(), outW, outH, 4, out.data(), outW * 4) != 0;
    if (!ok)
        Logger::Log::Warning("AssetViewPanel: failed to write HDRI preview '{}'", cachePath.string());
    return ok;
}
} // namespace

AssetViewPanel::AssetViewPanel()
    : DockPanel("Asset View")
    , m_VideoTextureResourceName("asset_view_video_" + std::to_string(reinterpret_cast<uintptr_t>(this)))
{
    EnsureUi();
}

AssetViewPanel::~AssetViewPanel()
{
    DrainRetiredVideoTextures(true);
    StopVideoPreview();
}

void AssetViewPanel::EnsureUi()
{
    if (m_Root)
        return;

    auto root = std::make_unique<UIElement>();
    root->AddClass("asset-view-panel-root");
    root->Overrides()
        .Set(Style::Position, PositionType::Relative)
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f));

    auto image = std::make_unique<UIElement>();
    image->SetId("asset-view-preview-image");
    image->AddClass("asset-view-image");
    image->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
        // This preview shows the raw texture, not a 9-sliced UI background, so the
        // previewed asset's own Sliced import setting must not lay it out. An
        // all-zero border-image slice replaces that intrinsic slice with none,
        // which keeps the image on the BackgroundSize::Contain path above.
        .Set(Style::BorderImageSlice, BorderImageSliceValue{0.0f, 0.0f, 0.0f, 0.0f, true});

    image->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
    {
        if (e.Button != 0) return;
        m_IsDragging = true;
        m_DidDrag = false;
        m_LastDragX = e.X;
        m_LastDragDeltaX = 0.0f;
        e.Capture(m_Image);
        if (m_IsVideoPreview)
        {
            m_VideoScrubStartX    = e.X;
            m_VideoScrubStartTime = m_VideoPlayer.GetCurrentTime();
        }
        e.Stop();
    });

    image->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
    {
        if (!m_IsDragging) return;
        const float dx = e.X - m_LastDragX;
        m_LastDragX      = e.X;
        m_LastDragDeltaX = dx;
        if (std::fabs(dx) > 1e-3f)
            m_DidDrag = true;

        if (m_IsVideoPreview)
        {
            const float panelW = m_Image->GetLayoutWidth();
            const float dur    = m_VideoPlayer.GetDuration();
            if (panelW > 0.0f && dur > 0.0f)
            {
                const float totalDx  = e.X - m_VideoScrubStartX;
                const float seekTime = std::clamp(
                    m_VideoScrubStartTime + totalDx / panelW * dur,
                    0.0f, dur);
                m_VideoPlayer.Seek(seekTime);
            }
        }
        else if (m_PreviewIsModel || m_PreviewIsMaterial || m_PreviewIsLensFlare)
        {
            constexpr float kSensitivity = 0.0025f;
            const float scale = ModelThumbnailHandler::GetOrbitSpeedScale();
            ModelThumbnailHandler::SetOrbitVelocity(-dx * kSensitivity * scale);
            if (std::fabs(dx) > 1e-3f)
                m_LastDragOrbitSign = dx > 0.0f ? -1.0f : 1.0f;
        }
        e.Stop();
    });

    image->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0) return;

        if (m_IsVideoPreview)
        {
            if (!m_DidDrag)
            {
                auto now = std::chrono::steady_clock::now();
                if ((now - m_VideoLastClickTime) < GameEngine::Platform::GetDoubleClickInterval())
                {
                    // Double-click: rewind to start and play.
                    m_VideoPlayer.Seek(0.0f);
                    m_VideoPlayer.Play();
                }
                else
                {
                    // Single click: toggle play/pause.
                    if (m_VideoPlayer.IsPlaying())
                        m_VideoPlayer.Pause();
                    else
                        m_VideoPlayer.Play();
                    m_VideoLastClickTime = now;
                }
            }
            m_IsDragging = false;
            e.Stop();
            return;
        }

        if (!m_PreviewIsModel && !m_PreviewIsMaterial && !m_PreviewIsLensFlare)
        {
            if (!m_DidDrag && !IsHdriExposurePreviewPath(m_PreviewPath))
                ToggleImageFilterMode();
            m_IsDragging = false;
            e.Stop();
            return;
        }

        // Engine preview: plain click pauses rotation; drag restarts it.
        if (!m_DidDrag)
        {
            ModelThumbnailHandler::PauseClickRotation();
            m_IsDragging = false;
            e.Stop();
            return;
        }

        m_IsDragging = false;
        constexpr float kReleaseMovementEpsilon = 1e-3f;
        if (std::fabs(m_LastDragDeltaX) <= kReleaseMovementEpsilon)
        {
            ModelThumbnailHandler::SetOrbitVelocity(0.0f);
            e.Stop();
            return;
        }

        if (ModelThumbnailHandler::GetRotatePreviewsEnabled())
        {
            constexpr float kDefaultSpeed = 0.02f;
            const float sign  = (std::fabs(m_LastDragOrbitSign) > 1e-3f) ? m_LastDragOrbitSign : 1.0f;
            const float scale = ModelThumbnailHandler::GetOrbitSpeedScale();
            ModelThumbnailHandler::EaseToOrbitVelocity(sign * kDefaultSpeed * scale);
        }
        else
        {
            ModelThumbnailHandler::SetOrbitVelocity(0.0f);
        }
        e.Stop();
    });

    image->RegisterEventHandler(kEventScroll, [this](UIEvent& e)
    {
        if (std::fabs(e.ScrollY) < 1e-3f) return;

        if (m_IsVideoPreview)
        {
            constexpr float kScrollSecondsPerStep = 0.1f;
            const float delta = (e.ScrollY > 0.0f ? 1.0f : -1.0f) * kScrollSecondsPerStep;
            const float seekTime = std::clamp(
                m_VideoPlayer.GetCurrentTime() + delta,
                0.0f, m_VideoPlayer.GetDuration());
            m_VideoPlayer.Seek(seekTime);
            (void)UploadVideoFrameIfReady();
            EnsureVideoPreviewBackgroundBound();
            e.Stop();
            return;
        }

        if (IsHdriExposurePreviewPath(m_PreviewPath))
        {
            AdjustHdriExposure(e.ScrollY);
            e.Stop();
            return;
        }

        const bool is3DPreview = m_PreviewIsModel || m_PreviewIsMaterial || m_PreviewIsLensFlare;
        if (!is3DPreview)
        {
            UpdateAnimationBadge("");
            e.Stop();
            return;
        }

        // Scroll bindings come from Settings → Shortcuts (Asset Preview). Cycle
        // Animation wins when both match the same mods (shouldn't happen after
        // conflict resolution).
        const int scrollMods = e.Mods & Input::kModShortcutMask;
        if (Editor::AssetPreviewScrollMatchesCycleAnimation(scrollMods) &&
            m_PreviewIsModel && !m_PreviewIsMaterial)
        {
            StepAnimation((e.ScrollY > 0.0f) ? 1.0f : -1.0f);
            e.Stop();
            return;
        }

        if ((m_PreviewIsModel || m_PreviewIsMaterial) &&
            ModelThumbnailHandler::GetPreviewScrollZoomEnabled() &&
            Editor::AssetPreviewScrollMatchesDolly(scrollMods))
        {
            ModelThumbnailHandler::AdjustFocusedPreviewZoom(e.ScrollY);
        }
        e.Stop();
    });

    m_Image = image.get();
    root->AddChild(std::move(image));

    auto animBadgeRow = std::make_unique<UIElement>();
    animBadgeRow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionRight, StyleLength::Px(0.0f))
        .Set(Style::PositionBottom, StyleLength::Px(12.0f))
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center);
    m_AnimBadgeRow = animBadgeRow.get();

    auto animBadgeBubble = std::make_unique<UIElement>();
    animBadgeBubble->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Width, StyleLength::Px(kAnimationBadgeWidthPx))
        .Set(Style::Height, StyleLength::Px(28.0f))
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(8.0f))
        .Set(Style::PaddingRight, StyleLength::Px(8.0f))
        .Set(Style::BackgroundColor, 0xFF1C1C1Du)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{14.0f, 14.0f, 14.0f, 14.0f});
    m_AnimBadgeBubble = animBadgeBubble.get();
    // The pill is a dedicated control, so it steps on a plain wheel; the
    // Asset Preview shortcut modifier applies only to scrolling over the image.
    animBadgeBubble->RegisterEventHandler(kEventScroll, [this](UIEvent& e) {
        if (std::fabs(e.ScrollY) < 1e-3f) return;
        if (ModelThumbnailHandler::GetFocusedAnimationCount() <= 1) return;
        StepAnimation((e.ScrollY > 0.0f) ? 1.0f : -1.0f);
        e.Stop();
    });

    auto makeAnimStepButton = [this](const char* iconPath, const char* tooltip, float deltaSteps) {
        auto button = std::make_unique<Button>();
        button->AddClass("icon-button");
        button->AddClass("inspector-header-history-icon");
        button->AddClass("asset-view-anim-arrow");
        button->SetText("");
        button->SetTooltip(tooltip);
        button->Overrides()
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::Height, StyleLength::Px(24.0f))
            .Set(Style::MinWidth, StyleLength::Px(24.0f))
            .Set(Style::MinHeight, StyleLength::Px(24.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(0.0f))
            .Set(Style::PaddingRight, StyleLength::Px(0.0f))
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::BackgroundColor, 0x00000000u)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f})
            .Set(Style::Opacity, kAnimationArrowIdleOpacity)
            .Set(Style::Cursor, CursorStyle::Pointer);
        UI::Layout::SetBackgroundPath(*button, iconPath);
        button->RegisterEventHandler(kEventMouseEnter, [](UIEvent& e) {
            if (auto* target = e.CurrentTarget)
            {
                target->Overrides().Set(Style::Opacity, kAnimationArrowHoverOpacity);
                target->MarkDirty(UIElement::VisualDirty);
            }
        });
        button->RegisterEventHandler(kEventMouseLeave, [](UIEvent& e) {
            if (auto* target = e.CurrentTarget)
            {
                target->Overrides().Set(Style::Opacity, kAnimationArrowIdleOpacity);
                target->MarkDirty(UIElement::VisualDirty);
            }
        });
        button->SetOnMouseDown([](Button& b) {
            b.Overrides().Set(Style::Opacity, kAnimationArrowHoverOpacity);
            b.MarkDirty(UIElement::VisualDirty);
        });
        button->RegisterEventHandler(kEventButtonClick, [this, deltaSteps](UIEvent&) {
            StepAnimation(deltaSteps);
        });
        return button;
    };

    auto prevAnimButton = makeAnimStepButton("Icons/left-arrow.png", "Previous Animation", -1.0f);
    m_AnimPrevButton = prevAnimButton.get();
    animBadgeBubble->AddChild(std::move(prevAnimButton));

    auto animLabel = std::make_unique<Label>();
    animLabel->SetText("");
    animLabel->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::FlexBasis, StyleLength::Px(0.0f))
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::Color, 0xFFFFFFFFu)
        .Set(Style::FontSize, StyleLength::Px(13.0f))
        .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap)
        .Set(Style::TextAlignProp, TextAlign::Center)
        .Set(Style::MarginTop, StyleLength::Px(0.0f))
        .Set(Style::MarginRight, StyleLength::Px(0.0f))
        .Set(Style::MarginBottom, StyleLength::Px(0.0f))
        .Set(Style::MarginLeft, StyleLength::Px(0.0f));
    m_AnimLabel = animLabel.get();

    animBadgeBubble->AddChild(std::move(animLabel));

    auto nextAnimButton = makeAnimStepButton("Icons/right-arrow.png", "Next Animation", 1.0f);
    m_AnimNextButton = nextAnimButton.get();
    animBadgeBubble->AddChild(std::move(nextAnimButton));

    animBadgeRow->AddChild(std::move(animBadgeBubble));
    root->AddChild(std::move(animBadgeRow));

    auto hdriExposureRow = std::make_unique<UIElement>();
    hdriExposureRow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionRight, StyleLength::Px(0.0f))
        .Set(Style::PositionBottom, StyleLength::Px(12.0f))
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center);
    m_HdriExposureBadgeRow = hdriExposureRow.get();

    auto hdriExposureBubble = std::make_unique<UIElement>();
    hdriExposureBubble->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Width, StyleLength::Px(kAnimationBadgeWidthPx))
        .Set(Style::Height, StyleLength::Px(28.0f))
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(8.0f))
        .Set(Style::PaddingRight, StyleLength::Px(8.0f))
        .Set(Style::BackgroundColor, 0xFF1C1C1Du)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{14.0f, 14.0f, 14.0f, 14.0f});
    m_HdriExposureBadgeBubble = hdriExposureBubble.get();

    auto makeHdriExposureButton = [this](const char* iconPath, const char* tooltip, float deltaEV) {
        auto button = std::make_unique<Button>();
        button->AddClass("icon-button");
        button->AddClass("inspector-header-history-icon");
        button->AddClass("asset-view-anim-arrow");
        button->SetText("");
        button->SetTooltip(tooltip);
        button->Overrides()
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::Height, StyleLength::Px(24.0f))
            .Set(Style::MinWidth, StyleLength::Px(24.0f))
            .Set(Style::MinHeight, StyleLength::Px(24.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(0.0f))
            .Set(Style::PaddingRight, StyleLength::Px(0.0f))
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::BackgroundColor, 0x00000000u)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f})
            .Set(Style::Opacity, kAnimationArrowIdleOpacity)
            .Set(Style::Cursor, CursorStyle::Pointer);
        UI::Layout::SetBackgroundPath(*button, iconPath);
        button->RegisterEventHandler(kEventMouseEnter, [](UIEvent& e) {
            if (auto* target = e.CurrentTarget)
            {
                target->Overrides().Set(Style::Opacity, kAnimationArrowHoverOpacity);
                target->MarkDirty(UIElement::VisualDirty);
            }
        });
        button->RegisterEventHandler(kEventMouseLeave, [](UIEvent& e) {
            if (auto* target = e.CurrentTarget)
            {
                target->Overrides().Set(Style::Opacity, kAnimationArrowIdleOpacity);
                target->MarkDirty(UIElement::VisualDirty);
            }
        });
        button->SetOnMouseDown([](Button& b) {
            b.Overrides().Set(Style::Opacity, kAnimationArrowHoverOpacity);
            b.MarkDirty(UIElement::VisualDirty);
        });
        button->RegisterEventHandler(kEventButtonClick, [this, deltaEV](UIEvent&) {
            StepHdriExposure(deltaEV);
        });
        return button;
    };

    auto prevExposureButton = makeHdriExposureButton("Icons/left-arrow.png", "Lower Exposure", -kHdriExposureScrollStepEV);
    m_HdriExposurePrevButton = prevExposureButton.get();
    hdriExposureBubble->AddChild(std::move(prevExposureButton));

    auto exposureLabel = std::make_unique<Label>();
    exposureLabel->SetText("");
    exposureLabel->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::FlexBasis, StyleLength::Px(0.0f))
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::Color, 0xFFFFFFFFu)
        .Set(Style::FontSize, StyleLength::Px(13.0f))
        .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap)
        .Set(Style::TextAlignProp, TextAlign::Center)
        .Set(Style::MarginTop, StyleLength::Px(0.0f))
        .Set(Style::MarginRight, StyleLength::Px(0.0f))
        .Set(Style::MarginBottom, StyleLength::Px(0.0f))
        .Set(Style::MarginLeft, StyleLength::Px(0.0f));
    m_HdriExposureLabel = exposureLabel.get();
    hdriExposureBubble->AddChild(std::move(exposureLabel));

    auto nextExposureButton = makeHdriExposureButton("Icons/right-arrow.png", "Raise Exposure", kHdriExposureScrollStepEV);
    m_HdriExposureNextButton = nextExposureButton.get();
    hdriExposureBubble->AddChild(std::move(nextExposureButton));

    hdriExposureRow->AddChild(std::move(hdriExposureBubble));
    root->AddChild(std::move(hdriExposureRow));

    auto filterBadgeRow = std::make_unique<UIElement>();
    filterBadgeRow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionRight, StyleLength::Px(0.0f))
        .Set(Style::PositionBottom, StyleLength::Px(12.0f))
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Opacity, 0.0f)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, false);
    m_FilterBadgeRow = filterBadgeRow.get();

    auto filterBadge = std::make_unique<UIElement>();
    filterBadge->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Width, StyleLength::Px(160.0f))
        .Set(Style::Height, StyleLength::Px(24.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
        .Set(Style::PaddingRight, StyleLength::Px(12.0f))
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::BackgroundColor, 0xDD111113u)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{12.0f, 12.0f, 12.0f, 12.0f});

    auto filterLabel = std::make_unique<Label>();
    filterLabel->SetText("Bilinear");
    filterLabel->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Color, 0xFFFFFFFFu)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap)
        .Set(Style::TextAlignProp, TextAlign::Center)
        .Set(Style::MarginTop, StyleLength::Px(0.0f))
        .Set(Style::MarginRight, StyleLength::Px(0.0f))
        .Set(Style::MarginBottom, StyleLength::Px(0.0f))
        .Set(Style::MarginLeft, StyleLength::Px(0.0f));
    m_FilterBadgeLabel = filterLabel.get();

    filterBadge->AddChild(std::move(filterLabel));
    filterBadgeRow->AddChild(std::move(filterBadge));
    root->AddChild(std::move(filterBadgeRow));

    auto lensFlareNameRow = std::make_unique<UIElement>();
    lensFlareNameRow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionRight, StyleLength::Px(0.0f))
        .Set(Style::PositionBottom, StyleLength::Px(12.0f))
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, false);
    m_LensFlareNameRow = lensFlareNameRow.get();

    auto lensFlareNamePill = std::make_unique<UIElement>();
    lensFlareNamePill->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Height, StyleLength::Px(24.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
        .Set(Style::PaddingRight, StyleLength::Px(12.0f))
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::BackgroundColor, 0xDD111113u)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{12.0f, 12.0f, 12.0f, 12.0f});

    auto lensFlareNameLabel = std::make_unique<Label>();
    lensFlareNameLabel->Overrides()
        .Set(Style::Color, 0xFFFFFFFFu)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap)
        .Set(Style::TextAlignProp, TextAlign::Center)
        .Set(Style::MarginTop, StyleLength::Px(0.0f))
        .Set(Style::MarginRight, StyleLength::Px(0.0f))
        .Set(Style::MarginBottom, StyleLength::Px(0.0f))
        .Set(Style::MarginLeft, StyleLength::Px(0.0f));
    m_LensFlareNameLabel = lensFlareNameLabel.get();

    lensFlareNamePill->AddChild(std::move(lensFlareNameLabel));
    lensFlareNameRow->AddChild(std::move(lensFlareNamePill));
    root->AddChild(std::move(lensFlareNameRow));

    // Shape selector row for material previews (sphere / cube / plane / capsule).
    auto shapeRow = std::make_unique<UIElement>();
    shapeRow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionRight, StyleLength::Px(0.0f))
        .Set(Style::PositionBottom, StyleLength::Px(12.0f))
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f));
    m_ShapeRow = shapeRow.get();

    struct ShapeBtn { const char* label; ModelThumbnailHandler::PreviewShape shape; };
    constexpr ShapeBtn kShapes[] = {
        { "Sphere",  ModelThumbnailHandler::PreviewShape::Sphere  },
        { "Cube",    ModelThumbnailHandler::PreviewShape::Cube    },
        { "Plane",   ModelThumbnailHandler::PreviewShape::Plane   },
    };

    for (const auto& btn : kShapes)
    {
        auto pill = std::make_unique<UIElement>();
        pill->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::Height, StyleLength::Px(24.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(10.0f))
            .Set(Style::PaddingRight, StyleLength::Px(10.0f))
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center)
            .Set(Style::BackgroundColor, 0xCC1C1C1Du)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{12.0f, 12.0f, 12.0f, 12.0f})
            .Set(Style::Cursor, CursorStyle::Pointer);

        auto lbl = std::make_unique<Label>();
        lbl->SetText(btn.label);
        lbl->Overrides()
            .Set(Style::Color, 0xFFFFFFFFu)
            .Set(Style::FontSize, StyleLength::Px(11.0f))
            .Set(Style::MarginTop, StyleLength::Px(0.0f))
            .Set(Style::MarginRight, StyleLength::Px(0.0f))
            .Set(Style::MarginBottom, StyleLength::Px(0.0f))
            .Set(Style::MarginLeft, StyleLength::Px(0.0f));

        const ModelThumbnailHandler::PreviewShape shapeCapture = btn.shape;
        pill->RegisterEventHandler(kEventMouseUp, [shapeCapture](UIEvent& e)
        {
            if (e.Button != 0) return;
            ModelThumbnailHandler::SetMaterialPreviewShape(shapeCapture);
            e.Stop();
        });

        pill->AddChild(std::move(lbl));
        shapeRow->AddChild(std::move(pill));
    }

    root->AddChild(std::move(shapeRow));

    m_Root = root.get();
    AddChild(std::move(root));
}

// ── Video helpers ────────────────────────────────────────────────────────────

/*static*/ bool AssetViewPanel::IsVideoPath(const std::filesystem::path& p)
{
    auto ext = ToLowerAscii(p.has_extension() ? p.extension().string() : std::string());
    return ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mkv" || ext == ".m4v";
}

void AssetViewPanel::StartVideoPreview(const std::filesystem::path& path)
{
    if (m_IsVideoPreview && m_VideoPlayer.IsLoaded() && VideoPreviewPathsEqual(m_VideoPreviewPath, path))
    {
        if (m_VideoTextureUploaded && m_VideoTexture.IsValid())
            BindVideoPreviewBackground(/*forceUiRegen=*/true);
        else if (UploadVideoFrameIfReady())
            BindVideoPreviewBackground(/*forceUiRegen=*/true);
        return;
    }

    if (!m_IsMounted || !m_Device || !m_UIManager)
        return;

    // Decoder-only stop: keep the last uploaded GPU frame visible while the new clip loads.
    StopVideoDecoder();
    DiscardStagingVideoTexture();
    m_ForceStagingVideoUpload = true;
    m_VideoNeedsBackgroundBind = true;

    m_IsVideoPreview = true;
    m_PreviewIsModel = false;
    m_PreviewIsMaterial = false;
    m_PreviewIsLensFlare = false;
    UpdateLensFlareNamePill(false);
    m_PendingEngineResource.clear();
    m_DisplayedEngineResource.clear();
    ModelThumbnailHandler::ClearOrbitFocus();
    ModelThumbnailHandler::ClearMaterialOrbitFocus();
    ModelThumbnailHandler::ClearLensFlareFocus();
    UpdateFilterModeClass();
    if (m_FilterBadgeRow)
        m_FilterBadgeRow->Overrides().Set(Style::Opacity, 0.0f);
    UpdateHdriExposureBadge(false);
    m_VideoPreviewPath = path;
    m_VideoPlayer.SetLoop(true);
    if (m_VideoPlayer.Load(path.string()))
    {
        m_VideoPlayer.Play();
        m_VideoPlayer.WaitForFrame(kVideoFirstFrameTimeoutSeconds);
        if (UploadVideoFrameIfReady())
            BindVideoPreviewBackground(/*forceUiRegen=*/true);
        else
            m_VideoNeedsBackgroundBind = true;
    }
    else
    {
        Logger::Log::Warning("AssetViewPanel: failed to load video preview '{}'", path.string());
        m_ForceStagingVideoUpload = false;
        if (!m_VideoTexture.IsValid())
        {
            m_IsVideoPreview = false;
            m_VideoPreviewPath.clear();
            m_VideoNeedsBackgroundBind = false;
        }
    }
}

void AssetViewPanel::StopVideoDecoder()
{
    if (m_VideoPlayer.IsLoaded())
    {
        m_VideoPlayer.Stop();
        m_VideoPlayer.Unload();
    }
}

void AssetViewPanel::ReleaseVideoPreviewGpuResources()
{
    m_ForceStagingVideoUpload = false;
    m_VideoNeedsBackgroundBind = false;
    DiscardStagingVideoTexture();

    if (m_VideoTexture.IsValid() && m_Device)
    {
        RetireVideoTexture(m_VideoTexture);
        m_VideoTexture = {};
        m_VideoTextureW = 0;
        m_VideoTextureH = 0;
        m_VideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    }
    m_VideoTextureUploaded = false;
    m_VideoPixelScratch.clear();
    if (m_UIManager)
        m_UIManager->RemoveExternalTexture(m_VideoTextureResourceName);
}

void AssetViewPanel::LeaveVideoPreview()
{
    StopVideoDecoder();
    m_IsVideoPreview = false;
    m_VideoPreviewPath.clear();
    m_ForceStagingVideoUpload = false;
    DiscardStagingVideoTexture();
    m_VideoNeedsBackgroundBind = false;
    // Keep m_VideoTexture, external texture registration, and the current
    // background binding until the still thumbnail is ready — same hold-last-
    // frame behavior as model/material preview swaps.
}

void AssetViewPanel::StopVideoPreview()
{
    const bool hadVideoPreview = m_IsVideoPreview || m_VideoTexture.IsValid() || !m_VideoPixelScratch.empty();
    StopVideoDecoder();
    m_IsVideoPreview = false;
    m_VideoPreviewPath.clear();
    m_VideoTextureUploaded = false;
    m_VideoNeedsBackgroundBind = false;
    m_ForceStagingVideoUpload = false;
    DiscardStagingVideoTexture();
    if (!hadVideoPreview)
        return;

    DrainRetiredVideoTextures(true);
    if (m_VideoTexture.IsValid() && m_Device)
    {
        m_Device->DestroyTexture(m_VideoTexture);
        m_VideoTexture = {};
        m_VideoTextureW = 0;
        m_VideoTextureH = 0;
        m_VideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    }
    m_VideoPixelScratch.clear();
    m_VideoPixelScratch.shrink_to_fit();
    if (m_UIManager)
        m_UIManager->RemoveExternalTexture(m_VideoTextureResourceName);
    if (m_Image && m_Image->GetOwnerManager())
        UI::Layout::ClearBackgroundOverride(*m_Image);
}

void AssetViewPanel::RetireVideoTexture(Rendering::TextureHandle handle)
{
    if (!handle.IsValid())
        return;
    m_RetiredVideoTextures.push_back({handle, kRetiredVideoTextureFrames});
}

void AssetViewPanel::DrainRetiredVideoTextures(bool force)
{
    if (!m_Device)
        return;

    for (auto it = m_RetiredVideoTextures.begin(); it != m_RetiredVideoTextures.end();)
    {
        if (force || --it->framesRemaining <= 0)
        {
            if (it->handle.IsValid())
                m_Device->DestroyTexture(it->handle);
            it = m_RetiredVideoTextures.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_RetiredVideoTextures.empty())
        m_RetiredVideoTextures.shrink_to_fit();
}

void AssetViewPanel::EnsureStagingVideoTexture(int w, int h, Video::VideoPixelFormat format)
{
    if (!m_Device) return;
    if (m_StagingVideoTexture.IsValid()
        && w == m_StagingVideoTextureW
        && h == m_StagingVideoTextureH
        && format == m_StagingVideoTextureFormat)
        return;

    if (m_StagingVideoTexture.IsValid())
    {
        RetireVideoTexture(m_StagingVideoTexture);
        m_StagingVideoTexture = {};
        m_StagingVideoTextureW = 0;
        m_StagingVideoTextureH = 0;
        m_StagingVideoTextureFormat = Video::VideoPixelFormat::RGBA8;
        m_StagingVideoTextureUploaded = false;
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
    desc.debugName   = "AssetViewVideoFrameStaging";

    m_StagingVideoTexture = m_Device->CreateTexture(desc);
    if (!m_StagingVideoTexture.IsValid())
        return;

    m_StagingVideoTextureW = w;
    m_StagingVideoTextureH = h;
    m_StagingVideoTextureFormat = format;
    m_StagingVideoTextureUploaded = false;
}

void AssetViewPanel::PromoteStagingVideoTexture(int w, int h, Video::VideoPixelFormat format)
{
    if (!m_StagingVideoTexture.IsValid()
        || w != m_StagingVideoTextureW
        || h != m_StagingVideoTextureH
        || format != m_StagingVideoTextureFormat)
        return;

    if (m_VideoTexture.IsValid())
        RetireVideoTexture(m_VideoTexture);

    m_VideoTexture = m_StagingVideoTexture;
    m_VideoTextureW = m_StagingVideoTextureW;
    m_VideoTextureH = m_StagingVideoTextureH;
    m_VideoTextureFormat = m_StagingVideoTextureFormat;
    m_VideoTextureUploaded = true;

    m_StagingVideoTexture = {};
    m_StagingVideoTextureW = 0;
    m_StagingVideoTextureH = 0;
    m_StagingVideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    m_StagingVideoTextureUploaded = false;
}

void AssetViewPanel::DiscardStagingVideoTexture()
{
    if (m_StagingVideoTexture.IsValid())
        RetireVideoTexture(m_StagingVideoTexture);
    m_StagingVideoTexture = {};
    m_StagingVideoTextureW = 0;
    m_StagingVideoTextureH = 0;
    m_StagingVideoTextureFormat = Video::VideoPixelFormat::RGBA8;
    m_StagingVideoTextureUploaded = false;
}

void AssetViewPanel::BindVideoPreviewBackground(bool forceUiRegen)
{
    if (!m_Image || m_Image->GetOwnerManager() == nullptr || !m_VideoTexture.IsValid() || !m_VideoTextureUploaded)
        return;

    // Re-register the GPU texture every bind. List/grid scroll invalidates UI
    // geometry for unrelated thumbnails; the preview image can rebuild while
    // m_VideoTextureUploaded stays true, leaving a stale or missing external slot.
    if (m_UIManager)
    {
        if (forceUiRegen)
            m_UIManager->RemoveExternalTexture(m_VideoTextureResourceName);

        // Decoded video frames are authored sRGB in an _SRGB view (the sampler
        // decodes on read): SrgbAuthored, same as VideoPlayerModal. The SDR-UI
        // arm keeps the preview at the UI's own white under HDR output instead
        // of passing it through as paper-white-relative scene content.
        m_UIManager->SetExternalTexture(
            m_VideoTextureResourceName,
            m_VideoTexture,
            {},
            static_cast<uint32_t>(std::max(m_VideoTextureW, 0)),
            static_cast<uint32_t>(std::max(m_VideoTextureH, 0)),
            UI::UITextureSpace::SrgbAuthored());
    }

    UI::Layout::SetBackgroundResourceName(*m_Image, m_VideoTextureResourceName);
    m_VideoNeedsBackgroundBind = false;
}

void AssetViewPanel::EnsureVideoPreviewBackgroundBound(bool forceUiRegen)
{
    if (!m_IsVideoPreview || !m_VideoTextureUploaded || !m_VideoTexture.IsValid())
        return;

    BindVideoPreviewBackground(forceUiRegen || m_VideoNeedsBackgroundBind);
}

bool AssetViewPanel::UploadVideoFrameIfReady()
{
    if (!m_IsVideoPreview || !m_VideoPlayer.IsLoaded() || !m_Device || !m_UIManager)
        return false;
    if (!m_VideoPlayer.PollNewFrame())
        return false;

    const int w = m_VideoPlayer.GetWidth();
    const int h = m_VideoPlayer.GetHeight();
    if (w <= 0 || h <= 0)
        return false;

    const Video::VideoPixelFormat frameFormat = m_VideoPlayer.GetFramePixelFormat();
    const bool reuseCurrentTexture = !m_ForceStagingVideoUpload
        && m_VideoTexture.IsValid()
        && w == m_VideoTextureW
        && h == m_VideoTextureH
        && frameFormat == m_VideoTextureFormat;

    Rendering::TextureHandle uploadTarget = m_VideoTexture;
    bool alreadyShaderReadable = m_VideoTextureUploaded;
    if (!reuseCurrentTexture)
    {
        EnsureStagingVideoTexture(w, h, frameFormat);
        if (!m_StagingVideoTexture.IsValid())
            return false;
        uploadTarget = m_StagingVideoTexture;
        alreadyShaderReadable = m_StagingVideoTextureUploaded;
    }

    m_VideoPixelScratch.resize(static_cast<size_t>(w) * h * 4);
    m_VideoPlayer.GetFramePixels(m_VideoPixelScratch.data());

    const size_t rowPitch = static_cast<size_t>(w) * 4;
    if (!UploadVideoFrame(m_Device, uploadTarget,
            m_VideoPixelScratch.data(),
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            rowPitch, alreadyShaderReadable))
        return false;

    if (reuseCurrentTexture)
    {
        m_VideoTextureUploaded = true;
    }
    else
    {
        m_StagingVideoTextureUploaded = true;
        PromoteStagingVideoTexture(w, h, frameFormat);
        m_ForceStagingVideoUpload = false;
    }

    // Same stamp rationale as BindVideoPreviewBackground.
    m_UIManager->SetExternalTexture(m_VideoTextureResourceName, m_VideoTexture,
        {}, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        UI::UITextureSpace::SrgbAuthored());
    BindVideoPreviewBackground(/*forceUiRegen=*/true);
    return true;
}

void AssetViewPanel::RefreshVideoPreviewBindingIfActive()
{
    EnsureVideoPreviewBackgroundBound(/*forceUiRegen=*/true);
}

AssetViewPanel::PreviewDebugState AssetViewPanel::GetPreviewDebugState() const
{
    PreviewDebugState state;
    state.previewPath = m_PreviewPath.string();
    state.previewEnabled = m_PreviewEnabled;
    state.isVideoPreview = m_IsVideoPreview;
    state.videoPlayerLoaded = m_VideoPlayer.IsLoaded();
    state.videoTextureUploaded = m_VideoTextureUploaded;
    state.videoNeedsBackgroundBind = m_VideoNeedsBackgroundBind;
    state.videoTextureWidth = m_VideoTextureW;
    state.videoTextureHeight = m_VideoTextureH;
    state.videoTextureResourceName = m_VideoTextureResourceName;
    state.displayedEngineResource = m_DisplayedEngineResource;
    return state;
}

bool AssetViewPanel::TryGetVideoPreviewReadbackTexture(Rendering::TextureHandle& outTexture,
                                                       Rendering::TextureFormat& outFormat) const
{
    if (!m_IsVideoPreview || !m_VideoTextureUploaded || !m_VideoTexture.IsValid() || !m_Device)
        return false;

    outTexture = m_VideoTexture;
    outFormat = m_VideoTextureFormat == Video::VideoPixelFormat::BGRA8
                    ? Rendering::TextureFormat::BGRA8_UNORM
                    : Rendering::TextureFormat::RGBA8_UNORM;
    return true;
}

void AssetViewPanel::Update(float dt)
{
    if (!m_IsMounted)
        return;

    DrainRetiredVideoTextures();

    // Push the current image element size down to the thumbnail handler so the
    // focused preview's RT is recreated whenever the panel is resized. Grid
    // thumbnails keep the square default; only the focused preview uses these
    // dimensions. Clearing the preview path reverts to square via (0, 0).
    if ((m_PreviewIsModel || m_PreviewIsMaterial || m_PreviewIsLensFlare) &&
        m_Image && m_Image->GetOwnerManager() != nullptr)
    {
        const float w = m_Image->GetLayoutWidth();
        const float h = m_Image->GetLayoutHeight();
        if (w > 0.0f && h > 0.0f)
        {
            ModelThumbnailHandler::SetPreviewSize(
                static_cast<uint32_t>(std::lround(w)),
                static_cast<uint32_t>(std::lround(h)));
        }
    }
    else if (!m_PreviewIsModel && !m_PreviewIsMaterial && !m_PreviewIsLensFlare)
    {
        ModelThumbnailHandler::SetPreviewSize(0, 0);
    }

    // Apply a deferred preview-resource swap once the new render is ready.
    // See ApplyAssetPreviewBackground: when the user picks a new asset whose
    // thumbnail isn't rendered yet, we keep the previous content visible
    // and stash the desired resource here. Polling each frame is fine —
    // the readiness check is a hash-map lookup.
    if (!m_PendingEngineResource.empty() && !m_IsVideoPreview && m_Image && m_Image->GetOwnerManager() != nullptr
        && m_Context && m_Context->Thumbnails)
    {
        const uint64_t windowId = m_Context->ThumbnailHostWindowId;
        const std::string engineName = ResourceNameWithoutEnginePrefix(m_PendingEngineResource);
        if (m_Context->Thumbnails->IsEngineThumbnailReadyForWindow(windowId, engineName))
        {
            UI::Layout::SetBackgroundResourceName(*m_Image, engineName);
            m_DisplayedEngineResource = m_PendingEngineResource;
            m_PendingEngineResource.clear();
            ReleaseVideoPreviewGpuResources();
        }
    }

    if (m_FilterPillSecondsRemaining > 0.0f)
    {
        m_FilterPillSecondsRemaining = std::max(0.0f, m_FilterPillSecondsRemaining - dt);
        if (m_FilterPillSecondsRemaining <= 0.0f && m_FilterBadgeRow)
            m_FilterBadgeRow->Overrides().Set(Style::Opacity, 0.0f);
    }

    if (!m_IsVideoPreview || !m_VideoPlayer.IsLoaded()) return;

    // The player paces its own decoding; this only samples what is ready.
    UploadVideoFrameIfReady();
    EnsureVideoPreviewBackgroundBound();
}

void AssetViewPanel::OnMountVisibilityChanged(bool isVisible)
{
    m_IsMounted = isVisible;
    if (!m_IsMounted)
    {
        StopVideoPreview();
        return;
    }

    if (!m_PreviewPath.empty() && IsVideoPath(m_PreviewPath))
        StartVideoPreview(m_PreviewPath);
}

// ── Preview ──────────────────────────────────────────────────────────────────

void AssetViewPanel::SetAssetPreview(const std::filesystem::path& path, bool enabled)
{
    ++m_PreviewVisualGeneration;

    // AssetViewPanel should always show the current asset when one is selected,
    // independent of the Scene-adjacent asset tab (Shift+F) / preview wiring.
    m_PreviewPath = path;
    m_PreviewEnabled = !m_PreviewPath.empty();

    (void)enabled;
    const bool wantsVideoPreview = !path.empty() && IsVideoPath(path);
    if (wantsVideoPreview)
    {
        m_PendingEngineResource.clear();
        StartVideoPreview(path);
        return;
    }

    if (m_IsVideoPreview)
        LeaveVideoPreview();

    if (!m_PreviewEnabled || m_PreviewPath.empty())
    {
        StopVideoPreview();
        UpdatePreviewVisual();
        return;
    }

    UpdatePreviewVisual();
    SchedulePreviewRefresh(4);
}

void AssetViewPanel::SchedulePreviewRefresh(int remainingAttempts)
{
    if (remainingAttempts <= 0)
        return;

    const uint64_t generation = m_PreviewVisualGeneration;
    PostAction([this, remainingAttempts, generation]()
    {
        if (generation != m_PreviewVisualGeneration)
            return;

        EnsureUi();
        if (!m_Image || m_Image->GetOwnerManager() == nullptr)
        {
            SchedulePreviewRefresh(remainingAttempts - 1);
            return;
        }

        UpdatePreviewVisual();
    });
}

void AssetViewPanel::UpdatePreviewVisual()
{
    EnsureUi();
    if (!m_Image || m_Image->GetOwnerManager() == nullptr)
        return;

    if (m_IsVideoPreview || IsVideoPath(m_PreviewPath))
        return;

    if (!m_PreviewEnabled || m_PreviewPath.empty())
    {
        UI::Layout::ClearBackgroundOverride(*m_Image);
        // The displayed/pending bookkeeping must die WITH the element's
        // background: re-selecting the same asset otherwise hits the
        // "already showing this resource" early-return and never rebinds —
        // the panel stays transparent while the handler renders into a slot
        // nothing displays.
        m_DisplayedEngineResource.clear();
        m_PendingEngineResource.clear();
        ModelThumbnailHandler::ClearOrbitFocus();
        ModelThumbnailHandler::ClearMaterialOrbitFocus();
        ModelThumbnailHandler::ClearLensFlareFocus();
        ModelThumbnailHandler::SetPreviewSize(0, 0);
        UpdateAnimationBadge("");
        m_PreviewIsModel = false;
        m_PreviewIsMaterial = false;
        m_PreviewIsLensFlare = false;
        UpdateLensFlareNamePill(false);
        if (m_ShapeRow)
            m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::None);
        if (m_FilterBadgeRow)
            m_FilterBadgeRow->Overrides().Set(Style::Opacity, 0.0f);
        UpdateHdriExposureBadge(false);
        return;
    }

    if (!m_Context || !m_Context->Thumbnails)
        return;

    int desiredSize = 1024;
    if (m_Image)
    {
        const float w = m_Image->GetLayoutWidth();
        const float h = m_Image->GetLayoutHeight();
        const float maxDim = std::max(w, h);
        if (maxDim > 0.0f)
            desiredSize = static_cast<int>(std::max(256.0f, std::min(maxDim, 4096.0f)));
    }

    const std::filesystem::path path = m_PreviewPath;
    const uint64_t generation = m_PreviewVisualGeneration;

    if (IsHdriExposurePreviewPath(path))
    {
        if (ApplyHdriExposurePreview(generation))
            return;
    }

    auto immediate = m_Context->Thumbnails->GetOrRequest(
        path, desiredSize,
        [this, path, generation, post = GetPostHandle()](const std::string& rel)
        {
            if (rel.empty())
                return;
            post.Post([this, path, rel, generation]()
                             {
                                 if (generation != m_PreviewVisualGeneration)
                                     return;
                                 if (!m_PreviewEnabled || path != m_PreviewPath)
                                     return;
                                 if (IsVideoPath(m_PreviewPath))
                                     return;
                                 ApplyAssetPreviewBackground(rel, generation);
                             });
        });

    if (!immediate.empty())
        ApplyAssetPreviewBackground(immediate, generation);
}

void AssetViewPanel::ApplyAssetPreviewBackground(const std::string& relOrEngine, uint64_t visualGeneration)
{
    if (visualGeneration != m_PreviewVisualGeneration)
        return;

    if (!m_Image || m_Image->GetOwnerManager() == nullptr)
        return;

    if (m_IsVideoPreview || IsVideoPath(m_PreviewPath))
        return;

    if (relOrEngine.empty())
    {
        m_PreviewIsModel = false;
        m_PreviewIsMaterial = false;
        m_PreviewIsLensFlare = false;
        UpdateLensFlareNamePill(false);
        ModelThumbnailHandler::ClearLensFlareFocus();
        UI::Layout::ClearBackgroundOverride(*m_Image);
        UpdateFilterModeClass();
        UpdateAnimationBadge("");
        m_DisplayedEngineResource.clear();
        m_PendingEngineResource.clear();
        if (m_ShapeRow)
            m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::None);
        if (m_FilterBadgeRow)
            m_FilterBadgeRow->Overrides().Set(Style::Opacity, 0.0f);
        UpdateHdriExposureBadge(false);
        return;
    }

    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t kEnginePrefixLen = 7;
    constexpr const char* kMaterialEnginePrefix = "engine:editor_material_thumb_";
    constexpr const char* kLensFlareEnginePrefix = "engine:editor_lensflare_thumb_";
    if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
    {
        const std::string engineName = relOrEngine.substr(kEnginePrefixLen);

        // Eagerly switch the orbit focus so the thumbnail handler starts
        // rendering the new asset at preview size on the next frame (a
        // material whose tile is not cached yet renders that tile first:
        // ModelThumbnailHandler::PromotePendingMaterialFocus). The
        // *visible* swap of the bound resource is gated below until the
        // new render is ready, but the focus update has to happen now or
        // the new render will never start.
        const bool isMaterial = relOrEngine.rfind(kMaterialEnginePrefix, 0) == 0;
        const bool isLensFlare = relOrEngine.rfind(kLensFlareEnginePrefix, 0) == 0;
        if (isMaterial)
        {
            m_PreviewIsModel = false;
            m_PreviewIsMaterial = true;
            m_PreviewIsLensFlare = false;
            UpdateLensFlareNamePill(false);
            ModelThumbnailHandler::ClearOrbitFocus();
            ModelThumbnailHandler::ClearLensFlareFocus();
            ModelThumbnailHandler::SetMaterialOrbitFocusFromEngineName(engineName);
            UpdateAnimationBadge("");
            UpdateFilterModeClass();
            if (m_ShapeRow)
                m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::Flex);
            UpdateHdriExposureBadge(false);
        }
        else if (isLensFlare)
        {
            m_PreviewIsModel = false;
            m_PreviewIsMaterial = false;
            m_PreviewIsLensFlare = true;
            UpdateLensFlareNamePill(true);
            ModelThumbnailHandler::ClearOrbitFocus();
            ModelThumbnailHandler::ClearMaterialOrbitFocus();
            ModelThumbnailHandler::SetLensFlareFocusFromEngineName(engineName);
            UpdateAnimationBadge("");
            UpdateFilterModeClass();
            if (m_ShapeRow)
                m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::None);
            UpdateHdriExposureBadge(false);
        }
        else
        {
            m_PreviewIsModel = true;
            m_PreviewIsMaterial = false;
            m_PreviewIsLensFlare = false;
            UpdateLensFlareNamePill(false);
            ModelThumbnailHandler::ClearMaterialOrbitFocus();
            ModelThumbnailHandler::ClearLensFlareFocus();
            ModelThumbnailHandler::SetOrbitFocusFromEngineName(engineName);
            UpdateAnimationBadge(ModelThumbnailHandler::GetFocusedAnimationLabel());
            UpdateFilterModeClass();
            if (m_ShapeRow)
                m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::None);
            UpdateHdriExposureBadge(false);
        }

        if (relOrEngine == m_DisplayedEngineResource)
        {
            // Already showing this resource. Clear any pending swap (the
            // previous click was superseded but happened to land on the
            // same selection again). Re-apply the binding anyway — selection
            // events are rare, the call is idempotent, and the bookkeeping
            // can outlive the element's actual background (a deselect path
            // that cleared the override but not this string would otherwise
            // leave the panel transparent forever).
            UI::Layout::SetBackgroundResourceName(*m_Image, engineName);
            m_PendingEngineResource.clear();
            return;
        }

        const uint64_t windowId = m_Context ? m_Context->ThumbnailHostWindowId : 0;
        if (m_Context && m_Context->Thumbnails)
            m_Context->Thumbnails->EnsureEngineThumbnailRequested(windowId, engineName);
        const bool ready = m_Context && m_Context->Thumbnails
                              && m_Context->Thumbnails->IsEngineThumbnailReadyForWindow(windowId, engineName);
        // Show the new content only when its render is ready. If it isn't,
        // queue it as pending and keep the previous bound resource on
        // screen — Update() polls each frame and applies once ready.
        // First-time switch (no previous content) skips the gate so the
        // panel doesn't sit empty waiting for itself.
        if (ready || m_DisplayedEngineResource.empty())
        {
            UI::Layout::SetBackgroundResourceName(*m_Image, engineName);
            m_DisplayedEngineResource = relOrEngine;
            m_PendingEngineResource.clear();
            ReleaseVideoPreviewGpuResources();
        }
        else
        {
            m_PendingEngineResource = relOrEngine;
        }
        return;
    }

    m_PreviewIsModel = false;
    m_PreviewIsMaterial = false;
    m_PreviewIsLensFlare = false;
    UpdateLensFlareNamePill(false);
    ModelThumbnailHandler::ClearOrbitFocus();
    ModelThumbnailHandler::ClearMaterialOrbitFocus();
    ModelThumbnailHandler::ClearLensFlareFocus();
    UI::Layout::SetBackgroundPath(*m_Image, relOrEngine);
    UpdateFilterModeClass();
    UpdateAnimationBadge("");
    if (m_ShapeRow)
        m_ShapeRow->Overrides().Set(Style::Display, DisplayMode::None);
    UpdateHdriExposureBadge(IsHdriExposurePreviewPath(m_PreviewPath));
    m_DisplayedEngineResource.clear();
    m_PendingEngineResource.clear();
    ReleaseVideoPreviewGpuResources();
}

bool AssetViewPanel::IsHdriExposurePreviewPath(const std::filesystem::path& path) const
{
    return !path.empty() && IsHdrImageExtension(path);
}

float AssetViewPanel::GetHdriExposureEVForPath(const std::filesystem::path& path) const
{
    const auto key = path.lexically_normal().generic_string();
    const auto it = m_HdriExposureByPath.find(key);
    if (it == m_HdriExposureByPath.end())
        return 0.0f;
    return std::clamp(it->second, kHdriExposureMinEV, kHdriExposureMaxEV);
}

void AssetViewPanel::SetHdriExposureEVForPath(const std::filesystem::path& path, float exposureEV)
{
    const auto key = path.lexically_normal().generic_string();
    if (key.empty())
        return;
    m_HdriExposureByPath[key] = std::clamp(exposureEV, kHdriExposureMinEV, kHdriExposureMaxEV);
}

void AssetViewPanel::AdjustHdriExposure(float scrollY)
{
    if (!IsHdriExposurePreviewPath(m_PreviewPath))
        return;

    const float direction = scrollY > 0.0f ? 1.0f : -1.0f;
    StepHdriExposure(direction * kHdriExposureScrollStepEV);
}

void AssetViewPanel::StepHdriExposure(float deltaEV)
{
    if (!IsHdriExposurePreviewPath(m_PreviewPath))
        return;

    const float exposureEV = std::clamp(
        GetHdriExposureEVForPath(m_PreviewPath) + deltaEV,
        kHdriExposureMinEV, kHdriExposureMaxEV);
    SetHdriExposureEVForPath(m_PreviewPath, exposureEV);
    ++m_PreviewVisualGeneration;
    ApplyHdriExposurePreview(m_PreviewVisualGeneration);
    UpdateHdriExposureBadge(true);
}

bool AssetViewPanel::ApplyHdriExposurePreview(uint64_t visualGeneration)
{
    if (visualGeneration != m_PreviewVisualGeneration)
        return false;
    if (!m_Image || m_Image->GetOwnerManager() == nullptr || m_PreviewPath.empty())
        return false;

    int desiredSize = 1024;
    const float w = m_Image->GetLayoutWidth();
    const float h = m_Image->GetLayoutHeight();
    const float maxDim = std::max(w, h);
    if (maxDim > 0.0f)
        desiredSize = static_cast<int>(std::max(256.0f, std::min(maxDim, 4096.0f)));

    const float exposureEV = GetHdriExposureEVForPath(m_PreviewPath);
    const auto previewPath = MakeHdriExposurePreviewPath(m_PreviewPath, desiredSize, exposureEV);
    if (previewPath.empty() || !GenerateHdriExposurePreview(m_PreviewPath, previewPath, desiredSize, exposureEV))
        return false;

    ApplyAssetPreviewBackground(previewPath.generic_string(), visualGeneration);
    return true;
}

void AssetViewPanel::UpdateHdriExposureBadge(bool visible)
{
    if (!m_HdriExposureBadgeRow || !m_HdriExposureLabel)
        return;

    if (!visible)
    {
        m_HdriExposureBadgeRow->Overrides().Set(Style::Display, DisplayMode::None);
        m_HdriExposureBadgeRow->MarkDirty(UIElement::VisualDirty);
        return;
    }

    m_HdriExposureLabel->SetText(FormatExposureEV(GetHdriExposureEVForPath(m_PreviewPath)));
    m_HdriExposureBadgeRow->Overrides().Set(Style::Display, DisplayMode::Flex);
    m_HdriExposureLabel->MarkDirty(UIElement::VisualDirty);
    m_HdriExposureBadgeRow->MarkDirty(UIElement::VisualDirty);
}

void AssetViewPanel::ToggleImageFilterMode()
{
    if (!m_Image || !m_PreviewEnabled || m_PreviewPath.empty() || m_IsVideoPreview || m_PreviewIsModel || m_PreviewIsMaterial || m_PreviewIsLensFlare)
        return;

    m_UsePointFilter = !m_UsePointFilter;
    UpdateFilterModeClass();
    ShowFilterModePill();
}

void AssetViewPanel::UpdateFilterModeClass()
{
    if (!m_Image)
        return;

    if (m_UsePointFilter && m_PreviewEnabled && !m_IsVideoPreview && !m_PreviewIsModel && !m_PreviewIsMaterial && !m_PreviewIsLensFlare)
        m_Image->AddClass("ui-bg-point-filter");
    else
        m_Image->RemoveClass("ui-bg-point-filter");
    m_Image->MarkDirty(UIElement::VisualDirty);
}

void AssetViewPanel::ShowFilterModePill()
{
    if (!m_FilterBadgeRow || !m_FilterBadgeLabel)
        return;

    m_FilterPillSecondsRemaining = 1.0f;
    m_FilterBadgeLabel->SetText(m_UsePointFilter ? "Point Filter" : "Bilinear Filter");
    m_FilterBadgeRow->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::Opacity, 1.0f);
    m_FilterBadgeLabel->MarkDirty(UIElement::VisualDirty);
    m_FilterBadgeRow->MarkDirty(UIElement::VisualDirty);
}

void AssetViewPanel::UpdateAnimationBadge(const std::string& text)
{
    if (!m_AnimBadgeRow || !m_AnimBadgeBubble || !m_AnimLabel)
        return;

    m_AnimLabel->SetText(text);
    if (text.empty())
    {
        m_AnimBadgeRow->Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }

    m_AnimBadgeRow->Overrides().Set(Style::Display, DisplayMode::Flex);
    const DisplayMode buttonDisplay = ModelThumbnailHandler::GetFocusedAnimationCount() > 1
        ? DisplayMode::Flex
        : DisplayMode::None;
    if (m_AnimPrevButton)
        m_AnimPrevButton->Overrides().Set(Style::Display, buttonDisplay);
    if (m_AnimNextButton)
        m_AnimNextButton->Overrides().Set(Style::Display, buttonDisplay);
}

void AssetViewPanel::StepAnimation(float deltaSteps)
{
    ModelThumbnailHandler::StepFocusedAnimation(deltaSteps);
    UpdateAnimationBadge(ModelThumbnailHandler::GetFocusedAnimationLabel());
}

void AssetViewPanel::UpdateLensFlareNamePill(bool visible)
{
    if (!m_LensFlareNameRow || !m_LensFlareNameLabel)
        return;

    const std::string name = m_PreviewPath.stem().string();
    if (!visible || name.empty())
    {
        m_LensFlareNameRow->Overrides().Set(Style::Display, DisplayMode::None);
        m_LensFlareNameRow->MarkDirty(UIElement::VisualDirty);
        return;
    }

    m_LensFlareNameLabel->SetText(name);
    m_LensFlareNameRow->Overrides().Set(Style::Display, DisplayMode::Flex);
    m_LensFlareNameLabel->MarkDirty(UIElement::VisualDirty);
    m_LensFlareNameRow->MarkDirty(UIElement::VisualDirty);
}

} // namespace GameEngine
