#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/FinalizeContract.h"

#include <cstdint>

namespace GameEngine {
namespace Rendering {

class IDevice;

namespace Passes {

// Parameters for an offscreen tonemap pass. Defaults mirror the main SceneView
// ForwardPlus pipeline's "Tonemap" node (Khronos PBR Neutral, exposure 1.0), so
// an offscreen render (asset thumbnails) shows the same operator as the viewport.
struct TonemapParams
{
	float exposure = 1.0f;
	// tonemap.frag operator selector: 0=ACES, 1=Reinhard, 2=AgX, 3=Filmic,
	// 4=Neutral, 5=Linear, 6=Gran Turismo 7, 7=ACES 2.
	int32_t tonemapMode = 4;
	// tonemap.frag output selector. 1 = emit display-referred LINEAR, UNDITHERED — a following
	// AddSRGBEncodePass owns the linear->sRGB OETF + dither at the destination bit depth. This is
	// the SDR chain the main view uses (Tonemap -> FinalSRGBEncode). Do not set an OETF here:
	// tonemap.frag never applies a transfer function.
	int32_t outEncoding = 1;
	// Only consulted by tonemap.frag for HDR-output encodings (outEncoding 2-4). The default
	// SDR/thumbnail path uses outEncoding=1 and never reads these, so 203 is an inert placeholder;
	// for HDR-output reuse, source the anchor from device HDR state (engine scRGB anchor is 80 nits).
	float paperWhiteNits = 203.0f;
	float maxOutputNits = 203.0f;
	// Effective HDR10/HDR10+ ICtCp highlight-chroma compression. Standalone
	// callers must leave this at zero for SDR, HLG, and scRGB output.
	float ictcpChromaCompression = 0.0f;
	// 0 = opaque output (default, main-view behaviour); 1 = preserve source alpha
	// (transparent background/thumbnail output); 2 = derive straight-alpha
	// coverage from the final tonemapped RGB and normalize RGB for premultiplied UI.
	int32_t preserveAlpha = 0;
};

// tonemap.frag's `outEncoding` for a pass declaring it against a live device.
// The Finalize contract's SECOND selector (FinalizeContract.h), and the one
// whose name most invites the wrong mental model: tonemap.frag applies NO
// output transfer function in any arm, so this is a dither-ownership selector,
// not a terminal encode. It cannot implement the Finalize requantize clauses
// (it has no encoded-input arm and does not know its destination), and it takes
// the input space as a REQUIRED argument anyway, for the reason the contract
// exists: a pipeline that ever points a Tonemap at an already-encoded source
// must state that rather than inherit an assumption, and find out loudly.
//
// The device is read for the ACTIVE HDR OUTPUT MODE only — a genuinely
// device-global property. NOT for the swapchain format: one pass owns the
// dither for a frame path, and on every SDR path that pass is the terminal
// Finalize, which sizes both filters to the real destination step
// (SRGBEncodePass.cpp). This pass cannot see a step, so it never filters for
// one — and a per-window format read through a process-global device property
// would pick the wrong window besides.
int SelectTonemapOutEncoding(IDevice* device, FinalizeInputSpace inputSpace);

// Offscreen HDR -> display-referred tonemap pass (no OETF) reusing the
// canonical tonemap.frag, declared fresh per frame — outEncoding=1 emits
// display-referred LINEAR, undithered; a following AddSRGBEncodePassRG owns
// the only OETF. Returns an invalid pass (declares nothing) on missing
// shaders (retry next frame, never latch) or src == dst.
RenderGraph::RGPass AddTonemapPassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                             const TonemapParams& params, const char* passName = "Tonemap");

// Release any per-device tonemap resources associated with the given logical device.
// Safe to call multiple times or with a null device.
void CleanupTonemapPassForDevice(IDevice* device);

// The ACES 2 tier-table buffer for the given device, created and uploaded on
// first use from the generated TonemapAces2Tables.h payload. tonemap.frag's
// Aces2Tables SSBO (set 0, binding 2) must point at it wherever that shader is
// bound — AddTonemapPassRG and the pipeline Tonemap node (FullscreenShaderNode)
// both bind it. Freed by CleanupTonemapPassForDevice.
BufferHandle GetAces2TablesBuffer(IDevice* device);

// Byte size of the Aces2Tables payload (the SSBO bind range).
uint32_t GetAces2TablesBufferBytes();

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
