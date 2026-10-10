#include "Rendering/Passes/SRGBEncodePass.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <cstdint>
#include <cstddef>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"

namespace GameEngine {
namespace Rendering {
namespace Passes {

void CleanupSRGBEncodePassForDevice(IDevice* device);

struct SRGBEncodeDeviceResources
{
	SamplerHandle sampler{INVALID_SAMPLER_HANDLE};
};

struct SRGBEncodeShaderBytes
{
	std::vector<uint8_t> vs;
	std::vector<uint8_t> fs;
	// Reflection meta — the single source of truth for the descriptor-set
	// layout and push-constant block (never hand-mirrored from encode_srgb.frag;
	// see TonemapPass for the drift-class rationale).
	ShaderMeta meta;
	bool loaded = false;
	bool warned = false;
};

// encode_srgb.frag PC members written by name:
//   outEncoding        1=sRGB, 2=PQ, 3=HLG, 4=scRGB/linear passthrough,
//                      5=encoded input requantized raw (UNORM dst),
//                      6=encoded input decoded D(c) (_SRGB dst; ROP re-encodes)
//   paperWhiteNits     SDR white level in HDR output modes
//   ditherLsb          dither step: one LSB of the destination's quantizer
//                      (1/255 8-bit, 1/1023 10-bit); 0 disables
//   scRGBRefWhiteNits  scRGB anchor: encode scales by paperWhite / max(this, 80).
//                      Sourced from GetScRGBFramebufferWhiteNits (80 on Windows;
//                      the paper white on macOS, for Metal and MoltenVK alike).
//                      Left at 0 it floors to 80 — correct on Windows, ~2.5x too
//                      bright on macOS.
//   debandThreshold    deband gate in output-encoded units (N output LSBs);
//                      <= 0 disables the filter (uniform branch, taps skipped)
//   debandRadius       first-iteration deband tap radius in pixels
//   ditherPhase        temporal dither realisation in [0,1); 0 = the static
//                      screen-space pattern (TemporalDither.h)
struct OutputEncodeValues
{
	int32_t outEncoding = 1;
	float paperWhiteNits = 203.0f;
	float ditherLsb = 1.0f / 255.0f;
	float scRGBRefWhiteNits = 80.0f;
	float debandThreshold = 0.0f;
	float debandRadius = 0.0f;
	float ditherPhase = 0.0f;
};

// Triangular-dither amplitude: one LSB of the step the destination actually
// quantizes at.
//
// Sizing to the destination and not to a guess about the display link is the
// rule. An 8-bit amplitude on a 10-bit target is 4x the real quantizer, and the
// excess is not free: on flat dark UI it turns an exactly flat fill into 7
// distinct values at +5.9% relative luminance (measured on the editor's modal
// surfaces). The tempting counter-argument — that the compositor and display
// link commonly requantize to 8 bits with no dither of their own, so a 10-bit
// TPDF rounds away downstream — was measured and did not hold up: run-length
// stats of the presented output observed at 8 bits were identical with and
// without the 4x amplitude, while the staircase it was meant to break
// persisted (that staircase is an upstream several-LSB band class, which the
// deband owns and no 1-LSB dither can break). If an 8-bit sink ever does need
// covering, that belongs in a display-chain setting, not a blanket 4x here.
inline constexpr float kEncodeDitherLsb8Bit = 1.0f / 255.0f;
inline constexpr float kEncodeDitherLsb10Bit = 1.0f / 1023.0f;

float EncodeDitherLsbForFormat(TextureFormat format)
{
	switch (format)
	{
	case TextureFormat::RGB10A2_UNORM:
		return kEncodeDitherLsb10Bit;
	case TextureFormat::R16G16B16A16_FLOAT:
	case TextureFormat::R32G32B32A32_FLOAT:
	case TextureFormat::R11G11B10_FLOAT:
		return 0.0f;
	// Unknown is a stated row, not a fall-through. Sizing an undescribed
	// destination at 8 bits is the conservative choice — assume a quantizer
	// exists rather than silently drop the dither — but how good a guess it is
	// depends on WHY the format is Unknown, and the two swapchain-side producers
	// are not alike:
	//   - no swapchain at all (headless, or before a window target attaches):
	//     GetSwapchainTextureFormat() has nothing to describe and nothing is
	//     presented, so the guess costs nothing;
	//   - a swapchain whose VkFormat has no FromVkFormat row (VulkanDevice.cpp):
	//     the surface may well be 10-bit, and 1/255 is 4x its real step. That
	//     host is already worse off than the amplitude — SwapchainNeedsManual-
	//     SRGBEncode() reads the same Unknown as "no manual encode" — so the fix
	//     is the missing FromVkFormat row, never a different guess here.
	// An imported render target that carries no declared format reaches this row
	// too, by the same 8-bit default.
	case TextureFormat::Unknown:
		return kEncodeDitherLsb8Bit;
	default:
		return kEncodeDitherLsb8Bit;
	}
}

FinalizeQuantizer SelectTransferQuantizer(bool sourceIsEncoded, TextureFormat sourceStepFormat,
                                          TextureFormat dstFormat)
{
	// A linear source means this transfer IS the quantization.
	if (!sourceIsEncoded)
		return FinalizeQuantizer::Destination;
	const bool dstStepIsCoarser =
		EncodeDitherLsbForFormat(sourceStepFormat) < EncodeDitherLsbForFormat(dstFormat);
	return dstStepIsCoarser ? FinalizeQuantizer::Destination : FinalizeQuantizer::None;
}

// The terminal-encode dither is ON; GE_OUTPUT_DITHER=0 is the A/B kill switch.
// What made a fullscreen dither unsafe as a default was its reach, not the
// dither: graining flat chrome that quantizes exactly is pure loss. Reach is now
// a property of WHERE a finalize is declared — a world view's own resolve sees
// no chrome at all — so that class is gone by construction rather than by
// amplitude tuning or by any rect the host passes in. Read once per process;
// declared in the header so the editor's capture mirror gates on the same
// switch.
bool IsOutputDitherEnabled()
{
	static const bool s_Enabled = []
	{
		const char* value = std::getenv("GE_OUTPUT_DITHER");
		return !(value && value[0] == '0' && value[1] == '\0');
	}();
	return s_Enabled;
}

// Deband defaults (the OutputDebandSettings initializers), sized to the
// measured band class (Demo_unity forensics and a captured banding reference):
// Synty's 8-bit-authored albedo gradients step 1 count per 4-6
// texels; under exposure gain each step lands on screen several output LSBs
// tall with plateaus tens of pixels wide — verified instances span 1-7 LSB
// (edge-free strips of the reference measure 4-5-LSB steps; the oft-quoted
// larger numbers turned out to be thickness-averaged strips sweeping real
// edges, which this filter must NOT touch). A rotated 4-tap cross straddling
// one boundary sees |center - avg| in {0, s/4, s/2}, so the 6-LSB gate covers
// steps to ~10 LSB with noise margin while real edges and texture detail
// (tens of LSBs in at least one channel) stay shut — replaying the filter
// over the reference image left its plank-edge contrast unchanged (30.4 LSB)
// at gates up to 10. The 8 px radius grows per iteration (8/16/24 max reach)
// to cover plateau widths, which scale with camera distance —
// GE_DEBAND_RADIUS is the lever when they don't.
const OutputDebandSettings& GetOutputDebandSettings()
{
	static const OutputDebandSettings s_Settings = []
	{
		OutputDebandSettings s{};
		if (const char* value = std::getenv("GE_DEBAND"); value && value[0] == '0' && value[1] == '\0')
			s.Enabled = false;
		if (const char* value = std::getenv("GE_DEBAND_THRESHOLD"))
		{
			char* end = nullptr;
			const float parsed = std::strtof(value, &end);
			if (end != value)
			{
				s.ThresholdLsb = std::clamp(parsed, 0.0f, 16.0f);
				s.ThresholdIsDebugOverride = true;
			}
		}
		if (const char* value = std::getenv("GE_DEBAND_RADIUS"))
		{
			char* end = nullptr;
			const float parsed = std::strtof(value, &end);
			if (end != value)
				s.RadiusPx = std::clamp(parsed, 1.0f, 64.0f);
		}
		return s;
	}();
	return s_Settings;
}

float ResolveOutputDebandThresholdLsb(float volumeThresholdLsb)
{
	const OutputDebandSettings& env = GetOutputDebandSettings();
	if (!env.Enabled)
		return 0.0f;
	if (env.ThresholdIsDebugOverride)
		return env.ThresholdLsb;
	return std::clamp(volumeThresholdLsb, 0.0f, 16.0f);
}

// Backbuffer effective-threshold publish for the capture mirror (see the
// header contract). Written at declaration on the render thread, read by the
// DebugServer's deferred capture conversion — hence atomic. Negative sentinel
// = "no backbuffer encode declared yet this process".
static std::atomic<float> g_BackbufferDebandThresholdLsb{-1.0f};

float GetBackbufferOutputDebandThresholdLsb()
{
	const float published = g_BackbufferDebandThresholdLsb.load(std::memory_order_relaxed);
	return published >= 0.0f ? published
	                         : ResolveOutputDebandThresholdLsb(kOutputDebandBaselineThresholdLsb);
}

// Backbuffer encode-facts publish (see the header contract): written at
// declaration on the render thread, read by the DebugServer stats endpoint —
// hence atomics. Two independent relaxed atomics, matching the deband
// publish above; a debug reader tolerates a one-declaration tear.
static std::atomic<int32_t> g_BackbufferOutEncoding{0};
static std::atomic<float> g_BackbufferDitherLsb{0.0f};

BackbufferEncodeFacts GetBackbufferEncodeFacts()
{
	BackbufferEncodeFacts facts;
	facts.OutEncoding = g_BackbufferOutEncoding.load(std::memory_order_relaxed);
	facts.DitherLsb = g_BackbufferDitherLsb.load(std::memory_order_relaxed);
	return facts;
}

// Cache SRGB encode resources per logical device so handles are never reused
// across devices. Note this is NOT a per-window bucket: the editor's extra
// windows ATTACH to the main window's device
// (RenderDeviceContext::InitParams::sharedDevice — EditorApplication_Windows.cpp:312,
// ColorPickerWindow.cpp:258), so every editor window lands in the same bucket.
// The key earns its keep only across genuinely separate devices (a device-lost
// recreate, a standalone Player device in-process). That is exactly why the
// encode-facts log gate below keys on the RGFrame stream instead: per-window
// state has to key on something that is actually per-window.
using DeviceKey = IDevice*;
static std::unordered_map<DeviceKey, SRGBEncodeDeviceResources> g_SrgbEncodePerDevice;

// Change gate for the backbuffer encode-facts log, keyed PER FRAME STREAM.
//
// The gate state cannot be process-wide: every window presents its own
// backbuffer through this pass, and the logged tuple is a property of that
// target — its format and the quantizer its host declared. One shared
// last-logged tuple flips on every alternation between windows whose targets
// disagree, which logs every frame from every window and evicts the rest of
// the log ring within seconds.
//
// The key is the RGFrame, not the IDevice: floating windows ATTACH to the main
// window's device (RenderDeviceContext::InitParams::sharedDevice), so a
// per-device key is one bucket for all windows and would not gate anything.
// The frame stream is 1:1 with the presented target — RGFrame is the engine's
// established stream identity (see RGFrame.h's (RGFrame*, FrameIndex) pairing).
// Diagnostic state only, never read by the publish endpoints above. Touched at
// declaration and only on the backbuffer path, which the editor walks one window
// at a time on its render thread — so this map needs no lock, but it does assume
// that serialization (unlike the publishes above, a map insert that raced would
// corrupt, not merely tear).
struct LoggedEncodeFacts
{
	int32_t Encoding = -1;
	float DitherLsb = -1.0f;
	float DebandThreshold = -1.0f;
};
static std::unordered_map<const RenderGraph::RGFrame*, LoggedEncodeFacts> g_LoggedEncodeFactsPerFrame;

// Pass names that have already reported an encoded rescale. Keyed on the NAME
// alone and not on the extents: a key carrying extents is unbounded, so an
// extent-varying source would mint a fresh key and emit another Log::Error every
// time — and Error is the level at which the logger attaches a 12-frame
// symbolized backtrace, which is the exact cost the latch exists to avoid.
// Cleared beside the facts map when the device's GPU objects die, for the same
// reason: the first offending encode after a rebuild should say so again.
static std::vector<std::string> g_ReportedRescalePasses;

static SRGBEncodeShaderBytes& GetSRGBEncodeShaderBytes(const IDevice& device)
{
	static SRGBEncodeShaderBytes s;
	if (!s.loaded)
	{
		ShaderPackage pkg{};
		std::string err;
		if (LoadShaderPkg("Shaders/encode_srgb.shaderpkg", device.PreferredShaderSource(), pkg, &err))
		{
			auto itVs = pkg.stageBytes.find("vs");
			auto itFs = pkg.stageBytes.find("fs");
			if (itVs != pkg.stageBytes.end())
				s.vs = std::move(itVs->second);
			if (itFs != pkg.stageBytes.end())
				s.fs = std::move(itFs->second);
			s.meta = std::move(pkg.meta);
		}
		// Latch only on success: this is the TERMINAL encode — a one-shot
		// failure latch here is a permanently black swapchain with no log.
		// A not-yet-ready loader environment retries; the warning fires once.
		s.loaded = !s.vs.empty() && !s.fs.empty();
		if (!s.loaded && !s.warned)
		{
			s.warned = true;
			Logger::Log::Warning(
				"SRGBEncodePass: Shaders/encode_srgb.shaderpkg unavailable ({}); terminal "
				"encode deferred (retrying)",
				err);
		}
	}
	return s;
}

static SamplerHandle GetOrCreateSRGBEncodeSampler(IDevice* device)
{
	if (!device)
	{
		return {};
	}
	auto [it, inserted] = g_SrgbEncodePerDevice.try_emplace(device);
	if (inserted)
	{
		// Drop this per-device cache whenever the device's GPU objects die —
		// shutdown OR an in-place rebuild after device loss. The cached sampler
		// belongs to one VkDevice; a rebuild destroys it while leaving this
		// IDevice* key alive, so the cache would keep serving a dead handle.
		device->RegisterPerDeviceCacheCleanup(
			"Passes.SRGBEncode",
			[](IDevice* d)
			{
				CleanupSRGBEncodePassForDevice(d);
			});
	}

	auto& res = it->second;
	if (res.sampler.IsValid())
		return res.sampler;

	SamplerDesc samplerDesc{};
	samplerDesc.minFilter = 1;
	samplerDesc.magFilter = 1;
	samplerDesc.mipFilter = 1;
	samplerDesc.addressModeU = 2; // clamp
	samplerDesc.addressModeV = 2;
	samplerDesc.addressModeW = 2;
	samplerDesc.debugName = "SRGBEncode.LinearClamp";
	res.sampler = device->CreateSampler(samplerDesc);
	return res.sampler;
}

void CleanupSRGBEncodePassForDevice(IDevice* device)
{
	if (!device)
		return;

	auto it = g_SrgbEncodePerDevice.find(device);
	if (it == g_SrgbEncodePerDevice.end())
		return;

	SRGBEncodeDeviceResources& res = it->second;
	if (res.sampler.IsValid())
	{
		device->DestroySampler(res.sampler);
		res.sampler = SamplerHandle{};
	}
	g_SrgbEncodePerDevice.erase(it);

	// The log gate keys on frame streams, which have no device affinity to
	// unwind here (windows share one device). Device teardown and rebuild are
	// the coarse reset points available to this pass: dropping the gate state means
	// the first encode after a device rebuild logs its facts once again, which
	// is what a diagnostic wants. Entries are a few bytes and only ever one per
	// live presented target, so nothing accumulates in between.
	g_LoggedEncodeFactsPerFrame.clear();
	// Same reasoning, same reset point: an encoded rescale that survives a device
	// rebuild is still a defect, and a latch that outlived the device would hide
	// it for the rest of the process.
	g_ReportedRescalePasses.clear();
}

RenderGraph::RGPass AddSRGBEncodePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                                const FinalizeParams& params, const char* passName)
{
	IDevice* device = frame.Device();
	if (!device || !src.IsValid() || !dst.IsValid() || src.Id == dst.Id)
		return {};
	// The two required operands, enforced. An aggregate cannot make a field
	// mandatory, so a designated initializer that omits one arrives as the
	// poison first enumerator (FinalizeContract.h). Refusing to declare is this
	// pass's loud channel and the right one here too: a frame missing its
	// terminal encode is unmissable, a frame silently encoded under an
	// inherited assumption is not.
	if (params.InputSpace == FinalizeInputSpace::Unspecified ||
	    params.Quantizer == FinalizeQuantizer::Unspecified)
	{
		static bool s_WarnedUnspecified = false;
		if (!s_WarnedUnspecified)
		{
			s_WarnedUnspecified = true;
			Logger::Log::Error(
				"SRGBEncodePass: '{}' declared with {}{}{} unspecified — both are required "
				"operands (FinalizeContract.h); pass not declared. Name them in the "
				"FinalizeParams initializer",
				passName ? passName : "FinalSRGBEncode",
				params.InputSpace == FinalizeInputSpace::Unspecified ? "InputSpace" : "",
				(params.InputSpace == FinalizeInputSpace::Unspecified &&
				 params.Quantizer == FinalizeQuantizer::Unspecified)
					? " and "
					: "",
				params.Quantizer == FinalizeQuantizer::Unspecified ? "Quantizer" : "");
		}
		return {};
	}
	const auto& shaders = GetSRGBEncodeShaderBytes(*device);
	if (shaders.vs.empty() || shaders.fs.empty())
		return {};

	// Layout + push-constant block derived from the shaderpkg's reflection meta.
	// Interned per device; the per-frame cost is the variant-cache hit.
	GraphicsPipelineDesc gd{};
	gd.Kind = GraphicsPipelineKind::VertexFragment;
	gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(shaders.vs);
	gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(shaders.fs);
	gd.Topology = PrimitiveTopology::TriangleList;
	gd.Rasterization.cullMode = CullModeFlagBits::None;
	gd.Rasterization.frontFace = FrontFace::CounterClockwise;
	gd.DepthStencil.depthTestEnable = false;
	gd.DepthStencil.depthWriteEnable = false;
	DynamicStateInfo dyn{};
	dyn.states.push_back(DynamicState::Viewport);
	dyn.states.push_back(DynamicState::Scissor);
	gd.DynamicState = std::move(dyn);
	gd.DebugName = "SRGBEncode.Pipeline";
	DescriptorSetLayoutDesc set0{};
	{
		std::string metaErr;
		if (!MaterialHelper::ApplyShaderMetaToGraphicsDesc(
				*device, shaders.meta, gd, MaterialBuilder::MergeMode::Auto, {true, 128},
				[&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
				{
					if (setIndex == 0)
						set0 = dsl;
				},
				&metaErr))
		{
			static bool s_WarnedMeta = false;
			if (!s_WarnedMeta)
			{
				s_WarnedMeta = true;
				Logger::Log::Warning("SRGBEncodePass: shader meta rejected ({})", metaErr);
			}
			return {};
		}
	}
	const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(gd));
	if (!pipelineId.IsValid())
		return {};

	// Encoding decided at DECLARATION (the by-value contract); the dither
	// step is sized to the real destination bit depth.
	const bool dstIsBackbuffer = frame.IsBackbuffer(dst);
	OutputEncodeValues pc{};
	const HdrOutputState hdrState = device->GetHdrOutputState();
	pc.paperWhiteNits = hdrState.staticMetadata.paperWhiteNits;
	pc.scRGBRefWhiteNits = GetScRGBFramebufferWhiteNits(hdrState);
	// HDR transfer functions (PQ/HLG/scRGB) are display-referred — they belong
	// only on the swapchain we actually present. An offscreen target (movie /
	// screenshot / thumbnail readback into an 8-bit SDR texture) must always get
	// plain sRGB: otherwise the readback captures display-encoded pixels and the
	// saved file is wrong (washed out under scRGB, crushed under PQ/HLG).
	// `EncodeOverride` opts a specific offscreen target back into an HDR encode
	// on purpose (HDR movie capture into a 10-bit PQ/HLG target).
	const HdrOutputMode outputMode =
		params.EncodeOverride.has_value()
			? *params.EncodeOverride
			: (dstIsBackbuffer ? device->GetActiveHdrOutputMode() : HdrOutputMode::Off);
	// Finalize contract (FinalizeContract.h): an already-encoded input is
	// SDR-only — the transfer function it carries is sRGB, which no HDR arm
	// can requantize. Refusing to declare is this pass's loud channel; the
	// frame's missing terminal encode is unmissable, a silently wrong
	// re-encode is not.
	if (IsFinalizeSpaceHdrViolation(params.InputSpace, IsHdrOutputModeActive(outputMode)))
	{
		static bool s_WarnedEncodedHdr = false;
		if (!s_WarnedEncodedHdr)
		{
			s_WarnedEncodedHdr = true;
			Logger::Log::Error(
				"SRGBEncodePass: '{}' declared FinalizeInputSpace::EncodedSrgb under active HDR "
				"output mode {} — the encoded arm is SDR-only; pass not declared",
				passName ? passName : "FinalSRGBEncode", HdrOutputModeToString(outputMode));
		}
		return {};
	}
	const bool ditherEnabled = IsOutputDitherEnabled();
	// The destination this pass writes: the swapchain's format when presenting,
	// the declared format otherwise. This picks the ENCODING arm (an _SRGB dst
	// re-encodes in its ROP) regardless of which quantizer the caller named.
	const TextureFormat dstFormat =
		dstIsBackbuffer ? device->GetSwapchainTextureFormat()
		                : static_cast<TextureFormat>(frame.Graph().ResourceDesc(dst.Id).Format);
	// The quantizer the FILTERS are sized to, which is not always the same
	// surface (FinalizeQuantizer): a finalize writing an FP16 intermediate the
	// UI still composites onto must dither at the presented step, and a transfer
	// of values that already sit on that step must not dither at all.
	const float dstQuantizerLsb = [&]
	{
		switch (params.Quantizer)
		{
		case FinalizeQuantizer::Unspecified: // refused above; kept exhaustive
		case FinalizeQuantizer::None:
			return 0.0f;
		case FinalizeQuantizer::Presented:
		{
			// `PresentedFormat` is the format a windowed host would have
			// reported; unset takes the live swapchain. A host without one
			// reports Unknown, which the matrix reads as the 8-bit step.
			const TextureFormat resolved =
				params.PresentedFormat.value_or(device->GetSwapchainTextureFormat());
			// Warn-once, DISCRIMINATING: fires only when the resolved format is
			// Unknown while the device itself presents a real one — a caller
			// that supplied the wrong window's handle, or an unmapped swapchain
			// VkFormat. Headless suites resolve Unknown while the device also
			// reports Unknown; that is the matrix's stated no-surface row, not
			// a defect, and stays silent.
			if (resolved == TextureFormat::Unknown &&
			    device->GetSwapchainTextureFormat() != TextureFormat::Unknown)
			{
				static bool s_WarnedUnknownPresented = false;
				if (!s_WarnedUnknownPresented)
				{
					s_WarnedUnknownPresented = true;
					Logger::Log::Warning(
						"SRGBEncodePass: '{}' names FinalizeQuantizer::Presented with an Unknown "
						"presented format while the device presents {} — the dither sizes to the "
						"8-bit fallback row. Fix the producer: supply the window's own format "
						"(IDevice::GetWindowTargetSwapchainFormat), or add the missing "
						"FromVkFormat row",
						passName ? passName : "FinalSRGBEncode",
						ToString(device->GetSwapchainTextureFormat()));
				}
			}
			return EncodeDitherLsbForFormat(resolved);
		}
		case FinalizeQuantizer::Destination:
			break;
		}
		return EncodeDitherLsbForFormat(dstFormat);
	}();
	switch (outputMode)
	{
	case HdrOutputMode::HDR10_PQ:
	case HdrOutputMode::HDR10Plus:
		pc.outEncoding = 2;
		break;
	case HdrOutputMode::HLG:
		pc.outEncoding = 3;
		break;
	case HdrOutputMode::ScRGB:
		pc.outEncoding = 4;
		break;
	default:
		if (params.InputSpace == FinalizeInputSpace::EncodedSrgb)
		{
			// Encoded input: this pass only debands/dithers/requantizes.
			// Keyed on the DESTINATION format, not backbuffer-ness — the
			// contract is a property of what quantizes the bytes: a UNORM
			// dst stores them raw (5); an _SRGB dst re-encodes in the ROP,
			// so the shader hands it D(c) and the round-trip E(D(c)) is
			// pinned byte-exact by SrgbEncodeRampRoundTripTests (6). An
			// unknown format is treated as UNORM: raw bytes, never a decode
			// the dst might not undo.
			const bool dstIsSrgbRop = dstFormat == TextureFormat::RGBA8_SRGB ||
			                          dstFormat == TextureFormat::BGRA8_SRGB;
			pc.outEncoding = dstIsSrgbRop ? 6 : 5;
		}
		else
		{
			// SDR: manual sRGB on UNORM swapchains / offscreen targets, linear
			// passthrough on hardware-sRGB swapchains (see the old pass's
			// rationale — keying an encode there would double-encode).
			pc.outEncoding =
				(!dstIsBackbuffer || device->SwapchainNeedsManualSRGBEncode()) ? 1 : 4;
		}
		break;
	}
	// ── One quantizer step, one sizing rule, every arm. ──
	// Arms 1/5/6 quantize in the sRGB-encoded domain at the destination's step;
	// PQ/HLG quantize their own encoded domain at the same destination step
	// (RGB10A2 → 1/1023, and an FP16 HDR swapchain correctly yields 0). Arm 4 is
	// the pass's only non-quantizing output — scRGB extended-linear, and the
	// hardware-sRGB passthrough where the ROP owns the transfer function — so it
	// takes neither filter: dithering a linear value for a quantizer that
	// applies the OETF afterwards needs slope-compensated noise, which belongs
	// with whoever writes that surface, not here.
	const float outputLsb = pc.outEncoding == 4 ? 0.0f : dstQuantizerLsb;
	pc.ditherLsb = ditherEnabled ? outputLsb : 0.0f;
	// A phase only means something to a dither that runs. Zeroing it wherever
	// this pass filters nothing keeps the declaration honest — a transfer that
	// moves values already on the destination step is byte-identical whatever
	// its caller passed, by construction rather than by the shader happening to
	// skip the branch — and it is what makes "same recording, toggle on vs off"
	// on a same-depth movie a provable no-op instead of an observed one.
	pc.ditherPhase = pc.ditherLsb > 0.0f ? params.DitherPhase : 0.0f;
	// The deband gate counts N LSBs of that same step; the count itself is the
	// resolved volume/env precedence (ResolveOutputDebandThresholdLsb). 0 leaves
	// the push constant at 0 — the shader's dynamically uniform off branch.
	const float effectiveDebandLsb = ResolveOutputDebandThresholdLsb(params.VolumeDebandThresholdLsb);
	if (effectiveDebandLsb > 0.0f)
	{
		pc.debandThreshold = effectiveDebandLsb * outputLsb;
		pc.debandRadius = GetOutputDebandSettings().RadiusPx;
	}
	// Values -> push constants by MEMBER NAME against the reflected block,
	// resolved at declaration (the by-value contract). A silently dropped
	// write leaves the block zeroed: outEncoding 0 falls through to the
	// shader's manual-sRGB arm with dither off — a correct-looking image
	// with the dither dead, so failures here must be loud.
	NamedPushConstantWriter pcw =
		shaders.meta.PushConstants.empty()
			? NamedPushConstantWriter(shaders.meta, std::string{})
			: NamedPushConstantWriter(shaders.meta, shaders.meta.PushConstants[0].Name);
	bool wroteAllPc = pcw.IsValid();
	if (pcw.IsValid())
	{
		wroteAllPc &= pcw.Add("outEncoding", pc.outEncoding);
		wroteAllPc &= pcw.Add("paperWhiteNits", pc.paperWhiteNits);
		wroteAllPc &= pcw.Add("ditherLsb", pc.ditherLsb);
		wroteAllPc &= pcw.Add("scRGBRefWhiteNits", pc.scRGBRefWhiteNits);
		wroteAllPc &= pcw.Add("debandThreshold", pc.debandThreshold);
		wroteAllPc &= pcw.Add("debandRadius", pc.debandRadius);
		wroteAllPc &= pcw.Add("ditherPhase", pc.ditherPhase);
	}
	if (dstIsBackbuffer)
	{
		// Publish the effective gate for the capture mirror (LSB domain, not
		// the encoded-domain push constant — captures always quantize 8-bit)
		// and the encode facts for the debug stats endpoint.
		g_BackbufferDebandThresholdLsb.store(effectiveDebandLsb, std::memory_order_relaxed);
		g_BackbufferOutEncoding.store(pc.outEncoding, std::memory_order_relaxed);
		g_BackbufferDitherLsb.store(pc.ditherLsb, std::memory_order_relaxed);
		LoggedEncodeFacts& logged = g_LoggedEncodeFactsPerFrame[&frame];
		if (pc.outEncoding != logged.Encoding || pc.ditherLsb != logged.DitherLsb ||
		    pc.debandThreshold != logged.DebandThreshold)
		{
			logged.Encoding = pc.outEncoding;
			logged.DitherLsb = pc.ditherLsb;
			logged.DebandThreshold = pc.debandThreshold;
			Logger::Log::Info(
				"SRGBEncodePass: backbuffer outEncoding={} ditherLsb={:.6f} debandThreshold={:.6f} "
				"debandRadius={:.1f} mode={} pcValid={} pcWroteAll={}",
				pc.outEncoding, pc.ditherLsb, pc.debandThreshold, pc.debandRadius,
				HdrOutputModeToString(outputMode), pcw.IsValid(), wroteAllPc);
		}
	}
	if (!wroteAllPc)
	{
		static bool s_WarnedPcWrite = false;
		if (!s_WarnedPcWrite)
		{
			s_WarnedPcWrite = true;
			Logger::Log::Warning(
				"SRGBEncodePass: push-constant write dropped (reflected block/member mismatch); "
				"terminal encode running with zeroed constants");
		}
	}

	const RenderGraph::RGResourceDesc dd = frame.Graph().ResourceDesc(dst.Id);
	const uint32_t w = dd.Width > 0 ? dd.Width : 1u;
	const uint32_t h = dd.Height > 0 ? dd.Height : 1u;

	// I11, and this pass is the only transport that can break it. An encoded
	// source arrives already dithered at the presented quantisation step, and
	// this pass samples through a LINEAR sampler (GetOrCreateSRGBEncodeSampler:
	// min/mag/mipFilter = 1). At matched extent with aligned texel centres that
	// sampler returns exact texels and the transfer is byte-clean; the moment the
	// extents differ it becomes a bilinear rescale, which low-passes the dither
	// and leaves the recovered detail to be re-rounded WITHOUT dither. The
	// pixel-perfect upscale is exempt for the reason this is not: it samples
	// nearest (PixelPerfectUpscalePass NearestClamp), so it replicates texels
	// instead of mixing them.
	//
	// The check lives here rather than in the render graph because the deciding
	// property is this pass's own sampler, which no declaration carries. Reported
	// rather than refused: dropping the terminal encode would black the display
	// instead of naming the defect, and a correct chain never reaches this.
	// `Quantizer == None` is half the condition, not a detail. When this pass owns
	// the step it re-dithers at the DESTINATION's resolution (dstQuantizerLsb
	// above) and encode_srgb.frag applies that dither AFTER its sample, so a
	// rescale under FinalizeQuantizer::Destination is followed by a fresh dither
	// sized to what actually quantizes — no loss, and nothing to report. The
	// damage is specific to the case where the source arrives already dithered
	// and NOTHING re-dithers afterwards, which is exactly `None`.
	if (params.InputSpace == FinalizeInputSpace::EncodedSrgb &&
	    params.Quantizer == FinalizeQuantizer::None)
	{
		const RenderGraph::RGResourceDesc sd = frame.Graph().ResourceDesc(src.Id);
		if (sd.Width != dd.Width || sd.Height != dd.Height)
		{
			const char* name = passName ? passName : "FinalSRGBEncode";
			if (std::find(g_ReportedRescalePasses.begin(), g_ReportedRescalePasses.end(), name) ==
			    g_ReportedRescalePasses.end())
			{
				g_ReportedRescalePasses.emplace_back(name);
				Logger::Log::Error(
					"SRGBEncodePass: '{}' rescales an ENCODED source {}x{} -> {}x{} through a "
					"linear sampler, and nothing re-dithers afterwards (this transfer does not own "
					"the step) — the finalize's dither is sized to the SOURCE's quantisation step, "
					"so the resample low-passes it and the recovered detail is re-rounded "
					"undithered (I11). Either give this pass a destination at the source's extent "
					"and rescale outside the dithered domain, or finalize again at the destination "
					"extent so the dither is sized to it",
					name, sd.Width, sd.Height, dd.Width, dd.Height);
			}
		}
	}

	return frame.AddPass(
		passName ? passName : "FinalSRGBEncode", PassPhase::kFinalize,
		[&](RenderGraph::RGPassBuilder& p)
		{
			p.Read(src, RenderGraph::RGTextureRead::Sampled);
			RenderGraph::RGAttachmentOps ops{};
			// Full-screen overwrite; the imported backbuffer's contract is
			// discard-on-first-write (its contents are presentation history).
			ops.Load = RenderGraph::RGLoadOp::DontCare;
			ops.Store = RenderGraph::RGStoreOp::Store;
			p.AttachColor(0, dst, ops);
		},
		[pipelineId, src, set0, pcw, meta = &shaders.meta, w, h](RenderGraph::RGContext& ctx)
		{
			auto* cl = ctx.Cmd;
			auto* dev = ctx.GetDevice();
			if (!cl || !dev)
				return;
			const auto srcTex = ctx.GetTexture(src);
			if (!srcTex.IsValid())
				return;
			const SamplerHandle sampler = GetOrCreateSRGBEncodeSampler(dev);
			if (!sampler.IsValid())
				return;
			const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
			if (!pipeline.IsValid())
				return;

			cl->SetPipeline(pipeline);
			if (pcw.IsValid() && !pcw.Flush(cl))
			{
				static bool s_WarnedFlush = false;
				if (!s_WarnedFlush)
				{
					s_WarnedFlush = true;
					Logger::Log::Warning(
						"SRGBEncodePass: push-constant flush rejected by command list; "
						"terminal encode running with zeroed constants");
				}
			}
			cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
			cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

			DescriptorSetDesc dsDesc{};
			dsDesc.layout = set0;
			dsDesc.transient = true;
			dsDesc.debugName = "SRGBEncode.DS0";
			const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
			if (!ds.IsValid())
				return;
			NamedDescriptorWriter wd(dev, ds, *meta, 0);
			wd.AddCombinedImageSampler("uLinearColor", srcTex, sampler);
			wd.Flush();
			cl->BindDescriptorSet(0, ds, pipeline);
			cl->Draw(3, 1);
		});
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
