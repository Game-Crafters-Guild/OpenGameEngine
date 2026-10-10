#version 450

// Shadow map debug overlay: renders 4 cascade layers as 128x128 thumbnails
// in the bottom-left corner of the viewport. Uses alpha blending so the
// world is visible underneath.
//
// Bound via FullscreenShaderNode with the shadow map array as ShadowMapArray.
// The sampler is a regular (non-comparison) linear clamp sampler, so we read
// raw depth values from the D32_FLOAT shadow map.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2DArray ShadowMapArray;

// Thumbnail layout constants.
const float kThumbnailSize = 128.0; // Pixels per cascade thumbnail.
const float kPadding = 4.0;         // Pixels between thumbnails.
const float kMargin = 8.0;          // Pixels from viewport edge.
const int   kNumCascades = 4;

// Per-cascade label tint (matches shadow_sampling.glsl cascade debug colors).
const vec3 kCascadeTint[4] = vec3[4](
    vec3(1.0, 0.2, 0.2),  // Cascade 0: red
    vec3(0.2, 1.0, 0.2),  // Cascade 1: green
    vec3(0.2, 0.2, 1.0),  // Cascade 2: blue
    vec3(1.0, 1.0, 0.2)   // Cascade 3: yellow
);

void main()
{
    // Convert UV to pixel coordinates using derivatives (viewport-independent).
    vec2 pixelPos = gl_FragCoord.xy;

    // Total strip width: 4 thumbnails with padding.
    float stripWidth = float(kNumCascades) * kThumbnailSize + float(kNumCascades - 1) * kPadding;
    float stripHeight = kThumbnailSize;

    // Bottom-left origin.
    vec2 stripOrigin = vec2(kMargin, kMargin);

    vec2 localPos = pixelPos - stripOrigin;

    // Outside the thumbnail strip: discard to preserve the scene underneath.
    if (localPos.x < 0.0 || localPos.x > stripWidth ||
        localPos.y < 0.0 || localPos.y > stripHeight)
    {
        discard;
    }

    // Determine which cascade and UV within it.
    float cellWidth = kThumbnailSize + kPadding;
    int cascadeIdx = int(localPos.x / cellWidth);
    float withinCellX = localPos.x - float(cascadeIdx) * cellWidth;

    // In the padding gap between thumbnails: discard.
    if (withinCellX > kThumbnailSize || cascadeIdx >= kNumCascades)
    {
        discard;
    }

    vec2 cascadeUV = vec2(withinCellX / kThumbnailSize, localPos.y / kThumbnailSize);

    // Sample depth from the shadow map array layer.
    float depth = texture(ShadowMapArray, vec3(cascadeUV, float(cascadeIdx))).r;

    // Visualize: reverse-Z natively makes near=bright (depth near 1.0), far=dark.
    vec3 col = vec3(depth);

    // Add a 2-pixel colored border for cascade identification.
    float border = 2.0;
    if (withinCellX < border || withinCellX > kThumbnailSize - border ||
        localPos.y < border || localPos.y > kThumbnailSize - border)
    {
        col = kCascadeTint[cascadeIdx];
    }

    oColor = vec4(col, 0.85);
}
