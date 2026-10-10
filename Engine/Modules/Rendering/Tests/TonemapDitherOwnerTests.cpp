// The one-dither-owner invariant, and the gate that can finally see arm 0.
//
// The invariant (landing 4): a pass may write a nonzero dither iff it owns the
// quantization step. On every SDR frame path that pass is the terminal Finalize
// (SRGBEncodePass), which sizes both filters to the real destination step. The
// Tonemap must therefore never dither — it neither owns the step nor can see it.
//
// Nothing in the tree could observe the violation. The headless view fixture
// overwrites the pipeline output with injected content before the finalize
// reads it, so the tonemap's dither is destroyed by the very mechanism that
// makes that fixture deterministic; and the one dither-ON statistical gate
// (SrgbEncodeDither.GradientQuantizesDithered) declares the encode pass against
// a synthetic gradient with no tonemap upstream. A defect that no test can see
// is a defect that gets fixed against a hypothesis, so these three run first:
//
//   1. TonemapEmitsTheSamePixelsForEverySdrOutEncoding — the PIXEL witness, on
//      the real shipped tonemap.frag through the real AddTonemapPassRG, into an
//      `_SRGB` colour attachment (arm 0's own assumed environment: the ROP
//      applies the OETF, which is what its slope compensation is sized for).
//      Two declares that differ ONLY in outEncoding must produce identical
//      bytes. A self-dithering tonemap arm makes them differ.
//   2. TheSdrTonemapArmDoesNotFollowTheSwapchainFormat — the DEVICE witness.
//      Two live window targets, one 10-bit and one 8-bit `_SRGB` (the same
//      SetPreferTenBitSwapchain lever GE_FORCE_8BIT_SRGB_SWAPCHAIN pulls), with
//      the instrument read in BOTH directions before anything is concluded. The
//      SDR tonemap arm must be 1 on both.
//   3. TheFinalizeDithersOnAnSrgbSwapchain — the SURVIVING owner. Green before
//      and after: with 1 and 2 it says two owners before, one owner after.
//
// Test 2 is the levered arm the design's G2 names, produced here rather than by
// an editor run: GE_FORCE_8BIT_SRGB_SWAPCHAIN's whole implementation is
// SetPreferTenBitSwapchain(false) (VulkanDevice.cpp), which this suite calls
// directly, per the discipline WindowTargetPresentedFormatTests established.

#include "Source/Vulkan/VulkanDevice.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TonemapPass.h"

#include <gtest/gtest.h>

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

// Both filters latch once per process. The tonemap arms are measured with the
// terminal dither ON because that is the shipping state the double dither
// happens in; test 1 declares no encode pass at all, so the latch only matters
// to test 3.
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
// leaves ~20 px runs — the same discriminating workload SrgbEncodeDitherTests
// uses, and shallow enough that a 1-LSB perturbation is unmistakable.
constexpr float kMaxLinear = 0.01f;

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

// A scene-linear horizontal ramp, uploaded and rested at ShaderResource.
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
	srcDesc.debugName = "DitherOwner.LinearRamp";
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
	const BufferHandle upload = device.CreateUploadBuffer(bytes, "DitherOwner.Upload");
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

// An RGBA8_UNORM source holding bytes a previous encode produced — what a
// FinalizeQuantizer::None transfer is defined against.
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
	d.debugName = "DitherOwner.EncodedSource";
	const TextureHandle tex = device.CreateTexture(d);
	if (!tex.IsValid())
		return tex;

	const BufferHandle upload = device.CreateUploadBuffer(bytes.size(), "DitherOwner.EncUpload");
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

TextureDesc ColorTarget(TextureFormat format, const char* name)
{
	TextureDesc d{};
	d.width = kWidth;
	d.height = kHeight;
	d.depth = 1;
	d.mipLevels = 1;
	d.arrayLayers = 1;
	d.sampleCount = 1;
	d.format = static_cast<uint32_t>(format);
	d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
	d.debugName = name;
	return d;
}

// Declares one pass into an `_SRGB` attachment and returns the STORED bytes.
// Empty means the pass declined (unstaged shaderpkg) — the caller must fail
// loudly rather than compare two empty vectors and call it agreement.
using DeclareFn = bool (*)(RG::RGFrame&, RG::RGTexture src, RG::RGTexture dst, int arm);

std::vector<uint8_t> RunOnePass(IDevice& device, TextureHandle ramp, TextureFormat dstFormat,
                                const char* dstName, int arm, DeclareFn declare,
                                TextureFormat srcFormat = TextureFormat::R32G32B32A32_FLOAT)
{
	const size_t outBytes = static_cast<size_t>(kWidth) * kHeight * 4;
	const BufferHandle readback = device.CreateReadbackBuffer(outBytes, "DitherOwner.Readback");
	if (!readback.IsValid())
		return {};

	FramePools pools(&device);
	RG::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
	frame.BeginFrame(0);

	const RG::RGTexture src =
		frame.ImportExternalTexture("DitherOwner.Ramp", ramp, ResourceState::ShaderResource,
	                                srcFormat);
	const RG::RGTexture dst = frame.CreateTexture(dstName, ColorTarget(dstFormat, dstName));
	if (!src.IsValid() || !dst.IsValid())
		return {};
	if (!declare(frame, src, dst, arm))
		return {};

	frame.AddPass(
		"DitherOwner.Readback", PassPhase::kFinalize,
		[&](RG::RGPassBuilder& p)
		{
			p.Read(dst, RG::RGTextureRead::CopySrc);
			p.PreventCulling();
		},
		[dst, readback](RG::RGContext& ctx)
		{ ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, kWidth, kHeight); });

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

bool DeclareTonemap(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, int arm)
{
	Passes::TonemapParams params{};
	params.outEncoding = arm;
	return Passes::AddTonemapPassRG(frame, src, dst, params, "DitherOwner.Tonemap").IsValid();
}

// Mean same-value run length along the gradient axis on the red channel — the
// instrument SrgbEncodeDitherTests uses, so its budgets transfer.
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

struct Difference
{
	size_t Pixels = 0;
	int MaxAbs = 0;
};

Difference Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	Difference d{};
	for (size_t i = 0; i < a.size(); i += 4)
	{
		int worst = 0;
		for (size_t c = 0; c < 3; ++c)
			worst = std::max(worst, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
		if (worst != 0)
			++d.Pixels;
		d.MaxAbs = std::max(d.MaxAbs, worst);
	}
	return d;
}

} // namespace

// ── 1. The pixel witness. ──────────────────────────────────────────────────
// tonemap.frag's SDR arms 0 and 1 both emit display-referred LINEAR and must
// differ in nothing at all. Into an `_SRGB` attachment the ROP applies the
// OETF, so a linear-domain perturbation sized to ~1 LSB after that encode lands
// as ~1 LSB in the stored bytes — visible, and exactly what arm 0 was built to
// do.
//
// REDDENS IF: any tonemap arm dithers, debands, or otherwise perturbs its
// output. That is the whole defect: the pass that does not own the
// quantization step must not filter for it.
TEST(TonemapDitherOwner, TonemapEmitsTheSamePixelsForEverySdrOutEncoding)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	const std::vector<uint8_t> armOne =
		RunOnePass(*device, ramp, TextureFormat::RGBA8_SRGB, "DitherOwner.Arm1", 1, &DeclareTonemap);
	ASSERT_FALSE(armOne.empty())
		<< "AddTonemapPassRG declared nothing — tonemap.shaderpkg not staged under the build "
		   "root's Shaders/ (tests run from CMAKE_BINARY_DIR)";
	const std::vector<uint8_t> armZero =
		RunOnePass(*device, ramp, TextureFormat::RGBA8_SRGB, "DitherOwner.Arm0", 0, &DeclareTonemap);
	ASSERT_FALSE(armZero.empty());

	// The workload has to be able to show a difference at all: a black or
	// constant output would agree trivially in either direction.
	const auto [lo, hi] = std::minmax_element(armOne.begin(), armOne.end());
	ASSERT_GE(static_cast<int>(*hi) - static_cast<int>(*lo), 15)
		<< "encoded ramp span collapsed (min=" << +*lo << " max=" << +*hi
		<< ") — this run cannot discriminate and proves nothing";

	const Difference diff = Compare(armZero, armOne);
	EXPECT_EQ(diff.Pixels, 0u)
		<< "the Tonemap DITHERS ITSELF on outEncoding 0: " << diff.Pixels << " of "
		<< (armZero.size() / 4) << " pixels differ from the undithered arm, max |delta| "
		<< diff.MaxAbs << " LSB (arm 0 mean run " << MeanRun(armZero) << " px, arm 1 "
		<< MeanRun(armOne) << " px). A second dither owner on every path whose terminal "
		   "Finalize also dithers — the double dither this landing deletes";
}

// ── 3. The surviving owner, on the same workload. ─────────────────────────
// Green before and after: with test 1 it reads "two owners before the
// retirement, one after". The finalize is declared in the shape
// DeclareViewFinalize declares it (Linear source, filters sized to the
// PRESENTED step), so what is measured is the production owner, not a
// test-only arm.
//
// REDDENS IF: the terminal Finalize stops dithering — which would make the
// retirement a genuine regression rather than a defect fix.
TEST(TonemapDitherOwner, TheFinalizeDithersOnAnSrgbSwapchain)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	const std::vector<uint8_t> encoded = RunOnePass(
		*device, ramp, TextureFormat::RGBA8_UNORM, "DitherOwner.Finalized", 0,
		+[](RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, int) -> bool
		{
			return Passes::AddSRGBEncodePassRG(
					   frame, src, dst,
					   {.InputSpace = Passes::FinalizeInputSpace::Linear,
			            .Quantizer = Passes::FinalizeQuantizer::Presented,
			            .PresentedFormat = TextureFormat::BGRA8_SRGB},
					   "DitherOwner.Finalize")
				.IsValid();
		});
	ASSERT_FALSE(encoded.empty())
		<< "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged";

	const double meanRun = MeanRun(encoded);
	EXPECT_LT(meanRun, 3.0)
		<< "the terminal Finalize is NOT dithering at the 8-bit presented step (mean run "
		<< meanRun << " px) — the tonemap retirement would leave this path with no dither owner "
		   "at all";
}

// ── 2. The device witness: the lever, read in both directions. ────────────
// GE_FORCE_8BIT_SRGB_SWAPCHAIN is SetPreferTenBitSwapchain(false) and nothing
// else, so this reproduces the levered host's swapchain here. The instrument is
// verified in BOTH directions before the arm is read: a one-sided reading
// proves nothing, because an unarmed instrument reads whatever it always read.
//
// REDDENS IF: the SDR tonemap arm is keyed on the swapchain format — the
// selector consulting SwapchainNeedsManualSRGBEncode(), which is both the
// second dither owner (D1) and the last process-global device read in the
// dither chain (D2: a multi-window editor lets whichever window activated last
// decide whether every view's tonemap dithers).
TEST(TonemapDitherOwner, TheSdrTonemapArmDoesNotFollowTheSwapchainFormat)
{
#if !defined(HAVE_GLFW)
	GTEST_SKIP() << "GLFW not available on this build agent";
#else
	PinEnvironment();
	ASSERT_EQ(glfwInit(), GLFW_TRUE);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* windowTenBit = glfwCreateWindow(320, 200, "DitherOwnerTenBit", nullptr, nullptr);
	GLFWwindow* windowSrgb = glfwCreateWindow(256, 160, "DitherOwnerSrgb", nullptr, nullptr);
	ASSERT_NE(windowTenBit, nullptr);
	ASSERT_NE(windowSrgb, nullptr);

	auto device = CreateDeviceOrNull();
	if (!device)
	{
		glfwDestroyWindow(windowSrgb);
		glfwDestroyWindow(windowTenBit);
		glfwTerminate();
		GTEST_SKIP() << "No Vulkan device available on this agent";
	}
	auto* vk = static_cast<VulkanDevice*>(device.get());

	vk->SetPreferTenBitSwapchain(true);
	WindowTargetHandle tenBit{};
	ASSERT_TRUE(device->CreateAndActivateWindowTarget(windowTenBit, 320, 200, &tenBit));
	const TextureFormat formatTenBit = device->GetSwapchainTextureFormat();
	const bool manualTenBit = device->SwapchainNeedsManualSRGBEncode();
	const int armTenBit =
		Passes::SelectTonemapOutEncoding(device.get(), Passes::FinalizeInputSpace::Linear);

	// The lever, pulled exactly as GE_FORCE_8BIT_SRGB_SWAPCHAIN pulls it.
	vk->SetPreferTenBitSwapchain(false);
	WindowTargetHandle srgb{};
	ASSERT_TRUE(device->CreateAndActivateWindowTarget(windowSrgb, 256, 160, &srgb));
	const TextureFormat formatSrgb = device->GetSwapchainTextureFormat();
	const bool manualSrgb = device->SwapchainNeedsManualSRGBEncode();
	const int armSrgb =
		Passes::SelectTonemapOutEncoding(device.get(), Passes::FinalizeInputSpace::Linear);

	const bool srgbIsHardware =
		formatSrgb == TextureFormat::BGRA8_SRGB || formatSrgb == TextureFormat::RGBA8_SRGB;
	const bool armed = srgbIsHardware && manualTenBit && !manualSrgb;

	std::cout << "[lever] ten-bit target: format=" << ToString(formatTenBit)
	          << " swapchainNeedsManualSRGBEncode=" << (manualTenBit ? "true" : "false")
	          << " tonemapArm=" << armTenBit << "\n"
	          << "[lever] _SRGB target:   format=" << ToString(formatSrgb)
	          << " swapchainNeedsManualSRGBEncode=" << (manualSrgb ? "true" : "false")
	          << " tonemapArm=" << armSrgb << std::endl;

	const bool skipped = !armed;
	if (skipped)
	{
		// Inherited from the sibling suite's discipline: a run whose instrument
		// did not move in both directions must SAY SO, not pass on assertions
		// that could not discriminate.
		GTEST_SKIP() << "the swapchain lever did not resolve both directions on this agent "
		                "(ten-bit manual=" << manualTenBit << " " << ToString(formatTenBit)
		             << ", levered manual=" << manualSrgb << " " << ToString(formatSrgb)
		             << ") — nothing below could discriminate";
	}

	EXPECT_EQ(armTenBit, 1) << "the SDR tonemap arm on a UNORM/10-bit swapchain must be 1 "
	                           "(emit undithered linear; the Finalize owns the dither)";
	EXPECT_EQ(armSrgb, 1)
		<< "the SDR tonemap arm FOLLOWS THE SWAPCHAIN FORMAT: a hardware-sRGB swapchain "
		   "(" << ToString(formatSrgb)
		<< ") selects arm " << armSrgb
		<< ", the arm tonemap.frag dithers itself in — while the view's Finalize is eligible "
		   "on exactly the same frame (it keys on HDR only). Two dither owners on one path, "
		   "selected from a PROCESS-GLOBAL device property that a second window can change";

	device->DestroyWindowTarget(srgb);
	device->DestroyWindowTarget(tenBit);
	device->Shutdown();
	device.reset();
	glfwDestroyWindow(windowSrgb);
	glfwDestroyWindow(windowTenBit);
	glfwTerminate();
#endif
}

// ── 4. The invariant the whole landing rests on, stated as an assertion. ──
// A pass writes a nonzero dither iff it owns the quantization step:
//   Quantizer::None      -> filters nothing (the bytes are already on the step)
//   Quantizer::Destination + dither enabled -> filters
// SRGBEncodePass implements it at the point it computes outputLsb; nothing
// asserted it, which is how a second owner survived in another pass entirely.
//
// REDDENS IF: either half inverts — a None transfer that grains an image that
// was already exact, or a real quantization step that ships undithered.
TEST(TonemapDitherOwner, TheEncodeFiltersExactlyWhenItOwnsTheStep)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	// Owns the step: a linear source quantized into 8-bit.
	const std::vector<uint8_t> owned = RunOnePass(
		*device, ramp, TextureFormat::RGBA8_UNORM, "DitherOwner.Owned", 0,
		+[](RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, int) -> bool
		{
			return Passes::AddSRGBEncodePassRG(
					   frame, src, dst,
					   {.InputSpace = Passes::FinalizeInputSpace::Linear,
			            .Quantizer = Passes::FinalizeQuantizer::Destination},
					   "DitherOwner.Owned")
				.IsValid();
		});
	ASSERT_FALSE(owned.empty()) << "AddSRGBEncodePassRG declared nothing — shaderpkg unstaged";
	const double ownedRun = MeanRun(owned);
	EXPECT_LT(ownedRun, 3.0) << "the pass owns this quantization step (Destination) and did not "
	                            "dither it (mean run " << ownedRun << " px)";

	// Does NOT own the step: the same bytes moved again under None. The source
	// already holds destination code values, so the transfer must add nothing —
	// byte-for-byte, not statistically.
	const TextureHandle encodedSrc = CreateTextureFromBytes(*device, owned);
	ASSERT_TRUE(encodedSrc.IsValid());
	const std::vector<uint8_t> moved = RunOnePass(
		*device, encodedSrc, TextureFormat::RGBA8_UNORM, "DitherOwner.Moved", 0,
		+[](RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, int) -> bool
		{
			return Passes::AddSRGBEncodePassRG(
					   frame, src, dst,
					   {.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb,
			            .Quantizer = Passes::FinalizeQuantizer::None},
					   "DitherOwner.Moved")
				.IsValid();
		},
		TextureFormat::RGBA8_UNORM);
	ASSERT_FALSE(moved.empty());
	const Difference diff = Compare(moved, owned);
	EXPECT_EQ(diff.Pixels, 0u)
		<< "a FinalizeQuantizer::None transfer FILTERED: " << diff.Pixels
		<< " pixels changed (max |delta| " << diff.MaxAbs
		<< "). Values already on the destination step must move untouched — dithering an "
		   "exact image only re-grains it";
}

// ── 5. The required operands, enforced where the type system cannot. ──
// FinalizeParams is an aggregate, so a designated initializer that omits
// InputSpace or Quantizer compiles and value-initializes the field. Both enums
// put a poison Unspecified first, and the pass refuses.
//
// REDDENS IF: the poison stops being the first enumerator, or the refusal is
// dropped — either of which turns a forgotten operand back into a silently
// inherited Linear/Destination, the exact failure the contract exists to stop.
TEST(TonemapDitherOwner, TheEncodeRefusesAnUnspecifiedOperand)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	const TextureHandle ramp = CreateLinearRamp(*device);
	ASSERT_TRUE(ramp.IsValid());

	EXPECT_EQ(static_cast<int>(Passes::FinalizeInputSpace{}), 0)
		<< "value-initialization must land on the poison, not on a real space";
	EXPECT_EQ(static_cast<int>(Passes::FinalizeQuantizer{}), 0)
		<< "value-initialization must land on the poison, not on a real quantizer";
	EXPECT_EQ(Passes::FinalizeInputSpace{}, Passes::FinalizeInputSpace::Unspecified);
	EXPECT_EQ(Passes::FinalizeQuantizer{}, Passes::FinalizeQuantizer::Unspecified);

	FramePools pools(device.get());
	RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
	frame.BeginFrame(0);
	const RG::RGTexture src =
		frame.ImportExternalTexture("Poison.Ramp", ramp, ResourceState::ShaderResource,
	                                TextureFormat::R32G32B32A32_FLOAT);
	const RG::RGTexture dst =
		frame.CreateTexture("Poison.Dst", ColorTarget(TextureFormat::RGBA8_UNORM, "Poison.Dst"));
	ASSERT_TRUE(src.IsValid() && dst.IsValid());

	// The positive control: naming both operands declares a pass here, so a
	// refusal below is the poison and not an unstaged shaderpkg.
	EXPECT_TRUE(Passes::AddSRGBEncodePassRG(
					frame, src, dst,
					{.InputSpace = Passes::FinalizeInputSpace::Linear,
	                 .Quantizer = Passes::FinalizeQuantizer::Destination},
					"Poison.Control")
					.IsValid())
		<< "the control declare failed — this run cannot tell a refusal from an unstaged shader";

	EXPECT_FALSE(
		Passes::AddSRGBEncodePassRG(frame, src, dst,
	                                {.Quantizer = Passes::FinalizeQuantizer::Destination},
	                                "Poison.NoInputSpace")
			.IsValid())
		<< "an omitted InputSpace declared a pass — it inherited Linear silently";
	EXPECT_FALSE(
		Passes::AddSRGBEncodePassRG(frame, src, dst,
	                                {.InputSpace = Passes::FinalizeInputSpace::Linear},
	                                "Poison.NoQuantizer")
			.IsValid())
		<< "an omitted Quantizer declared a pass — it inherited Destination silently";
	EXPECT_FALSE(Passes::AddSRGBEncodePassRG(frame, src, dst, {}, "Poison.Neither").IsValid())
		<< "a fully defaulted FinalizeParams declared a pass";
}
