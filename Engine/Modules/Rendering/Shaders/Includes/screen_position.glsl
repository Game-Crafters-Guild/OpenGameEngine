#ifndef GE_SCREEN_POSITION_GLSL
#define GE_SCREEN_POSITION_GLSL

// Fullscreen coordinate vocabulary:
// - ViewportUV: (0,0) at the top-left of the viewport, (1,1) at bottom-right.
// - PixelPosition: pixel coordinate in the active viewport.
// - RenderTargetUV: normalized UV in the backing render target texture.
// - YUpNdc: normalized device coordinates with +Y up.
//
// fullscreen_noinput.vert writes ViewportUV into vUV. Use vUV directly when
// sampling fullscreen textures, and convert through these helpers only when
// reconstructing or projecting Y-up NDC positions.
//
// When the active viewport covers the whole render target, ViewportUV and
// RenderTargetUV are identical. Keep the spaces separate anyway; dynamic
// resolution, editor subviews, split views, or letterboxed render areas can
// make the backing texture larger than the active viewport.
struct GE_ScreenRect
{
    vec2 originPx;
    vec2 sizePx;
};

vec2 GE_ViewportUVToYUpNdc(vec2 viewportUV)
{
    return vec2(viewportUV.x * 2.0 - 1.0, (1.0 - viewportUV.y) * 2.0 - 1.0);
}

vec2 GE_YUpNdcToViewportUV(vec2 yUpNdc)
{
    return vec2(yUpNdc.x * 0.5 + 0.5, 1.0 - (yUpNdc.y * 0.5 + 0.5));
}

vec2 GE_GetViewportUV(vec2 viewportUV)
{
    return viewportUV;
}

vec2 GE_ViewportUVToPixelPosition(vec2 viewportUV, vec2 viewportSize)
{
    return viewportUV * viewportSize;
}

vec2 GE_PixelPositionToViewportUV(vec2 pixelPosition, vec2 viewportSize)
{
    return pixelPosition / max(viewportSize, vec2(1.0));
}

vec2 GE_PixelPositionToViewportUV(vec2 pixelPosition, GE_ScreenRect viewport)
{
    return GE_PixelPositionToViewportUV(pixelPosition - viewport.originPx, viewport.sizePx);
}

vec2 GE_ViewportUVToRenderTargetUV(vec2 viewportUV, GE_ScreenRect viewport, vec2 renderTargetSize)
{
    vec2 pixelPosition = viewport.originPx + viewportUV * viewport.sizePx;
    return pixelPosition / max(renderTargetSize, vec2(1.0));
}

vec2 GE_RenderTargetUVToViewportUV(vec2 renderTargetUV, GE_ScreenRect viewport, vec2 renderTargetSize)
{
    vec2 pixelPosition = renderTargetUV * renderTargetSize;
    return GE_PixelPositionToViewportUV(pixelPosition, viewport);
}

vec2 GE_ViewportUVToNdcYDown(vec2 viewportUV)
{
    return viewportUV * 2.0 - 1.0;
}

vec2 GE_NdcYDownToViewportUV(vec2 ndc)
{
    return ndc * 0.5 + 0.5;
}

bool GE_ViewportUVInside(vec2 viewportUV)
{
    return all(greaterThanEqual(viewportUV, vec2(0.0))) &&
           all(lessThanEqual(viewportUV, vec2(1.0)));
}

// Earlier spellings kept as wrappers; prefer the YUpNdc names above.
vec2 GE_ViewportUVToNdcYUp(vec2 viewportUV)
{
    return GE_ViewportUVToYUpNdc(viewportUV);
}

vec2 GE_NdcYUpToViewportUV(vec2 ndc)
{
    return GE_YUpNdcToViewportUV(ndc);
}

vec2 GE_FullscreenUvToNdcYUp(vec2 uv)
{
    return GE_ViewportUVToYUpNdc(uv);
}

#endif
