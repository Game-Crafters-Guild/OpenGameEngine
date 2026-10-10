#include "Panels/AnimationGraphPanel.h"

#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Assets/AssetManager.h"
#include "Components/Animation/Animator.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "EditorContext.h"
#include "Graph/BlendSpace1DEditor.h"
#include "Graph/BlendSpace2DEditor.h"
#include "Graph/GraphAnimationRuntimeDebug.h"
#include "Graph/GraphBlendSpace1DStore.h"
#include "Graph/GraphBlendSpace2DStore.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNest.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <utility>

namespace GameEngine {

AnimationGraphPanel::AnimationGraphPanel()
    : GraphPanel(Graph::kKindIdAnimation)
{
    Init();
    SetupUI();
    SetTitle("Animation Graph");
}

void AnimationGraphPanel::Init()
{
    GraphKindHooks hooks;
    hooks.OnUpdate = [this](float dt) { OnUpdate(dt); };
    hooks.ContributeChrome = [this](GraphPanelRegion region, UIElement& host)
    {
        if (region == GraphPanelRegion::CanvasOverlays)
            ContributeCanvasOverlays(host);
    };
    hooks.Nest.WriteLiveHost = [this](Graph::Node& snapshotHost, const Graph::Node* liveHost,
                                      GraphNestKind kind, bool liveTop)
    { WriteLiveNestHost(snapshotHost, liveHost, kind, liveTop); };
    hooks.Nest.OnChanged = [this](const Graph::Node* host, GraphNestKind kind)
    { OnNestChanged(host, kind); };
    SetKindHooks(std::move(hooks));
}

void AnimationGraphPanel::OnNestChanged(const Graph::Node* host, GraphNestKind kind)
{
    ApplyNestChrome(kind);
    if (host)
        LoadNestEditors(*host, kind);
}

void AnimationGraphPanel::OnUpdate(float dt)
{
    (void)dt;
    UpdateAnimationRuntimePreview();
}

void AnimationGraphPanel::ContributeCanvasOverlays(UIElement& canvasWrapper)
{
    auto blendUndo = [this](const std::string& actionName, std::function<void()> mutate)
    {
        CommitLiveGraphMutation(actionName.c_str(), std::move(mutate));
    };
    auto blendChanged = [this]()
    {
        WriteLiveNestToHosts();
        MarkDirty();
    };

    auto blendEditor = std::make_unique<BlendSpace1DEditor>();
    blendEditor->SetId("NodeGraphBlendSpace1DEditor");
    blendEditor->AddClass("hidden");
    blendEditor->SetUndoScope(blendUndo);
    blendEditor->SetOnChanged(blendChanged);
    m_BlendSpaceEditor = blendEditor.get();
    canvasWrapper.AddChild(std::move(blendEditor));

    auto blend2Editor = std::make_unique<BlendSpace2DEditor>();
    blend2Editor->SetId("NodeGraphBlendSpace2DEditor");
    blend2Editor->AddClass("hidden");
    blend2Editor->SetUndoScope(blendUndo);
    blend2Editor->SetOnChanged(blendChanged);
    m_BlendSpace2DEditor = blend2Editor.get();
    canvasWrapper.AddChild(std::move(blend2Editor));
}

void AnimationGraphPanel::WriteLiveNestHost(Graph::Node& snapshotHost, const Graph::Node* liveHost,
                                                GraphNestKind kind, bool liveTop) const
{
    if (kind == GraphNestKind::BlendSpace1D)
    {
        std::vector<BlendSpace1DSampleDesc> samples;
        if (liveTop && m_BlendSpaceEditor)
            samples = m_BlendSpaceEditor->GetSamples();
        else if (liveHost)
            GraphBlendSpace1DStore::TryLoad(*liveHost, samples);
        GraphBlendSpace1DStore::Store(snapshotHost, samples);
        return;
    }
    if (kind == GraphNestKind::BlendSpace2D)
    {
        std::vector<BlendSpace2DSampleDesc> samples;
        if (liveTop && m_BlendSpace2DEditor)
            samples = m_BlendSpace2DEditor->GetSamples();
        else if (liveHost)
            GraphBlendSpace2DStore::TryLoad(*liveHost, samples);
        GraphBlendSpace2DStore::Store(snapshotHost, samples);
    }
}

void AnimationGraphPanel::ApplyNestChrome(GraphNestKind kind)
{
    if (m_BlendSpaceEditor)
    {
        if (kind == GraphNestKind::BlendSpace1D)
            m_BlendSpaceEditor->RemoveClass("hidden");
        else
            m_BlendSpaceEditor->AddClass("hidden");
        m_BlendSpaceEditor->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_BlendSpace2DEditor)
    {
        if (kind == GraphNestKind::BlendSpace2D)
            m_BlendSpace2DEditor->RemoveClass("hidden");
        else
            m_BlendSpace2DEditor->AddClass("hidden");
        m_BlendSpace2DEditor->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void AnimationGraphPanel::LoadNestEditors(const Graph::Node& host, GraphNestKind kind)
{
    if (kind == GraphNestKind::BlendSpace1D && m_BlendSpaceEditor)
    {
        std::vector<BlendSpace1DSampleDesc> samples;
        GraphBlendSpace1DStore::TryLoad(host, samples);
        std::string parameterName = host.Parameters.GetString("parameter", "Speed");
        if (parameterName.empty())
            parameterName = "Speed";
        m_BlendSpaceEditor->SetSamples(std::move(samples));
        m_BlendSpaceEditor->SetParameterName(std::move(parameterName));
        return;
    }
    if (kind == GraphNestKind::BlendSpace2D && m_BlendSpace2DEditor)
    {
        std::vector<BlendSpace2DSampleDesc> samples;
        GraphBlendSpace2DStore::TryLoad(host, samples);
        std::string parameterNameX = host.Parameters.GetString("parameterX", "Speed");
        std::string parameterNameY = host.Parameters.GetString("parameterY", "Direction");
        if (parameterNameX.empty())
            parameterNameX = "Speed";
        if (parameterNameY.empty())
            parameterNameY = "Direction";
        m_BlendSpace2DEditor->SetSamples(std::move(samples));
        m_BlendSpace2DEditor->SetParameterNameX(std::move(parameterNameX));
        m_BlendSpace2DEditor->SetParameterNameY(std::move(parameterNameY));
    }
}

void AnimationGraphPanel::UpdateAnimationRuntimePreview()
{
    const bool playing = Canvas() != nullptr &&
                         Context() != nullptr &&
                         Context()->PlayMode != nullptr &&
                         Context()->PlayMode->IsPlayingOrPaused() &&
                         Context()->World != nullptr &&
                         Context()->Assets != nullptr &&
                         !GetCurrentGraphPath().empty();

    auto clearBlendPreview = [this]()
    {
        if (m_BlendSpaceEditor)
            m_BlendSpaceEditor->SetPreviewPosition(false, 0.f);
        if (m_BlendSpace2DEditor)
            m_BlendSpace2DEditor->SetPreviewPosition(false, 0.f, 0.f);
    };

    if (!playing)
    {
        if (m_AnimationRuntimePreviewWasPlaying && Canvas())
            Canvas()->ClearRuntimeVisualization();
        if (m_AnimationRuntimePreviewWasPlaying)
            clearBlendPreview();
        m_AnimationRuntimePreviewWasPlaying = false;
        m_AnimationPreviewPath.clear();
        m_AnimationPreviewGraphGuid = {};
        m_AnimationPreviewRuntimeId = 0;
        return;
    }
    m_AnimationRuntimePreviewWasPlaying = true;

    const GUID graphGuid = Context()->Assets->ResolveAssetGuid(GetCurrentGraphPath());
    if (graphGuid.IsNull())
    {
        Canvas()->ClearRuntimeVisualization();
        clearBlendPreview();
        m_AnimationPreviewPath.clear();
        m_AnimationPreviewGraphGuid = {};
        m_AnimationPreviewRuntimeId = 0;
        return;
    }

    const Animation::AnimationGraphPlayer* player = nullptr;
    if (m_AnimationPreviewRuntimeId != 0 && m_AnimationPreviewPath == GetCurrentGraphPath() &&
        m_AnimationPreviewGraphGuid == graphGuid)
        player = Animation::AnimationGraphStore::Instance().Get(m_AnimationPreviewRuntimeId);
    if (!player)
    {
        m_AnimationPreviewRuntimeId = 0;
        Context()->World->Query<ECS::Read<Components::Animator>>().IncludeDisabled().Each(
            [&](ECS::EntityHandle, const Components::Animator& animator)
            {
                if (player)
                    return;
                if (animator.graphGuid.Guid != graphGuid || animator.graphRuntimeId == 0)
                    return;
                player = Animation::AnimationGraphStore::Instance().Get(animator.graphRuntimeId);
                if (player)
                    m_AnimationPreviewRuntimeId = animator.graphRuntimeId;
            });
        if (player)
        {
            m_AnimationPreviewPath = GetCurrentGraphPath();
            m_AnimationPreviewGraphGuid = graphGuid;
        }
        else
        {
            m_AnimationPreviewPath.clear();
            m_AnimationPreviewGraphGuid = {};
        }
    }
    if (!player)
    {
        Canvas()->ClearRuntimeVisualization();
        clearBlendPreview();
        return;
    }

    const GraphNestKind nest = CurrentNestKind();
    if (nest == GraphNestKind::BlendSpace1D && m_BlendSpaceEditor)
    {
        float value = 0.f;
        const bool has =
            GraphAnimationRuntimeDebug::TryPreviewFloat(*player, m_BlendSpaceEditor->GetParameterName(), value);
        m_BlendSpaceEditor->SetPreviewPosition(has, value);
        if (m_BlendSpace2DEditor)
            m_BlendSpace2DEditor->SetPreviewPosition(false, 0.f, 0.f);
        return;
    }
    if (nest == GraphNestKind::BlendSpace2D && m_BlendSpace2DEditor)
    {
        float x = 0.f;
        float y = 0.f;
        const bool hasX =
            GraphAnimationRuntimeDebug::TryPreviewFloat(*player, m_BlendSpace2DEditor->GetParameterNameX(), x);
        const bool hasY =
            GraphAnimationRuntimeDebug::TryPreviewFloat(*player, m_BlendSpace2DEditor->GetParameterNameY(), y);
        m_BlendSpace2DEditor->SetPreviewPosition(hasX && hasY, x, y);
        if (m_BlendSpaceEditor)
            m_BlendSpaceEditor->SetPreviewPosition(false, 0.f);
        return;
    }

    clearBlendPreview();

    const auto* sm = dynamic_cast<const Animation::StateMachineNode*>(player->RootNode.get());
    if (nest == GraphNestKind::PoseGraph)
    {
        Graph::Model* pose = ActiveGraphModel();
        if (!pose || !sm || NestStack().size() < 2)
        {
            Canvas()->ClearRuntimeVisualization();
            return;
        }
        const GraphNestFrame& poseFrame = NestStack().back();
        const GraphNestFrame& smFrame = NestStack()[NestStack().size() - 2];
        if (smFrame.Kind != GraphNestKind::StateMachine ||
            !GraphAnimationRuntimeDebug::StateIsEvaluating(smFrame.Model, poseFrame.HostNodeId, *sm))
        {
            Canvas()->ClearRuntimeVisualization();
            return;
        }
        Canvas()->ClearRuntimeVisualization();
        const GraphAnimationRuntimeHighlight highlight = GraphAnimationRuntimeDebug::ForPoseGraph(*pose);
        Canvas()->SetRuntimeActiveNodes(highlight.NodeIds);
        return;
    }

    if (!sm)
    {
        Canvas()->ClearRuntimeVisualization();
        return;
    }

    if (nest == GraphNestKind::StateMachine)
    {
        const Graph::Model* nested = ActiveGraphModel();
        if (!nested)
        {
            Canvas()->ClearRuntimeVisualization();
            return;
        }
        const GraphAnimationRuntimeHighlight highlight =
            GraphAnimationRuntimeDebug::ForStateMachine(*nested, *sm);
        Canvas()->SetRuntimeActiveNodes(highlight.NodeIds);
        Canvas()->AddRuntimeTransitionPulses(highlight.LinkIds);
        return;
    }

    if (nest == GraphNestKind::Root)
    {
        std::unordered_set<std::string> ids;
        for (const Graph::Node& node : RootModel().Nodes)
        {
            if (node.TypeId == "StateMachine")
                ids.insert(node.Id);
        }
        Canvas()->SetRuntimeActiveNodes(ids);
        return;
    }

    Canvas()->ClearRuntimeVisualization();
}

} // namespace GameEngine
