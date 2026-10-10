#pragma once

#include "Rendering/Core/Device.h"
#include "SceneView/ViewSnapshotTexture.h"
#include "UI/UITextureSpace.h"

#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
struct RGTexture;
}

namespace GameEngine::Editor
{

// Owns a non-aliased copy of a view's pipeline output, frozen for sampling on
// later frames. Render-graph pipeline outputs are pooled and may be reused by
// another hidden/visible view, so their physical handles cannot safely be
// retained past the frame that declared them — the scene view's last complete
// presentation and the bookmark popup's frozen preview both need a copy they
// own.
class ViewPresentationSnapshot
{
  public:
    void Destroy(Rendering::IDevice* device);
    bool Update(Rendering::RenderGraph::RGFrame& frame,
                Rendering::IDevice& device,
                Rendering::RenderGraph::RGTexture source,
                UI::UITextureSpace sourceSpace,
                const char* debugName);

    // The frozen copy, or an invalid handle once `device` has been rebuilt
    // since it was written. Every consumer reads through here and most never
    // reach Update on the frame they read (the waiting-for-extraction arm
    // imports the cached handle; the bookmark popup binds it hover cycles
    // later), so this read is the only rebuild check they get.
    Rendering::TextureHandle Texture(const Rendering::IDevice& device) const
    {
        return m_Texture.Texture(device);
    }
    uint32_t Width() const { return m_Texture.Width(); }
    uint32_t Height() const { return m_Texture.Height(); }
    // Space of the pixels the snapshot holds, stamped by the Update that wrote
    // them (#767): the frozen copy keeps the space of the frame that rendered
    // it across output-mode switches. Meaningful only while Texture() reports a
    // valid handle.
    UI::UITextureSpace Space() const { return m_Space; }

  private:
    ViewSnapshotTexture m_Texture;
    // Never observable before the first Update: Texture() reports an invalid
    // handle until then.
    UI::UITextureSpace m_Space = UI::UITextureSpace::DisplayLinearSdr();
};

} // namespace GameEngine::Editor
