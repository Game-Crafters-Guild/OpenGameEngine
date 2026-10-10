#pragma once

#include <cstdint>

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/FinalizeContract.h"

namespace GameEngine {
namespace Rendering {

class IDevice;

namespace Passes {

// Release any per-device SRGB encode resources associated with the given
// logical device. Safe to call multiple times or with a null device.
void CleanupSRGBEncodePassForDevice(IDevice* device);

// Terminal-encode dither enable, read once per process: the TPDF dither the
// encode adds before its quantize is ON, and GE_OUTPUT_DITHER=0 kills it.
//
// It is the standard answer to quantization banding — a 1-LSB triangular
// perturbation applied in the encoded domain immediately before the store, so
// the ROP's rounding error becomes noise instead of a contour. What made it
// unsafe as a default was NOT the dither but its reach: a terminal pass seeing a
// composite in which UI chrome has already been flattened over the world cannot
// tell the two apart, and a fullscreen dither grains flat chrome fills that
// quantize exactly (measured: an editor modal fill going from one value to
// seven). The reach is now settled by WHERE the pass is declared rather than by
// any rect a host passes in: each world view finalizes at its own resolve, so
// the image this pass filters holds world pixels and nothing else, while the
// composite's terminal pass names FinalizeQuantizer::None and filters nothing.
// Amplitude is sized to the destination's real quantizer
// (EncodeDitherLsbForFormat), and targets with no quantizer of their own
// (scRGB/FP16) take 0.
//
// Shared with the editor's CPU capture encode for exactly the reason the
// deband settings below are: on the arms where a capture still runs the filters
// itself (an HDR-display frame, whose composite is linear), a switch the two
// disagreed on would make a GE_OUTPUT_DITHER=0 A/B compare two identically
// dithered captures and read as "the dither changes nothing".
bool IsOutputDitherEnabled();

// THE output format matrix, decided here and nowhere else: one LSB of the step
// `format` actually quantizes at, in output-encoded units. Sizes the dither
// amplitude and the deband's N-LSB gate together, so the two can never disagree
// about how deep the destination is. A float target has no quantizer of its own
// and takes 0, which is why a pass writing one states the step it is really
// sizing for instead of taking its destination's: the Player's FP16 HUD
// composite names FinalizeQuantizer::Presented and dithers at the swapchain's
// step, and its terminal transfer names None (FinalizeContract.h). Anything not
// named falls back to the 8-bit step —
// conservative in ASSUMING a quantizer (imported targets legitimately carry
// Format 0), not in amplitude: a destination deeper than 8 bits and not listed
// here would take an oversized dither, so add the row when one becomes reachable.
// A device with no swapchain reports Unknown and lands in that same fallback, so
// the Presented arm reads 1/255 off-screen. That collides with a Destination arm
// ONLY where the destination is itself 8-bit; against a float destination the
// pair is 1/255 against 0, which a measurement separates easily and
// PresentedSizesFiltersPastAFloatDestination already does. No 8-bit destination
// reaches Presented in tree today. Where one would, amplitude cannot tell the
// arms apart and the declaration's FinalizeParams::PresentedFormat is what
// makes the format read observable.
float EncodeDitherLsbForFormat(TextureFormat format);

// THE requantize decision for a transfer whose source may already hold code
// values, decided here and nowhere else — both hosts' movie encodes AND both
// hosts' terminal transfers take it, so a recording can never disagree with the
// screen about who owns the step, and the two terminals cannot drift from the
// movies. The terminals call it at its degenerate point (source step and
// destination are the same swapchain), where it answers None for an encoded
// composite and Destination for a linear one. The view finalize itself is NOT
// a transfer — its source is linear by definition and its Presented choice is
// the policy's own (ViewFinalize.cpp) — so it is rightly outside this decision.
//
// `sourceIsEncoded` is `inputSpace == FinalizeInputSpace::EncodedSrgb`.
// `sourceStepFormat` is the format whose step those code values were quantized
// to, which is the SWAPCHAIN's and not the source texture's: a finalized F16
// intermediate carries swapchain-stepped values in a float surface, so reading
// the step off the source would report "no quantizer" and always requantize.
// `dstFormat` is what this transfer writes.
//
// A linear source always yields Destination — the transfer IS the
// quantization. An encoded source yields None (move the bytes, filter nothing)
// unless the destination's step is the coarser of the two, the 10-bit screen
// recorded to 8-bit, in which case the transfer owns the step again and BOTH
// filters resize with it: the deband gate counts N LSBs of the target step, so
// sizing the two apart leaves 8-bit-visible staircases standing under a gate
// satisfied at 1/1023, and a 1-LSB dither cannot break steps several LSB tall.
FinalizeQuantizer SelectTransferQuantizer(bool sourceIsEncoded, TextureFormat sourceStepFormat,
                                          TextureFormat dstFormat);

// Terminal-encode deband settings, read once per process from the
// environment: GE_DEBAND=0 kills the filter, GE_DEBAND_THRESHOLD overrides
// the gate threshold (in output LSBs of the active target depth),
// GE_DEBAND_RADIUS overrides the first-iteration tap radius in pixels.
// Shared with the editor's CPU capture encode so screenshots keep matching
// the screen (the same parity contract as the capture dither).
struct OutputDebandSettings
{
	bool Enabled = true;
	float ThresholdLsb = kOutputDebandBaselineThresholdLsb;
	// True only when GE_DEBAND_THRESHOLD supplied ThresholdLsb — the debug
	// override then beats any volume-resolved value in
	// ResolveOutputDebandThresholdLsb.
	bool ThresholdIsDebugOverride = false;
	float RadiusPx = 8.0f;
};
const OutputDebandSettings& GetOutputDebandSettings();

// THE deband precedence, decided here and nowhere else:
//   1. GE_DEBAND=0            → 0 (global kill override — the review-endorsed
//                                debug/A-B instrument and capture-parity gate);
//   2. GE_DEBAND_THRESHOLD=N  → N (debug override, beats any volume);
//   3. otherwise              → the volume-resolved threshold (the
//                                PostProcessVolume lever; baseline 0 — the
//                                filter is opt-in, and a volume's DebandEffect
//                                carries the reviewed 6-LSB default).
// Returns the effective gate in output LSBs; <= 0 means the filter is off.
float ResolveOutputDebandThresholdLsb(float volumeThresholdLsb);

// The effective deband threshold (output LSBs) most recently declared for a
// backbuffer encode — published by AddSRGBEncodePassRG so the editor's CPU
// capture mirror quantizes with EXACTLY the gate the screen used, whatever
// combination of volume/env produced it. Before any backbuffer encode has
// been declared this resolves the baseline through the same precedence.
// (All windows of a process share one primary world in practice; the value is
// process-wide, last declared window wins.)
float GetBackbufferOutputDebandThresholdLsb();

// The output transfer function + dither step most recently declared for a
// backbuffer encode — same publish discipline as
// GetBackbufferOutputDebandThresholdLsb (process-wide, last declared window
// wins). Surfaces what the screen's terminal pass actually used so the debug
// stats endpoint reports facts instead of log archaeology. OutEncoding 0 =
// no backbuffer encode declared yet this process.
struct BackbufferEncodeFacts
{
	int32_t OutEncoding = 0;
	float DitherLsb = 0.0f;
};
BackbufferEncodeFacts GetBackbufferEncodeFacts();

// ── The single terminal encode, declared fresh per frame. ──
// Samples `src` and writes `dst` with the output transfer function selected
// from the device's active HDR mode / swapchain format — the encode shader's
// passthrough mode (outEncoding 4) covers hardware-sRGB swapchains too, so
// this is the ONE terminal pass for every output config. Covered for the
// TRANSFER, not filtered: on that passthrough arm the ROP owns the quantize
// and the pass sizes both dither and deband to zero — dithering a linear
// value for a quantizer that applies the OETF afterwards needs
// slope-compensated noise, which this arm does not carry.
// When dst is the frame's imported backbuffer the attachment uses DontCare
// (the swapchain discard contract); offscreen dsts (movie readback) always
// get the manual sRGB encode, matching the old offscreen overload.
// Declares nothing and returns invalid when shaders are unavailable or
// src == dst.
// The decision operands — input space, quantizer, encode override, deband
// gate, presented format, dither phase — arrive folded in `params` (FinalizeParams,
// FinalizeContract.h), which owns each field's contract. Enforcement that is
// this pass's own: an EncodedSrgb input under an active HDR output mode
// (including via EncodeOverride) is a contract violation — the pass logs an
// error and declares nothing.
//
// Both filters always cover the WHOLE image this pass writes. That is safe
// because every caller hands it one content class: a world view finalizing
// itself, or a composite whose values already sit on the destination step and
// which therefore names FinalizeQuantizer::None. A pass that had to filter
// SOME of its pixels would be a pass declared at the wrong point in the chain.
RenderGraph::RGPass AddSRGBEncodePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                                const FinalizeParams& params,
                                const char* passName = "FinalSRGBEncode");

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine

