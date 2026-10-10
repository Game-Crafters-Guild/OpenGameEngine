#pragma once

#include <string>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuilder.h"

namespace GameEngine { namespace Rendering {

// Lightweight convenience wrapper to construct PipelineDesc from ShaderDesc metadata
// and build a PipelineHandle via IDevice. Header-only for simplicity.
class PipelineBuilder {
public:
    PipelineBuilder() = default;

    PipelineBuilder& SetType(PipelineType t) { m_Desc.type = t; return *this; }
    PipelineBuilder& SetDebugName(const std::string& n) { m_DebugName = n; m_Desc.debugName = m_DebugName.c_str(); return *this; }
    PipelineBuilder& SetDebugName(const char* n) { m_DebugName = n ? n : ""; m_Desc.debugName = m_DebugName.c_str(); return *this; }

    // Provide shader binaries directly (preferred when already loaded)
    PipelineBuilder& SetVertexShader(const std::vector<uint8_t>& vs) { m_Desc.vertexShader = vs; return *this; }
    PipelineBuilder& SetPixelShader(const std::vector<uint8_t>& ps) { m_Desc.pixelShader = ps; return *this; }
    PipelineBuilder& SetComputeShader(const std::vector<uint8_t>& cs) { m_Desc.computeShader = cs; return *this; }


    // Apply metadata to PipelineDesc (set layouts, push constants, plus optional format hints)
    bool ApplyMeta(const ShaderMeta& meta,
                   const MaterialBuilder::FormatsHint& formats,
                   MaterialBuilder::MergeMode mode = MaterialBuilder::MergeMode::Auto,
                   const MaterialBuilder::PushConstantPolicy& policy = {},
                   std::string* outError = nullptr) {
        std::string err;
        if (!MaterialBuilder::BuildPipelineDescFromMeta(meta, m_Desc, formats, mode, policy, &err)) {
            if (outError) *outError = err; else m_LastError = err;
            return false;
        }
        m_HaveMeta = true;
        return true;
    }

    // Dynamic rendering format hints
    PipelineBuilder& SetColorAttachmentFormats(const std::vector<uint32_t>& fmts) { m_Desc.colorAttachmentFormats = fmts; return *this; }
    PipelineBuilder& SetDepthAttachmentFormat(uint32_t fmt) { m_Desc.depthAttachmentFormat = fmt; return *this; }

    // Common dynamic states for post passes
    PipelineBuilder& WithFullscreenDynamicStates() { m_Desc.AddDynamicState(DynamicState::Viewport); m_Desc.AddDynamicState(DynamicState::Scissor); return *this; }


	    // Alias for discoverability
	    PipelineBuilder& WithDynamicViewportScissor() { return WithFullscreenDynamicStates(); }

    // Rasterization
    PipelineBuilder& SetCullingMode(CullModeFlags mode, FrontFace frontFace = FrontFace::CounterClockwise) { m_Desc.SetCullingMode(mode, frontFace); return *this; }

    // Depth
    PipelineBuilder& DisableDepthTest() { m_Desc.EnableDepthTest(false); return *this; }

    // Enable alpha blending on all color attachments (SrcAlpha/OneMinusSrcAlpha)
    PipelineBuilder& EnableAlphaBlendForAllColorAttachments() {
        const size_t n = m_Desc.colorAttachmentFormats.size();
        if (n == 0) return *this;
        if (m_Desc.colorBlendState.attachments.size() < n) m_Desc.colorBlendState.attachments.resize(n);
        for (size_t i = 0; i < n; ++i) {
            auto& a = m_Desc.colorBlendState.attachments[i];
            a.blendEnable = true;
            a.srcColorBlendFactor = BlendFactor::SrcAlpha;
            a.dstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
            a.colorBlendOp = BlendOp::Add;
            a.srcAlphaBlendFactor = BlendFactor::SrcAlpha;
            a.dstAlphaBlendFactor = BlendFactor::OneMinusSrcAlpha;
            a.alphaBlendOp = BlendOp::Add;
            a.colorWriteMask = 0xF;
        }
        return *this;
    }

    // Convenience profile for fullscreen post-process passes
    PipelineBuilder& ProfileFullscreenPost(const std::vector<uint32_t>& colorFormats) {
        SetColorAttachmentFormats(colorFormats);
        WithFullscreenDynamicStates();
        SetCullingMode(CullModeFlagBits::None);
        m_Desc.EnableDepthTest(false);
        return *this;
    }

    // Convenience profile for overlays/HUD with alpha blending across MRTs
    PipelineBuilder& ProfileOverlayAlphaBlend(const std::vector<uint32_t>& colorFormats) {
        SetColorAttachmentFormats(colorFormats);
        WithDynamicViewportScissor();
        SetCullingMode(CullModeFlagBits::None);
        DisableDepthTest();
        EnableAlphaBlendForAllColorAttachments();
        return *this;

    }

    // Build the pipeline. Forwards to `IDevice::CreatePipeline` which
    // translates the desc through the typed intern + GetOrCreate cache.
    PipelineHandle Build(IDevice* dev) const
    {
        return dev ? dev->CreatePipeline(m_Desc) : PipelineHandle{};
    }

    const PipelineDesc& GetDesc() const { return m_Desc; }
    const std::string& LastError() const { return m_LastError; }

private:
    PipelineDesc m_Desc{};
    bool m_HaveMeta = false;
    std::string m_LastError;
    std::string m_DebugName;
};

}} // namespace GameEngine::Rendering

