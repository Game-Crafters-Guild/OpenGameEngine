// Guard for the temporal dither phase: the experimental switch that advances
// the terminal encode's TPDF realisation per frame instead of holding it still.
//
// Two failure classes this exists to catch, both of which leave every plumbing
// log healthy:
//
//   1. A COLLAPSED PHASE SEQUENCE. The phase mapping is an additive
//      golden-ratio recurrence; a badly chosen increment (or a phase folded
//      back into the noise function's own offset parameter, whose
//      offset-constant degeneracy the comment above TriangularDither in
//      encode_srgb.frag documents) can look healthy at ordinal 1 and produce
//      near-identical frames at ordinal 40. A
//      0-vs-1 comparison is therefore not a gate: these tests sample a RANGE of
//      ordinals and assert on the whole set — per-ordinal statistics AND a
//      pairwise difference relation across every pair.
//   2. A PHASE THAT LEAKS PAST ITS GATE. Toggle off, and a transfer that owns
//      no quantization step, must both be bitwise the shipped static pattern.
//      "Same recording with the switch off" has to be provably identical, or
//      the A/B this feature exists for compares two unknowns.
//
// Own process, like its sibling encode suites: GE_OUTPUT_DITHER and GE_DEBAND
// latch once per process, and this suite needs dither ON with deband OFF to
// measure the dither in isolation.

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

void PinEnvironment()
{
	SetEnv("GE_HEADLESS_TEST", "1");
	SetEnv("GE_OUTPUT_DITHER", "1");
	SetEnv("GE_DEBAND", "0");
#ifdef RENDERING_SHADER_OUTPUT_DIR
	Utils::SetShaderPathResolver(
		+[](const std::filesystem::path& rel) -> std::filesystem::path
		{ return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / rel; });
#endif
}

struct FramePools
{
	RG::RGResourcePool Persistent;
	RG::RGTransientPool Transient;
	RG::RGUploadRing Ring;
	explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 32;
// Shallow ramp: ~26 encoded levels across 512 px, so an undithered quantize
// leaves ~20 px runs. The same discriminating workload the sibling dither
// suites use, so their budgets transfer unchanged.
constexpr float kMaxLinear = 0.01f;
// Ordinals sampled. Wide enough that a degeneracy which only appears after the
// first few realisations cannot hide, and cheap: 64 encodes of a 512x32 image.
constexpr uint64_t kOrdinalCount = 64;

std::unique_ptr<IDevice> CreateDeviceOrNull()
{
	DeviceDesc dd{};
	dd.preferredAPI = GraphicsAPI::Vulkan;
	dd.enableDynamicRendering = true;
	auto device = DeviceFactory::CreateDevice(dd);
	if (!device || !device->Initialize(dd))
		return nullptr;
	return device;
}

TextureHandle CreateLinearRamp(IDevice& device)
{
	TextureDesc srcDesc{};
	srcDesc.width = kWidth;
	srcDesc.height = kHeight;
	srcDesc.depth = 1;
	srcDesc.mipLevels = 1;
	srcDesc.arrayLayers = 1;
	srcDesc.sampleCount = 1;
	srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
	srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
	srcDesc.debugName = "TemporalDither.LinearRamp";
	const TextureHandle tex = device.CreateTexture(srcDesc);
	if (!tex.IsValid())
		return tex;

	std::vector<float> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const float v = kMaxLinear * static_cast<float>(x) / static_cast<float>(kWidth - 1);
			float* px = &pixels[(static_cast<size_t>(y) * kWidth + x) * 4];
			px[0] = px[1] = px[2] = v;
			px[3] = 1.0f;
		}
	}
	const size_t bytes = pixels.size() * sizeof(float);
	const BufferHandle upload = device.CreateUploadBuffer(bytes, "TemporalDither.Upload");
	if (!upload.IsValid())
		return TextureHandle{};
	device.UpdateBuffer(upload, 0, bytes, pixels.data());

	auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
	if (!cl)
		return TextureHandle{};
	cl->Begin();
	cl->CopyBufferToTextureSubresource(upload, tex, 0, 0, kWidth, kHeight, 0,
	                                   static_cast<size_t>(kWidth) * 4 * sizeof(float));
	cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
	                                                  ResourceState::ShaderResource));
	cl->End();
	std::vector<CommandList*> lists{cl.get()};
	device.ExecuteCommandLists(lists);
	device.WaitForIdle();
	return tex;
}

TextureHandle CreateTextureFromBytes(IDevice& device, const std::vector<uint8_t>& bytes)
{
	TextureDesc d{};
	d.width = kWidth;
	d.height = kHeight;
	d.depth = 1;
	d.mipLevels = 1;
	d.arrayLayers = 1;
	d.sampleCount = 1;
	d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
	d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
	d.debugName = "TemporalDither.EncodedSource";
	const TextureHandle tex = device.CreateTexture(d);
	if (!tex.IsValid())
		return tex;

	const BufferHandle upload = device.CreateUploadBuffer(bytes.size(), "TemporalDither.EncUpload");
	if (!upload.IsValid())
		return TextureHandle{};
	device.UpdateBuffer(upload, 0, bytes.size(), bytes.data());

	auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
	if (!cl)
		return TextureHandle{};
	cl->Begin();
	cl->CopyBufferToTextureSubresource(upload, tex, 0, 0, kWidth, kHeight, 0,
	                                   static_cast<size_t>(kWidth) * 4);
	cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
	                                                  ResourceState::ShaderResource));
	cl->End();
	std::vector<CommandList*> lists{cl.get()};
	device.ExecuteCommandLists(lists);
	device.WaitForIdle();
	return tex;
}

// One encode of `source` through the real pass with the given params. Empty
// means the pass declined (unstaged shaderpkg) — callers must fail loudly
// rather than compare two empty vectors and call that agreement.
std::vector<uint8_t> Encode(IDevice& device, TextureHandle source, TextureFormat srcFormat,
                            const Passes::FinalizeParams& params)
{
	const size_t outBytes = static_cast<size_t>(kWidth) * kHeight * 4;
	const BufferHandle readback = device.CreateReadbackBuffer(outBytes, "TemporalDither.Readback");
	if (!readback.IsValid())
		return {};

	FramePools pools(&device);
	RG::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
	frame.BeginFrame(0);

	const RG::RGTexture src = frame.ImportExternalTexture(
		"TemporalDither.Source", source, ResourceState::ShaderResource, srcFormat);
	TextureDesc dstDesc{};
	dstDesc.width = kWidth;
	dstDesc.height = kHeight;
	dstDesc.depth = 1;
	dstDesc.mipLevels = 1;
	dstDesc.arrayLayers = 1;
	dstDesc.sampleCount = 1;
	dstDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
	dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
	dstDesc.debugName = "TemporalDither.Encoded";
	const RG::RGTexture dst = frame.CreateTexture("TemporalDither.Encoded", dstDesc);
	if (!src.IsValid() || !dst.IsValid())
		return {};
	if (!Passes::AddSRGBEncodePassRG(frame, src, dst, params, "TemporalDither.Encode").IsValid())
		return {};

	frame.AddPass(
		"TemporalDither.Readback", PassPhase::kFinalize,
		[&](RG::RGPassBuilder& p)
		{
			p.Read(dst, RG::RGTextureRead::CopySrc);
			p.PreventCulling();
		},
		[dst, readback](RG::RGContext& ctx) {
			ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, kWidth,
			                                        kHeight);
		});

	frame.Execute();
	device.WaitForIdle();

	const void* mapped = device.MapBuffer(readback);
	if (!mapped)
		return {};
	std::vector<uint8_t> out(static_cast<const uint8_t*>(mapped),
	                         static_cast<const uint8_t*>(mapped) + outBytes);
	device.UnmapBuffer(readback);
	return out;
}

float LinearToSrgbRef(float c)
{
	if (c <= 0.0031308f)
		return 12.92f * c;
	return 1.055f * std::pow(std::max(c, 1e-6f), 1.0f / 2.4f) - 0.055f;
}

double MeanRun(const std::vector<uint8_t>& image)
{
	uint64_t runs = 0;
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		int prev = -1;
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const int v = image[(static_cast<size_t>(y) * kWidth + x) * 4];
			if (v != prev)
			{
				++runs;
				prev = v;
			}
		}
	}
	return runs == 0 ? 0.0 : static_cast<double>(kWidth) * kHeight / static_cast<double>(runs);
}

// Largest deviation from the ideal transfer curve, in output LSBs.
int MaxAbsDev(const std::vector<uint8_t>& image)
{
	int worst = 0;
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const int v = image[(static_cast<size_t>(y) * kWidth + x) * 4];
			const float lin = kMaxLinear * static_cast<float>(x) / static_cast<float>(kWidth - 1);
			const int ideal = static_cast<int>(std::lround(LinearToSrgbRef(lin) * 255.0f));
			worst = std::max(worst, std::abs(v - ideal));
		}
	}
	return worst;
}

// Fraction of pixels whose stored bytes differ between two encodes.
double DifferenceRate(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	size_t differing = 0;
	const size_t pixels = a.size() / 4;
	for (size_t i = 0; i < a.size(); i += 4)
	{
		if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
			++differing;
	}
	return pixels == 0 ? 0.0 : static_cast<double>(differing) / static_cast<double>(pixels);
}

Passes::FinalizeParams DestinationParams(float ditherPhase)
{
	return {.InputSpace = Passes::FinalizeInputSpace::Linear,
	        .Quantizer = Passes::FinalizeQuantizer::Destination,
	        .DitherPhase = ditherPhase};
}

// Restores both experimental toggles when it leaves scope: they are
// process-wide, and a test that left one armed would silently change the
// meaning of every test after it in this binary.
class ScopedTemporalDither
{
  public:
	ScopedTemporalDither(bool movie, bool screen)
		: m_Movie(Passes::IsTemporalMovieDitherEnabled()),
		  m_Screen(Passes::IsTemporalScreenDitherEnabled())
	{
		Passes::SetTemporalMovieDitherEnabled(movie);
		Passes::SetTemporalScreenDitherEnabled(screen);
	}
	~ScopedTemporalDither()
	{
		Passes::SetTemporalMovieDitherEnabled(m_Movie);
		Passes::SetTemporalScreenDitherEnabled(m_Screen);
	}
	ScopedTemporalDither(const ScopedTemporalDither&) = delete;
	ScopedTemporalDither& operator=(const ScopedTemporalDither&) = delete;

  private:
	bool m_Movie;
	bool m_Screen;
};

} // namespace

// ── 1. The mapping, on the CPU. ───────────────────────────────────────────
// The phase sequence must stay inside [0,1) and must SPREAD: a golden-ratio
// recurrence guarantees a minimum gap between any two of the first N terms
// (the three-distance theorem bounds it near 1/N) AND a bounded largest
// circular gap, so an increment rounded to something rational, wrapped to a
// short period, or shrunk until every phase huddles inside one arc shows up
// here rather than as a subtle artefact in a recording weeks later.
TEST(SrgbEncodeTemporalDither, ThePhaseSequenceSpreadsAcrossARangeOfOrdinals)
{
	std::vector<float> phases;
	phases.reserve(kOrdinalCount);
	for (uint64_t n = 0; n < kOrdinalCount; ++n)
	{
		const float phase = Passes::TemporalDitherPhaseForIndex(n);
		EXPECT_GE(phase, 0.0f) << "phase left [0,1) at ordinal " << n;
		EXPECT_LT(phase, 1.0f) << "phase left [0,1) at ordinal " << n;
		phases.push_back(phase);
	}
	EXPECT_FLOAT_EQ(phases[0], 0.0f) << "ordinal 0 must be the unphased pattern";

	std::vector<float> sorted = phases;
	std::sort(sorted.begin(), sorted.end());
	float minGap = 1.0f;
	for (size_t i = 1; i < sorted.size(); ++i)
		minGap = std::min(minGap, sorted[i] - sorted[i - 1]);
	// The three-distance theorem puts the smallest gap of the first N terms of
	// the golden-ratio sequence at ~1/(N*phi^2) at worst; half of that is a
	// floor no correct increment approaches and a collapsed one cannot clear.
	const float kMinGapFloor = 0.5f / (static_cast<float>(kOrdinalCount) * 2.618f);
	EXPECT_GT(minGap, kMinGapFloor)
		<< "the phase sequence CLUSTERS: closest pair of the first " << kOrdinalCount
		<< " ordinals is " << minGap << " apart — two frames that far apart carry nearly the same "
		   "dither realisation, which is the collapse this sequence exists to avoid";

	float maxGap = 1.0f - sorted.back() + sorted.front();
	for (size_t i = 1; i < sorted.size(); ++i)
		maxGap = std::max(maxGap, sorted[i] - sorted[i - 1]);
	// The floor's dual, so an arc-confined sequence cannot pass by clustering
	// tightly inside a fraction of [0,1): the golden-ratio sequence's largest
	// circular gap sits near phi^2/N (0.0213 at N=64), so twice that admits it
	// with ~4x headroom while any unexplored arc trips the wraparound gap.
	const float kMaxGapCeiling = 2.0f * 2.618f / static_cast<float>(kOrdinalCount);
	EXPECT_LT(maxGap, kMaxGapCeiling)
		<< "the phase sequence is ARC-CONFINED: the largest circular gap of the first "
		<< kOrdinalCount << " ordinals is " << maxGap
		<< " — realisations never enter that span of [0,1), so the temporal average stays "
		   "biased across it";

	// Long recordings must not lose the fractional part to float precision.
	const float late = Passes::TemporalDitherPhaseForIndex(1000000);
	EXPECT_GE(late, 0.0f);
	EXPECT_LT(late, 1.0f);
	EXPECT_NE(late, Passes::TemporalDitherPhaseForIndex(1000001))
		<< "the mapping has gone constant at high ordinals — a long recording would carry one "
		   "realisation for the whole tail";
}

// ── 2. The gates, and that the two hosts read the same one. ───────────────
// Both hosts seed their movie encode from m_MovieFramesScheduled through
// MovieDitherPhase, so a host computing its own phase — or a toggle wired to
// the wrong switch — makes two recordings of one scene disagree.
TEST(SrgbEncodeTemporalDither, ThePhaseIsZeroUnlessItsOwnToggleIsOn)
{
	{
		const ScopedTemporalDither off(false, false);
		for (uint64_t n = 0; n < kOrdinalCount; ++n)
		{
			EXPECT_FLOAT_EQ(Passes::MovieDitherPhase(n), 0.0f)
				<< "movie phase nonzero with the movie toggle OFF at ordinal " << n;
			EXPECT_FLOAT_EQ(Passes::ScreenDitherPhase(n), 0.0f)
				<< "screen phase nonzero with the screen toggle OFF at frame " << n;
		}
	}
	{
		// Independent switches: arming the movie must not animate the screen.
		const ScopedTemporalDither movieOnly(true, false);
		for (uint64_t n = 0; n < kOrdinalCount; ++n)
		{
			EXPECT_FLOAT_EQ(Passes::MovieDitherPhase(n), Passes::TemporalDitherPhaseForIndex(n))
				<< "the movie phase is not the shared mapping at ordinal " << n;
			EXPECT_FLOAT_EQ(Passes::ScreenDitherPhase(n), 0.0f)
				<< "the movie toggle animated the SCREEN dither at frame " << n;
		}
	}
	{
		const ScopedTemporalDither screenOnly(false, true);
		EXPECT_FLOAT_EQ(Passes::ScreenDitherPhase(7), Passes::TemporalDitherPhaseForIndex(7));
		EXPECT_FLOAT_EQ(Passes::MovieDitherPhase(7), 0.0f)
			<< "the screen toggle armed the MOVIE dither";
	}
}

// ── 3. Every ordinal dithers, and no two ordinals agree. ──────────────────
// The gate the design owes: a RANGE of ordinals, not a 0-vs-1 pair.
// Assertions on the whole set, because any one alone is passable by a broken
// mapping — per-ordinal statistics catch a phase that destroys the dither's
// distribution, the pairwise relation catches one that re-uses realisations
// or fails to decorrelate them.
TEST(SrgbEncodeTemporalDither, EveryOrdinalDithersAndNoTwoOrdinalsMatch)
{
	PinEnvironment();
	const ScopedTemporalDither armed(true, false);
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	std::vector<std::vector<uint8_t>> frames;
	frames.reserve(kOrdinalCount);
	for (uint64_t n = 0; n < kOrdinalCount; ++n)
	{
		std::vector<uint8_t> encoded =
			Encode(*device, ramp, TextureFormat::R32G32B32A32_FLOAT,
		           DestinationParams(Passes::MovieDitherPhase(n)));
		ASSERT_FALSE(encoded.empty())
			<< "AddSRGBEncodePassRG declared nothing at ordinal " << n
			<< " — encode_srgb.shaderpkg not staged under the build root's Shaders/";
		frames.push_back(std::move(encoded));
	}

	// The workload has to be able to show a difference at all.
	const auto [lo, hi] = std::minmax_element(frames[0].begin(), frames[0].end());
	ASSERT_GE(static_cast<int>(*hi) - static_cast<int>(*lo), 15)
		<< "encoded ramp span collapsed — this run cannot discriminate and proves nothing";

	// (a) Per-ordinal statistics, in the budget GradientQuantizesDithered pins.
	// A translated realisation has the same run-length and deviation
	// distribution; anything that does not is a phase that changed the noise
	// rather than moving it.
	for (uint64_t n = 0; n < kOrdinalCount; ++n)
	{
		const double meanRun = MeanRun(frames[n]);
		EXPECT_LT(meanRun, 3.0) << "ordinal " << n << " lost its dither (mean run " << meanRun
		                        << " px) — the phase destroyed the realisation instead of "
		                           "advancing it";
		const int dev = MaxAbsDev(frames[n]);
		EXPECT_LE(dev, 2) << "ordinal " << n << " deviates from the sRGB transfer curve by " << dev
		                  << " LSB — the phase changed the dither's amplitude, not just its phase";
	}

	// (b) Pairwise, against the shift mechanism's own relation. A wrapped
	// scalar shift by δ flips only pixels whose dither sample sits within δ of
	// a quantization decision, so the pixel-difference rate is monotone in the
	// circular phase distance (measured on this ramp: ≈2.6·d for small d,
	// saturating ≈0.66, Spearman ρ > 0.999 over all pairs). Any 64 phases in
	// [0,1) contain a pair within 1/64 by pigeonhole, so an unconditional
	// pairwise-independence floor is unsatisfiable by construction for a
	// scalar phase shift. Three satisfiable properties, each catching a real
	// collapse: DISTINCT (a rate-0 pair is one realisation carried by two
	// movie frames), RELATION (a rate below the envelope means the phase moved
	// without moving pixels), FLOOR (far-apart phases must reach the
	// independence class, which a sequence collapsed onto a short arc never
	// does). Bounds are the measured relation with margin: the observed
	// minimum rate/distance ratio is 1.34 (at the saturated far end), and
	// every pair with distance ≥ 0.125 measures ≥ 0.305.
	constexpr double kMinRatePerDistance = 1.0;
	constexpr double kIndependenceFloor = 0.20;
	constexpr double kIndependenceDistance = 0.125;
	double worstRate = 1.0;
	uint64_t worstA = 0;
	uint64_t worstB = 0;
	for (uint64_t a = 0; a < kOrdinalCount; ++a)
	{
		for (uint64_t b = a + 1; b < kOrdinalCount; ++b)
		{
			const double pa = Passes::TemporalDitherPhaseForIndex(a);
			const double pb = Passes::TemporalDitherPhaseForIndex(b);
			double dist = std::abs(pa - pb);
			dist = std::min(dist, 1.0 - dist);
			const double rate = DifferenceRate(frames[a], frames[b]);
			EXPECT_GT(rate, 0.0)
				<< "ordinals " << a << " and " << b
				<< " produce identical images — two movie frames carry one realisation";
			EXPECT_GT(rate, kMinRatePerDistance * dist)
				<< "ordinals " << a << " and " << b << " differ in " << (rate * 100.0)
				<< "% of pixels at circular phase distance " << dist
				<< " — below the shift mechanism's lower envelope, so the phase advanced "
				   "without moving pixels";
			if (dist >= kIndependenceDistance)
			{
				EXPECT_GT(rate, kIndependenceFloor)
					<< "ordinals " << a << " and " << b << " sit " << dist
					<< " apart in phase yet differ in only " << (rate * 100.0)
					<< "% of pixels — far-apart realisations failed to decorrelate";
			}
			if (rate < worstRate)
			{
				worstRate = rate;
				worstA = a;
				worstB = b;
			}
		}
	}
	std::cout << "[temporal] worst pair (" << worstA << "," << worstB << ") difference rate "
	          << (worstRate * 100.0) << "%" << std::endl;
}

// ── 4. Off is bitwise the shipped path. ───────────────────────────────────
// The A/B this feature exists for compares a recording made with the toggle
// off against one made with it on. If "off" is not byte-identical to the
// static pattern, the control arm is not a control.
TEST(SrgbEncodeTemporalDither, TheToggleOffArmIsByteIdenticalToTheStaticPattern)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	const std::vector<uint8_t> statik =
		Encode(*device, ramp, TextureFormat::R32G32B32A32_FLOAT, DestinationParams(0.0f));
	ASSERT_FALSE(statik.empty()) << "AddSRGBEncodePassRG declared nothing — shaderpkg unstaged";

	const ScopedTemporalDither off(false, false);
	for (uint64_t n : {uint64_t{1}, uint64_t{17}, uint64_t{63}})
	{
		const std::vector<uint8_t> encoded =
			Encode(*device, ramp, TextureFormat::R32G32B32A32_FLOAT,
		           DestinationParams(Passes::MovieDitherPhase(n)));
		ASSERT_FALSE(encoded.empty());
		EXPECT_EQ(encoded, statik)
			<< "ordinal " << n
			<< " changed the image with the toggle OFF — the control arm of every A/B is not a "
			   "control";
	}
}

// ── 5. A transfer that owns no step ignores the phase. ────────────────────
// The pass zeroes the phase wherever it zeroes the dither, so a same-depth
// movie transfer (an 8-bit screen recorded to an 8-bit movie: the quantizer
// answers None) is byte-identical whatever the host passed. Without this the
// feature would silently perturb the one arm where it is defined to do nothing.
TEST(SrgbEncodeTemporalDither, ANoneTransferIsUnchangedByThePhase)
{
	PinEnvironment();
	const ScopedTemporalDither armed(true, true);
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());
	// Bytes a previous encode produced — what a None transfer is defined against.
	const std::vector<uint8_t> encodedSource =
		Encode(*device, ramp, TextureFormat::R32G32B32A32_FLOAT, DestinationParams(0.0f));
	ASSERT_FALSE(encodedSource.empty()) << "AddSRGBEncodePassRG declared nothing — shaderpkg unstaged";
	const TextureHandle source = CreateTextureFromBytes(*device, encodedSource);
	ASSERT_TRUE(source.IsValid());

	const Passes::FinalizeParams unphased{.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb,
	                                      .Quantizer = Passes::FinalizeQuantizer::None,
	                                      .DitherPhase = 0.0f};
	const std::vector<uint8_t> moved =
		Encode(*device, source, TextureFormat::RGBA8_UNORM, unphased);
	ASSERT_FALSE(moved.empty());
	EXPECT_EQ(moved, encodedSource) << "a None transfer altered bytes already on the step";

	for (uint64_t n : {uint64_t{1}, uint64_t{29}})
	{
		Passes::FinalizeParams phased = unphased;
		phased.DitherPhase = Passes::MovieDitherPhase(n);
		ASSERT_GT(phased.DitherPhase, 0.0f) << "the arm is not armed — this run proves nothing";
		const std::vector<uint8_t> phasedMove =
			Encode(*device, source, TextureFormat::RGBA8_UNORM, phased);
		ASSERT_FALSE(phasedMove.empty());
		EXPECT_EQ(phasedMove, moved)
			<< "ordinal " << n
			<< " perturbed a FinalizeQuantizer::None transfer — the phase leaked into a pass that "
			   "owns no quantization step, which is exactly the same-depth movie arm the feature "
			   "is defined to leave alone";
	}
}
