#include "SceneView/ViewPresentationSnapshot.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Editor
{

void ViewPresentationSnapshot::Destroy(Rendering::IDevice* device)
{
    m_Texture.Destroy(device);
}

bool ViewPresentationSnapshot::Update(Rendering::RenderGraph::RGFrame& frame,
                                      Rendering::IDevice& device,
                                      Rendering::RenderGraph::RGTexture source,
                                      UI::UITextureSpace sourceSpace,
                                      const char* debugName)
{
    if (!source.IsValid() || !debugName)
        return false;

    const auto& sourceDesc = frame.Graph().ResourceDesc(source.Id);
    if (sourceDesc.SampleCount > 1 || sourceDesc.Width == 0 || sourceDesc.Height == 0)
        return false;

    const auto texture =
        m_Texture.Ensure(device, sourceDesc.Width, sourceDesc.Height, sourceDesc.Format, debugName);
    if (!texture.IsValid())
        return false;

    // ALWAYS import at Undefined: the copy below fully overwrites the snapshot,
    // so the discard transition is correct on every write frame — and it stays
    // correct even if a previous write frame was abandoned before Execute (a
    // tracked-state claim of ShaderResource would then be a layout lie).
    const auto snapshot = frame.ImportExternalTexture(
        debugName, texture, Rendering::ResourceState::Undefined,
        static_cast<Rendering::TextureFormat>(m_Texture.Format()));
    if (!snapshot.IsValid())
        return false;

    frame.AddPass(
        debugName, static_cast<int32_t>(Rendering::PassPhase::kFinalize),
        [&](Rendering::RenderGraph::RGPassBuilder& pass)
        {
            pass.Read(source, Rendering::RenderGraph::RGTextureRead::CopySrc);
            pass.Write(snapshot, Rendering::RenderGraph::RGTextureWrite::CopyDst);
        },
        [source, snapshot](Rendering::RenderGraph::RGContext& ctx)
        {
            if (ctx.Cmd)
                ctx.Cmd->CopyTexture(ctx.GetTexture(source), ctx.GetTexture(snapshot));
        });
    // Consumers sample the snapshot descriptor-direct on LATER frames
    // (same-queue ordering; the visible bind happens after this frame's
    // submit) — export at the sampling layout.
    frame.MarkOutput(snapshot, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
    m_Space = sourceSpace;
    return true;
}

} // namespace GameEngine::Editor
