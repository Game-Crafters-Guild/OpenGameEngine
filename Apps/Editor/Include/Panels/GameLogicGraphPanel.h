#pragma once

#include "Graph/GraphFsmRuntime.h"
#include "Panels/GraphPanel.h"

namespace GameEngine {

class GameLogicGraphPanel final : public GraphPanel
{
public:
    GameLogicGraphPanel();

protected:
    /** States what a game logic graph contributes, and what it reacts to. */
    void Init();

    void OnUpdate(float dt);
    void OnCanvasPrimaryPress();
    bool BuildSampleGraph(Graph::Model& model);
    void CollectImpliedVariables(std::vector<Graph::Variable>& variables,
                                 std::unordered_set<std::string>& names) const;

private:
    void UpdateGameLogicRuntimePreview(float dt);
    void PollFsmContinueInput();
    void PlayFsmPreviewAudioForState(const std::string& stateNodeId);

    bool m_RuntimeGraphPreviewWasPlaying = false;
    float m_RuntimeGraphPreviewTimer = 0.f;
    size_t m_RuntimeGraphPreviewNextLink = 0;
    GraphFsmRuntime m_FsmRuntime;
    bool m_FsmRuntimeCompiled = false;
    bool m_FsmRuntimeDirty = true;
    std::string m_FsmPreviewAudioState;
    bool m_FsmCanvasPressPending = false;
};

} // namespace GameEngine
