#pragma once

#include <cstdint>
#include <optional>

namespace GameEngine {
namespace Rendering {

enum class HdrOutputMode : uint8_t;
enum class TextureFormat : uint32_t;

namespace Passes {

// The Finalize input-space contract (#767 P6b): every selector that keys an
// output encoding off the device takes a FinalizeInputSpace as a REQUIRED
// argument — no default — stating the space its source chain holds, so a new
// call site must decide instead of inheriting a silent assumption.
//
// Linear is what a chain still holding scene-linear light declares: every
// active HDR output mode, and any SDR chain whose world did not finalize ahead
// of it. EncodedSrgb is the SDR blend-space arm, and it is live — the editor's
// per-window terminal encode, the editor's game view, and the Player's HUD
// chain each declare it once their world has finalized before the UI
// composited. Its clauses, where a selector is a terminal encode (it applies
// the OETF and its bytes are presented or captured):
//   - the pass only debands/dithers/requantizes — the transfer function was
//     applied upstream, before the UI blended;
//   - a UNORM destination takes the raw encoded bytes;
//   - an _SRGB destination takes D(c) — the sRGB decode — so the ROP's
//     hardware re-encode round-trips byte-exact (pinned for all 256 levels by
//     SrgbEncodeRampRoundTripTests; a driver that misses byte-exactness fails
//     that gate and the arm must be replaced, never tolerated);
//   - the HDR arms are unreachable: the flip is SDR-only.
//
// Which selectors carry it, and why the value mapping is NOT shared:
//   - AddSRGBEncodePassRG (SRGBEncodePass.cpp) — terminal encode, encode_srgb.frag,
//     keyed on the DESTINATION (backbuffer-ness + dst format). Implements every
//     clause above.
//   - SelectTonemapOutEncoding (TonemapPass.cpp) — NOT a terminal encode:
//     tonemap.frag applies no output transfer function in any arm (its SDR
//     values 0 and 1 both emit display-referred LINEAR and differ only in which
//     pass owns the dither). It takes the argument so a future pipeline that
//     points a Tonemap at a presented surface must state its input space, and
//     it enforces the HDR clause; it cannot implement the requantize clauses
//     because it has no encoded-input arm and does not know its destination.
// The two shaders' outEncoding vocabularies also collide — tonemap.frag's 5 is
// the luminance-heatmap debug arm, encode_srgb.frag's 5 is the raw-requantize
// arm — so a shared value-producing function is not merely a false abstraction
// but impossible. What IS genuinely common is the enum and the enforcement
// clause below, and those are shared so the selectors cannot drift on them.
enum class FinalizeInputSpace : uint8_t
{
	// The poison value, first so it is what value-initialization lands on. It
	// names no space; AddSRGBEncodePassRG refuses to declare and logs. See
	// FinalizeParams below for why a poison first enumerator is the only way an
	// aggregate can make a field mandatory.
	Unspecified = 0,
	Linear,
	EncodedSrgb,
};

// Which quantizer a finalize sizes its filters — deband gate AND TPDF dither —
// to. Stated by the caller for the same reason FinalizeInputSpace is: the pass
// can see its own destination, but not where in a chain it sits.
//
//   Destination — this pass IS the quantization step for the surface it writes.
//                 Every terminal encode; the ordinary case.
//   Presented   — this pass quantizes for the PRESENTED surface while writing an
//                 intermediate the UI still composites onto. An FP16 intermediate
//                 has no step of its own, so sizing to its format would size the
//                 dither to zero and leave the banding for a pass that can no
//                 longer tell world pixels from chrome.
//   None        — the source already holds destination code values. The pass
//                 moves them and filters nothing: dithering an image that is
//                 already exact only re-grains it, and a same-depth requantize
//                 round-trips byte-exact without help.
enum class FinalizeQuantizer : uint8_t
{
	// Poison, for the same reason and with the same refusal.
	Unspecified = 0,
	Destination,
	Presented,
	None,
};

// The deband is OPT-IN: volume-less worlds, legacy scenes, and call sites
// without resolved post settings run with the filter OFF (0). Enabling it is
// a per-volume choice via Components::DebandEffect, whose ThresholdLsb default
// carries the reviewed 6-LSB gate. Keep in lockstep with
// PostProcessSettings::DebandThresholdLsb.
inline constexpr float kOutputDebandBaselineThresholdLsb = 0.0f;

// The decision operands of a finalize declaration (AddSRGBEncodePassRG),
// folded into one aggregate so a call site names what it states — designated
// initializers — instead of counting defaulted positions.
//
// InputSpace and Quantizer follow the required-argument rule above and carry
// NO default member initializer: every declaration site spells both out.
// An aggregate cannot make a field mandatory — a designated initializer that
// omitted one value-initializes it to the FIRST enumerator — so both enums put
// a poison `Unspecified` there and the pass refuses to declare on either. The
// omission that the type system cannot reject becomes a missing terminal
// encode plus a log line naming the field, instead of a silently inherited
// Linear/Destination. Write them.
// The remaining operands default to the ordinary terminal encode.
struct FinalizeParams
{
	FinalizeInputSpace InputSpace;
	FinalizeQuantizer Quantizer;

	// Forces a specific output transfer function instead of the default
	// selection (backbuffer: the display's active mode; offscreen: sRGB). Used
	// for HDR movie capture, where an offscreen 10-bit target must be PQ/HLG-
	// encoded even though it isn't the swapchain.
	std::optional<HdrOutputMode> EncodeOverride = std::nullopt;

	// The world's resolved deband gate (PostProcessSettings::DebandThresholdLsb);
	// env overrides are applied on top via ResolveOutputDebandThresholdLsb. The
	// default keeps call sites without resolved post settings (tests, examples)
	// deband-OFF (opt-in filter).
	float VolumeDebandThresholdLsb = kOutputDebandBaselineThresholdLsb;

	// The format FinalizeQuantizer::Presented sizes its filters to, in place of
	// the device's live swapchain format. It pairs with EncodeOverride — both
	// let a caller state an output property the device cannot report correctly
	// for this pass. Who supplies it:
	// - PRODUCTION Presented callers, always ENGAGED: the window owner resolves
	//   its own target's format per frame (IDevice::GetWindowTargetSwapchainFormat)
	//   and threads it here, so the step never rides whichever window target
	//   happens to be active on a shared device (DeclareViewFinalize and the
	//   Player's finalize both do).
	// - Headless suites asserting an absolute Presented amplitude: with no
	//   swapchain GetSwapchainTextureFormat() returns Unknown and the format
	//   matrix reads Unknown as the 8-bit step, so only a supplied format reaches
	//   the true presented step off-screen.
	// Unset is exactly the device read — the module-test arm that pins the
	// ambient mechanism itself. An Unknown resolve while the device presents a
	// real format warns once (the wrong-owner/unmapped-format class); Unknown
	// against a swapchain-less device is the matrix's stated row and stays silent.
	std::optional<TextureFormat> PresentedFormat = std::nullopt;

	// Which realisation of the TPDF dither this declaration takes, in [0,1).
	// Defaulted — unlike the two operands above — because 0 is the screen-space
	// static pattern every path shipped with, so a call site that says nothing
	// is saying the right thing rather than inheriting a guess. Only the
	// experimental temporal-dither toggles supply a nonzero value, and only
	// through Passes::MovieDitherPhase / ScreenDitherPhase (TemporalDither.h),
	// which own the seed→phase mapping for both hosts. The pass forces it back
	// to 0 wherever it does not own the quantization step, so a phase can never
	// reach a transfer that filters nothing.
	float DitherPhase = 0.0f;
};

// The contract's load-bearing enforcement clause, shared so the selectors
// cannot drift apart on it: an already-encoded input may never feed an active
// HDR output mode — the transfer function it carries is sRGB, which no HDR arm
// can requantize. On true, the caller must refuse or fall back LOUDLY.
inline bool IsFinalizeSpaceHdrViolation(FinalizeInputSpace space, bool hdrOutputActive)
{
	return space == FinalizeInputSpace::EncodedSrgb && hdrOutputActive;
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
