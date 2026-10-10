#pragma once

#include "AssetCore/GUID.h"
#include "Panels/GraphPanel.h"

#include <cstdint>
#include <filesystem>

namespace GameEngine {

class BlendSpace1DEditor;
class BlendSpace2DEditor;

class AnimationGraphPanel final : public GraphPanel
{
public:
    AnimationGraphPanel();

protected:
    /** States what an animation graph contributes, and what it reacts to. */
    void Init();

    void OnUpdate(float dt);
    void ContributeCanvasOverlays(UIElement& canvasWrapper);
    void WriteLiveNestHost(Graph::Node& snapshotHost, const Graph::Node* liveHost,
                           GraphNestKind kind, bool liveTop) const;
    void OnNestChanged(const Graph::Node* host, GraphNestKind kind);
    void ApplyNestChrome(GraphNestKind kind);
    void LoadNestEditors(const Graph::Node& host, GraphNestKind kind);

private:
    void UpdateAnimationRuntimePreview();

    BlendSpace1DEditor* m_BlendSpaceEditor = nullptr;
    BlendSpace2DEditor* m_BlendSpace2DEditor = nullptr;
    bool m_AnimationRuntimePreviewWasPlaying = false;
    std::filesystem::path m_AnimationPreviewPath;
    GUID m_AnimationPreviewGraphGuid{};
    std::uint64_t m_AnimationPreviewRuntimeId = 0;
};

} // namespace GameEngine
