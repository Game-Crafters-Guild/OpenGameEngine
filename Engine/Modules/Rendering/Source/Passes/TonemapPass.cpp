#include "Rendering/Passes/TonemapPass.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Logger/Logger.h"

#include "TonemapAces2Tables.h"

namespace GameEngine {
namespace Rendering {
namespace Passes {

void CleanupTonemapPassForDevice(IDevice* device);

int SelectTonemapOutEncoding(IDevice* device, FinalizeInputSpace inputSpace)
{
	const bool hdrActive = IsHdrOutputModeActive(device->GetActiveHdrOutputMode());

	if (inputSpace == FinalizeInputSpace::EncodedSrgb)
	{
		// No refusal channel exists mid-execute, so the loud channel is an
		// error plus the linear arm: a wrong image with a log line beats a
		// wrong image without one.
		static std::atomic<bool> s_WarnedEncodedInput{false};
		if (!s_WarnedEncodedInput.exchange(true))
		{
			Logger::Log::Error(
				"Tonemap: FinalizeInputSpace::EncodedSrgb is unsupported — tonemap.frag's "
				"chain is defined on scene-linear input only{}. The encode belongs downstream "
				"in the terminal Finalize pass.",
				IsFinalizeSpaceHdrViolation(inputSpace, hdrActive)
					? ", and the encoded arm is SDR-only besides"
					: "");
		}
	}

	if (hdrActive)
		return 4; // HDR linear with headroom; FinalSRGBEncode applies PQ/HLG/scRGB.
	// SDR, unconditionally: display-referred linear, undithered. Whether the ROP
	// or a manual encode applies the OETF changes who quantizes, not who
	// filters — and the pass that quantizes owns the filters, because it is the
	// only one that can see the step.
	return 1;
}


struct TonemapDeviceResources
{
	SamplerHandle sampler{INVALID_SAMPLER_HANDLE};
	// Default exposure-history buffer. tonemap.frag statically reads uExposure even when
	// useAutoExposure==0, so the binding must point at a live buffer; the contents are never
	// consumed on this path (the ternary selects the static push-constant exposure).
	BufferHandle exposureBuffer{INVALID_BUFFER_HANDLE};
	// ACES 2 tier tables (generated TonemapAces2Tables.h payload), uploaded once.
	// Lives in a device buffer, never in shader constants: glslang materializes
	// dynamically indexed const arrays into per-invocation scratch memory, which
	// at this size faults the device.
	BufferHandle aces2TablesBuffer{INVALID_BUFFER_HANDLE};
};

struct TonemapShaderBytes
{
	std::vector<uint8_t> vs;
	std::vector<uint8_t> fs;
	// Reflection meta from the shaderpkg — the single source of truth for the
	// descriptor-set layout and push-constant block. Hand-mirroring the .frag
	// interface here is the drift class that broke the thumbnail tonemap when
	// tonemap.frag gained uExposure (8f8a754a9); layouts and member offsets now
	// come from reflection so a shader edit cannot silently desync this pass.
	ShaderMeta meta;
	bool loaded = false;
	bool warned = false;
};

// Cache tonemap resources per logical device to avoid cross-device handle reuse when
// the Editor creates multiple windows (and therefore multiple IDevice instances).
using DeviceKey = IDevice*;
static std::unordered_map<DeviceKey, TonemapDeviceResources> g_TonemapPerDevice;

// Matches AutoExposureNode's exposure-state buffer ({float scale; uint valid; float pad0,pad1}).
static constexpr uint32_t kTonemapExposureStateBytes = 16u;

static TonemapShaderBytes& GetTonemapShaderBytes(const IDevice& device)
{
	static TonemapShaderBytes s;
	if (!s.loaded)
	{
		ShaderPackage pkg{};
		std::string err;
		if (LoadShaderPkg("Shaders/tonemap.shaderpkg", device.PreferredShaderSource(), pkg, &err))
		{
			auto itVs = pkg.stageBytes.find("vs");
			auto itFs = pkg.stageBytes.find("fs");
			if (itVs != pkg.stageBytes.end())
				s.vs = std::move(itVs->second);
			if (itFs != pkg.stageBytes.end())
				s.fs = std::move(itFs->second);
			s.meta = std::move(pkg.meta);
		}
		// Latch only on success — a not-yet-ready loader environment retries
		// instead of permanently disabling the pass; the warning fires once.
		s.loaded = !s.vs.empty() && !s.fs.empty();
		if (!s.loaded && !s.warned)
		{
			s.warned = true;
			Logger::Log::Warning(
				"Passes::Tonemap: Shaders/tonemap.shaderpkg unavailable ({}); retrying", err);
		}
	}
	return s;
}

static TonemapDeviceResources* GetOrCreateTonemapResources(IDevice* device)
{
	if (!device)
		return nullptr;

	auto [it, inserted] = g_TonemapPerDevice.try_emplace(device);
	if (inserted)
	{
		// Drop this per-device cache whenever the device's GPU objects die —
		// shutdown OR an in-place rebuild after device loss. The cached sampler
		// and exposure buffer belong to one VkDevice; a rebuild destroys them
		// while leaving this IDevice* key alive.
		device->RegisterPerDeviceCacheCleanup(
			"Passes.Tonemap",
			[](IDevice* d)
			{
				CleanupTonemapPassForDevice(d);
			});
	}

	auto& res = it->second;
	if (!res.sampler.IsValid())
	{
		SamplerDesc samplerDesc{};
		samplerDesc.minFilter = 1;
		samplerDesc.magFilter = 1;
		samplerDesc.mipFilter = 1;
		samplerDesc.addressModeU = 2; // clamp
		samplerDesc.addressModeV = 2;
		samplerDesc.addressModeW = 2;
		samplerDesc.debugName = "Tonemap.LinearClamp";
		res.sampler = device->CreateSampler(samplerDesc);
	}
	if (!res.exposureBuffer.IsValid())
	{
		BufferDesc expDesc{};
		expDesc.size = kTonemapExposureStateBytes;
		expDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
		expDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
		expDesc.debugName = "Tonemap.DefaultExposure";
		res.exposureBuffer = device->CreateBuffer(expDesc);
	}
	if (!res.aces2TablesBuffer.IsValid())
	{
		BufferDesc tablesDesc{};
		tablesDesc.size = kAces2TablesBytes;
		tablesDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
		tablesDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
		tablesDesc.debugName = "Tonemap.Aces2Tables";
		res.aces2TablesBuffer =
			CreateBufferInitialized(device, tablesDesc, kAces2Tables, kAces2TablesBytes);
	}
	return &res;
}

BufferHandle GetAces2TablesBuffer(IDevice* device)
{
	TonemapDeviceResources* res = GetOrCreateTonemapResources(device);
	return res ? res->aces2TablesBuffer : BufferHandle{};
}

uint32_t GetAces2TablesBufferBytes()
{
	return kAces2TablesBytes;
}

void CleanupTonemapPassForDevice(IDevice* device)
{
	if (!device)
		return;

	auto it = g_TonemapPerDevice.find(device);
	if (it == g_TonemapPerDevice.end())
		return;

	TonemapDeviceResources& res = it->second;
	if (res.sampler.IsValid())
	{
		device->DestroySampler(res.sampler);
		res.sampler = SamplerHandle{};
	}
	if (res.exposureBuffer.IsValid())
	{
		device->DestroyBuffer(res.exposureBuffer);
		res.exposureBuffer = BufferHandle{};
	}
	if (res.aces2TablesBuffer.IsValid())
	{
		device->DestroyBuffer(res.aces2TablesBuffer);
		res.aces2TablesBuffer = BufferHandle{};
	}
	g_TonemapPerDevice.erase(it);
}

RenderGraph::RGPass AddTonemapPassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                             const TonemapParams& params, const char* passName)
{
	IDevice* device = frame.Device();
	if (!device || !src.IsValid() || !dst.IsValid() || src.Id == dst.Id)
		return {};
	const auto& shaders = GetTonemapShaderBytes(*device);
	if (shaders.vs.empty() || shaders.fs.empty())
		return {}; // loader retries next frame — never latch the failure

	// Layout + push-constant block derived from the shaderpkg's reflection meta
	// (never hand-mirrored — see TonemapShaderBytes). Interned per device; the
	// per-frame cost is the variant-cache hit. Mirrors AddSRGBEncodePassRG.
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
	gd.DebugName = "Tonemap.Pipeline";
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
				Logger::Log::Warning("Passes::Tonemap: shader meta rejected ({})", metaErr);
			}
			return {};
		}
	}
	const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(gd));
	if (!pipelineId.IsValid())
		return {};

	// TonemapParams -> push constants by MEMBER NAME against the reflected
	// block, resolved at declaration (the by-value contract). Unnamed members
	// (ditherMode, useAutoExposure) stay zero: outEncoding=1 emits undithered
	// display-referred LINEAR, and this standalone pass never meters.
	NamedPushConstantWriter pcw =
		shaders.meta.PushConstants.empty()
			? NamedPushConstantWriter(shaders.meta, std::string{})
			: NamedPushConstantWriter(shaders.meta, shaders.meta.PushConstants[0].Name);
	if (pcw.IsValid())
	{
		pcw.Add("exposure", params.exposure);
		pcw.Add("tonemapMode", params.tonemapMode);
		pcw.Add("outEncoding", params.outEncoding);
		pcw.Add("paperWhiteNits", params.paperWhiteNits);
		pcw.Add("maxOutputNits", params.maxOutputNits);
		pcw.Add("ictcpChromaCompression", std::clamp(params.ictcpChromaCompression, 0.0f, 1.0f));
		pcw.Add("preserveAlpha", params.preserveAlpha);
	}

	const RenderGraph::RGResourceDesc dd = frame.Graph().ResourceDesc(dst.Id);
	const uint32_t w = dd.Width > 0 ? dd.Width : 1u;
	const uint32_t h = dd.Height > 0 ? dd.Height : 1u;

	return frame.AddPass(
		passName ? passName : "Tonemap", PassPhase::kFinalize,
		[&](RenderGraph::RGPassBuilder& p)
		{
			p.Read(src, RenderGraph::RGTextureRead::Sampled);
			RenderGraph::RGAttachmentOps ops{};
			// Full-screen triangle overwrites every pixel.
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
			TonemapDeviceResources* res = GetOrCreateTonemapResources(dev);
			if (!res || !res->sampler.IsValid() || !res->exposureBuffer.IsValid())
				return;
			const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
			if (!pipeline.IsValid())
				return;

			cl->SetPipeline(pipeline);
			if (pcw.IsValid())
				pcw.Flush(cl);
			cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
			cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

			DescriptorSetDesc dsDesc{};
			dsDesc.layout = set0;
			dsDesc.transient = true;
			dsDesc.debugName = "Tonemap.DS0";
			const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
			if (!ds.IsValid())
				return;
			// Bind by reflected NAME: uExposure points at the never-metered
			// default buffer (tonemap.frag statically references it even with
			// useAutoExposure==0).
			NamedDescriptorWriter wd(dev, ds, *meta, 0);
			wd.AddCombinedImageSampler("uHDRColor", srcTex, res->sampler);
			wd.AddStorageBuffer("uExposure", res->exposureBuffer, 0, kTonemapExposureStateBytes);
			wd.AddStorageBuffer("Aces2Tables", res->aces2TablesBuffer, 0, kAces2TablesBytes);
			wd.Flush();
			cl->BindDescriptorSet(0, ds, pipeline);
			cl->Draw(3, 1);
		});
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
