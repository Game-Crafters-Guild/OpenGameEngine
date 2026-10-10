#include "Engine/Rendering/CameraAspectRatio.h"

#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraPixelGrid.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::CameraId;
using ::GameEngine::Rendering::ViewId;
namespace
{
constexpr float kMinAspectRatio = 0.0001f;
constexpr float kAspectMatchEpsilon = 0.001f;

float SafeViewportAspect(uint32_t viewportWidth, uint32_t viewportHeight)
{
    if (viewportWidth == 0 || viewportHeight == 0)
        return 16.0f / 9.0f;
    return static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
}

float ResolveTargetAspect(const Components::Camera& camera)
{
    const auto preset = GetCameraAspectPreset(camera);
    if (preset == CameraAspectPreset::Custom)
    {
        const float width = std::max(camera.CustomAspectWidth, kMinAspectRatio);
        const float height = std::max(camera.CustomAspectHeight, kMinAspectRatio);
        return width / height;
    }

    return GetCameraAspectPresetRatio(preset);
}

} // namespace

float GetCameraAspectPresetRatio(CameraAspectPreset preset)
{
    switch (preset)
    {
    case CameraAspectPreset::Ratio16x9:
        return 16.0f / 9.0f;
    case CameraAspectPreset::Ratio4x3:
        return 4.0f / 3.0f;
    case CameraAspectPreset::Ratio16x10:
        return 16.0f / 10.0f;
    case CameraAspectPreset::Ratio21x9:
        return 21.0f / 9.0f;
    case CameraAspectPreset::Native:
    case CameraAspectPreset::Custom:
    default:
        return 16.0f / 9.0f;
    }
}

CameraAspectPreset GetCameraAspectPreset(const Components::Camera& camera)
{
    switch (camera.AspectPreset)
    {
    case static_cast<uint32>(CameraAspectPreset::Native):
    case static_cast<uint32>(CameraAspectPreset::Ratio16x9):
    case static_cast<uint32>(CameraAspectPreset::Ratio4x3):
    case static_cast<uint32>(CameraAspectPreset::Ratio16x10):
    case static_cast<uint32>(CameraAspectPreset::Ratio21x9):
    case static_cast<uint32>(CameraAspectPreset::Custom):
        return static_cast<CameraAspectPreset>(camera.AspectPreset);
    default:
        return CameraAspectPreset::Native;
    }
}

std::string GetCameraAspectPresetDropdownValue(const Components::Camera& camera)
{
    switch (GetCameraAspectPreset(camera))
    {
    case CameraAspectPreset::Native:
        return "native";
    case CameraAspectPreset::Ratio16x9:
        return "16:9";
    case CameraAspectPreset::Ratio4x3:
        return "4:3";
    case CameraAspectPreset::Ratio16x10:
        return "16:10";
    case CameraAspectPreset::Ratio21x9:
        return "21:9";
    case CameraAspectPreset::Custom:
        return "custom";
    }
    return "native";
}

CameraAspectPreset CameraAspectPresetFromDropdownValue(const std::string& value)
{
    if (value == "16:9")
        return CameraAspectPreset::Ratio16x9;
    if (value == "4:3")
        return CameraAspectPreset::Ratio4x3;
    if (value == "16:10")
        return CameraAspectPreset::Ratio16x10;
    if (value == "21:9")
        return CameraAspectPreset::Ratio21x9;
    if (value == "custom")
        return CameraAspectPreset::Custom;
    return CameraAspectPreset::Native;
}

ViewLetterbox ComputeLetterboxRect(uint32_t viewportWidth,
                                   uint32_t viewportHeight,
                                   float targetAspect)
{
    ViewLetterbox rect{};
    if (viewportWidth == 0 || viewportHeight == 0)
        return rect;

    const float safeTargetAspect = std::max(targetAspect, kMinAspectRatio);
    const float viewportAspect = SafeViewportAspect(viewportWidth, viewportHeight);
    if (std::abs(viewportAspect - safeTargetAspect) <= kAspectMatchEpsilon)
        return rect;

    rect.active = true;
    if (viewportAspect > safeTargetAspect)
    {
        rect.height = viewportHeight;
        rect.width = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(viewportHeight) * safeTargetAspect)));
        rect.x = (viewportWidth - rect.width) / 2u;
        rect.y = 0u;
    }
    else
    {
        rect.width = viewportWidth;
        rect.height = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(viewportWidth) / safeTargetAspect)));
        rect.x = 0u;
        rect.y = (viewportHeight - rect.height) / 2u;
    }
    return rect;
}

CameraAspectResolution ResolveCameraAspect(const Components::Camera& camera,
                                           uint32_t viewportWidth,
                                           uint32_t viewportHeight)
{
    CameraAspectResolution resolved{};
    const float viewportAspect = SafeViewportAspect(viewportWidth, viewportHeight);
    const auto preset = GetCameraAspectPreset(camera);
    if (preset == CameraAspectPreset::Native)
    {
        resolved.projectionAspect = viewportAspect;
        return resolved;
    }

    const float targetAspect = ResolveTargetAspect(camera);
    resolved.projectionAspect = targetAspect;
    resolved.letterbox = ComputeLetterboxRect(viewportWidth, viewportHeight, targetAspect);
    return resolved;
}

namespace
{
// One reference-resolution texel border on every side of the offscreen RT.
// The upscale pass shifts its sample window by the sub-pixel camera remainder
// (up to +/-0.5 texels), so a single guard texel keeps the window in bounds.
constexpr uint32_t kPixelPerfectBorderTexels = 1u;

struct PixelPerfectResult
{
    PixelPerfectViewState state{};
    float orthoHeight {0.0f};
    // Texel size in world units (1 / pixels-per-unit). The snapped camera
    // translation is a multiple of this; the remainder is the sub-pixel offset.
};

// Smooth sub-pixel pixel-perfect ortho computation.
//
// The world is rendered into an offscreen RT at native art scale (one world
// unit = ppu texels), sized (refW + 2) x (refH + 2). The ortho extent covers
// the FULL padded RT so the border texels are valid scene pixels. The integer
// Zoom is the largest factor that fits the reference resolution in the
// swapchain; the upscale pass blits refW*Zoom x refH*Zoom and letterboxes the
// remainder.
PixelPerfectResult ComputePixelPerfect(const Components::Camera& camera,
                                       uint32_t viewportWidth,
                                       uint32_t viewportHeight)
{
    const float ppu = static_cast<float>(std::max(1u, camera.PixelPerfectPixelsPerUnit));
    const uint32_t refH = std::max(1u, camera.PixelPerfectReferenceHeight);
    uint32_t refW = std::max(1u, camera.PixelPerfectReferenceWidth);

    // An aspect-ratio preset constrains the reference aspect: derive the reference
    // width from the height so the pixel-perfect image is letterboxed/pillarboxed
    // to the preset aspect (the upscale pass black-bars the remaining screen). The
    // Native preset keeps the authored reference width.
    const CameraAspectPreset preset = GetCameraAspectPreset(camera);
    const bool aspectLocked = (preset != CameraAspectPreset::Native);
    if (aspectLocked)
    {
        const float targetAspect = std::max(ResolveTargetAspect(camera), kMinAspectRatio);
        refW = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(refH) * targetAspect)));
    }

    // Integer zoom is always derived from the REFERENCE resolution, so a sprite
    // authored at the reference scale renders at the same pixel size in either
    // crop mode.
    const uint32_t zoomW = viewportWidth / refW;
    const uint32_t zoomH = viewportHeight / refH;
    const uint32_t zoom = std::max(1u, std::min(zoomW, zoomH));

    // Sampled window (the area the camera renders + the upscale pass samples).
    // Letterbox mode: exactly the reference resolution; the upscale pass black-bars
    // any screen area beyond reference*zoom. Expand mode: grow to ceil(screen/zoom)
    // so the upscaled image fills the whole screen (revealing a little more world);
    // the upscale pass crops the sub-zoom overflow via its centered (signed) origin.
    // The window never shrinks below the reference resolution. Expand is suppressed
    // when an aspect preset is active, since the preset's whole purpose is to keep
    // bars at a fixed aspect.
    uint32_t visW = refW;
    uint32_t visH = refH;
    if (camera.PixelPerfectExpand && !aspectLocked && viewportWidth > 0u && viewportHeight > 0u)
    {
        visW = std::max(refW, (viewportWidth + zoom - 1u) / zoom);
        visH = std::max(refH, (viewportHeight + zoom - 1u) / zoom);
    }

    const uint32_t paddedW = visW + 2u * kPixelPerfectBorderTexels;
    const uint32_t paddedH = visH + 2u * kPixelPerfectBorderTexels;

    PixelPerfectResult result{};
    // Cover the full padded RT height (native scale, NOT pre-zoomed).
    result.orthoHeight = static_cast<float>(paddedH) / ppu;
    result.state.Active = true;
    result.state.ReferenceWidth = visW;
    result.state.ReferenceHeight = visH;
    result.state.PaddedWidth = paddedW;
    result.state.PaddedHeight = paddedH;
    result.state.Zoom = zoom;
    return result;
}
} // namespace

PixelPerfectViewState ComputePixelPerfectViewState(const Components::Camera& camera,
                                                   uint32_t viewportWidth,
                                                   uint32_t viewportHeight)
{
    if (!camera.PixelPerfect || camera.Perspective)
        return {};
    return ComputePixelPerfect(camera, viewportWidth, viewportHeight).state;
}

void ApplyActiveCameraAspect(RenderServices& rs,
                             Rendering::ViewId viewId,
                             Rendering::CameraId cameraId,
                             const Camera& activeCamera,
                             uint32_t viewportWidth,
                             uint32_t viewportHeight)
{
    const bool pixelPerfect = activeCamera.params.PixelPerfect && !activeCamera.params.Perspective;
    if (pixelPerfect)
    {
        PixelPerfectResult pp = ComputePixelPerfect(activeCamera.params, viewportWidth, viewportHeight);

        // Aspect comes from the padded RT, not the swapchain — the world renders
        // square-pixel into the RT and the upscale pass owns letterboxing.
        const float rtAspect = static_cast<float>(pp.state.PaddedWidth) /
                               static_cast<float>(pp.state.PaddedHeight);
        auto cameraData = activeCamera.ToCameraData(rtAspect, pp.orthoHeight);
        if (activeCamera.params.PixelPerfectPixelSnap)
        {
            std::array<float, 4> uvTransform;
            // The pixel-perfect raster already includes its guard cells.
            cameraData = Rendering::StabilizeCameraToPixelGrid(cameraData,
                pp.state.PaddedWidth, pp.state.PaddedHeight, 1.0f, uvTransform, 0);
            pp.state.FracX = uvTransform[2] * pp.state.PaddedWidth;
            pp.state.FracY = -uvTransform[3] * pp.state.PaddedHeight;
        }
        rs.Views().SetCameraData(cameraId, cameraData);
        // The upscale pass centers + letterboxes the output itself, so the view
        // itself carries no letterbox rect.
        rs.Views().SetViewLetterbox(viewId, ViewLetterbox{});
        rs.Views().SetViewPixelPerfect(viewId, pp.state);
        return;
    }

    const CameraAspectResolution resolved =
        ResolveCameraAspect(activeCamera.params, viewportWidth, viewportHeight);
    rs.Views().SetCameraData(cameraId, activeCamera.ToCameraData(resolved.projectionAspect));
    rs.Views().SetViewLetterbox(viewId, resolved.letterbox);
    rs.Views().SetViewPixelPerfect(viewId, PixelPerfectViewState{});
}

} // namespace GameEngine::Engine::Renderer
