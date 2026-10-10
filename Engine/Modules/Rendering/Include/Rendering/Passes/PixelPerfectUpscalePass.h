#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/FinalizeContract.h"

#include <cstdint>

namespace GameEngine {
namespace Rendering {

class IDevice;

namespace Passes {

// Parameters for the smooth sub-pixel pixel-perfect upscale pass.
//
// The source is an offscreen render target sized (ReferenceWidth + 2) x
// (ReferenceHeight + 2) carrying the camera's final output at native reference
// scale, in the space the caller declares to AddPixelPerfectUpscalePassRG. The
// pass point-samples a reference-sized window out of it, shifted by the
// sub-pixel camera remainder, and writes the integer-upscaled result centered
// in the destination (letterboxing the rest to black). Every output pixel is a
// copy of exactly one source texel (nearest sampler, integer zoom) — the pass
// never filters. Src/Dst are frame-local values (the editor PP chain passes
// its FinalLinear as Dst with PassthroughLinear=true; a Player port passes the
// imported backbuffer).
//
// THE MAPPING, EXACTLY — this is contract, not implementation detail, because a
// consumer that has to recover it by reading the shader can only ever agree
// with whatever the shader currently does:
//   outputSize   = (ReferenceWidth, ReferenceHeight) * Zoom
//   outputOrigin = floor((dstExtent - outputSize) * 0.5), per axis, so the rect
//                  lands on a whole destination pixel and the surplus is split
//                  with the extra pixel on the bottom/right
//   local        = (destination pixel CENTRE) - outputOrigin; outside
//                  [0, outputSize) the pixel is opaque black
//   sample       = (1 + FracX, 1 - FracY) + local / Zoom, in source texels —
//                  the origin is the one-texel border moved by the remainder in
//                  the direction the camera moved: a camera right of its snap
//                  reads further right (columns grow with world X), a camera
//                  above its snap reads further up (rows grow against world Y)
//   texel        = floor(sample), the nearest sampler's snap on a texel-centre
//                  grid; the one-texel border is what keeps this off the clamp
// The border is not decoration: a sample that reaches the edge is answered by
// clamp-to-edge, and "a copy of exactly one source texel" would then be
// satisfied by the addressing mode rather than by this mapping.
struct PixelPerfectUpscaleParamsRG
{
    RenderGraph::RGTexture Src{};
    RenderGraph::RGTexture Dst{};
    // Linear input only. When true, output linear color unchanged (a
    // downstream pass owns the output transfer function). When false, encode
    // for the active swapchain/HDR output (keyed on whether Dst is the
    // backbuffer). Ignored for an EncodedSrgb input, whose one behaviour is
    // byte passthrough (see AddPixelPerfectUpscalePassRG).
    bool PassthroughLinear = false;

    uint32_t ReferenceWidth = 0;
    uint32_t ReferenceHeight = 0;
    uint32_t SourceWidth = 0;  // ReferenceWidth + 2 (padded RT)
    uint32_t SourceHeight = 0; // ReferenceHeight + 2
    uint32_t Zoom = 1;
    // Sub-pixel camera remainder in source texels, range [-0.5, 0.5] per axis.
    float FracX = 0.0f;
    float FracY = 0.0f;
};

// `inputSpace` is REQUIRED (FinalizeContract.h): Linear means Src holds
// display-linear values and the pass encodes (or passes through for a
// downstream encode, per PassthroughLinear). EncodedSrgb means Src already
// holds sRGB-encoded bytes (#767 — an SDR HUD chain's composite): a
// point-sampled integer upscale of encoded bytes is byte-preserving, so the
// pass applies no transfer function — raw bytes into a UNORM or float
// destination, D(c) into an _SRGB destination so the ROP's re-encode
// round-trips — and refuses to declare under an active HDR output mode.
RenderGraph::RGPass AddPixelPerfectUpscalePassRG(RenderGraph::RGFrame& frame,
                                         const PixelPerfectUpscaleParamsRG& params,
                                         FinalizeInputSpace inputSpace,
                                         const char* passName = "PixelPerfectUpscale");

// Release any per-device upscale resources for the given logical device. Safe to
// call multiple times or with a null device.
void CleanupPixelPerfectUpscalePassForDevice(IDevice* device);

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
