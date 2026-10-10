#include "Panels/GameLogicGraphPanel.h"

#include "AssetCore/GUID.h"
#include "Audio/AudioSystem.h"
#include "Core/Engine.h"
#include "EditorContext.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNodeRegistry.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine {
namespace {

constexpr const char* kFsmContinueEvent = "Continue";

bool IsGameLogicVariableNode(const Graph::Node& node)
{
    return node.TypeId == "GetVariable" || node.TypeId == "SetVariable";
}

} // namespace

GameLogicGraphPanel::GameLogicGraphPanel()
    : GraphPanel(Graph::kKindIdGameLogic)
{
    Init();
    SetupUI();
    SetTitle("Game Logic Graph");
}

void GameLogicGraphPanel::Init()
{
    GraphKindHooks hooks;
    hooks.OnUpdate = [this](float dt) { OnUpdate(dt); };
    hooks.Sample.Label = "Game Logic Example Graph";
    hooks.Sample.SearchKey = "GameLogicExampleGraph";
    hooks.Sample.Build = [this](Graph::Model& m) { return BuildSampleGraph(m); };
    hooks.CollectImpliedVariables =
        [this](std::vector<Graph::Variable>& variables, std::unordered_set<std::string>& names)
    { CollectImpliedVariables(variables, names); };
    SetKindHooks(std::move(hooks));

    Listen(GraphPanelEvent::ModelChanged, [this]() { m_FsmRuntimeDirty = true; });
    Listen(GraphPanelEvent::Opened, [this]() {
        m_FsmRuntimeCompiled = false;
        m_FsmRuntimeDirty = true;
    });
    Listen(GraphPanelEvent::CanvasPrimaryPress, [this]() { OnCanvasPrimaryPress(); });
}

void GameLogicGraphPanel::OnUpdate(float dt)
{
    UpdateGameLogicRuntimePreview(dt);
}

void GameLogicGraphPanel::OnCanvasPrimaryPress()
{
    m_FsmCanvasPressPending = true;
}

void GameLogicGraphPanel::UpdateGameLogicRuntimePreview(float dt)
{
    const bool playing = Canvas() != nullptr &&
                         Context() != nullptr &&
                         Context()->PlayMode != nullptr &&
                         Context()->PlayMode->IsPlaying() &&
                         !RootModel().Links.empty();
    if (!playing)
    {
        if (m_RuntimeGraphPreviewWasPlaying && Canvas())
            Canvas()->ClearRuntimeVisualization();
        m_RuntimeGraphPreviewWasPlaying = false;
        m_RuntimeGraphPreviewTimer = 0.f;
        m_RuntimeGraphPreviewNextLink = 0;
        m_FsmRuntime.Reset();
        m_FsmRuntimeCompiled = false;
        m_FsmRuntimeDirty = true;
        m_FsmPreviewAudioState.clear();
        m_FsmCanvasPressPending = false;
        return;
    }

    if (!m_RuntimeGraphPreviewWasPlaying)
    {
        m_RuntimeGraphPreviewWasPlaying = true;
        m_RuntimeGraphPreviewTimer = 0.f;
        m_RuntimeGraphPreviewNextLink = 0;
    }

    if (m_FsmRuntimeDirty)
    {
        std::string error;
        m_FsmRuntimeCompiled = m_FsmRuntime.Build(RootModel(), error);
        m_FsmRuntimeDirty = false;
        m_FsmPreviewAudioState.clear();
        if (!m_FsmRuntimeCompiled)
            Canvas()->ClearRuntimeVisualization();
    }

    if (m_FsmRuntimeCompiled)
    {
        PollFsmContinueInput();
        m_FsmRuntime.Tick(dt);

        const std::string& activeState = m_FsmRuntime.GetActiveStateNodeId();
        if (!activeState.empty() && activeState != m_FsmPreviewAudioState)
        {
            PlayFsmPreviewAudioForState(activeState);
            m_FsmPreviewAudioState = activeState;
        }

        std::unordered_set<std::string> activeNodes;
        auto addActive = [&activeNodes](const std::string& id)
        {
            if (!id.empty())
                activeNodes.insert(id);
        };
        addActive(activeState);
        addActive(m_FsmRuntime.GetLastFiredSourceStateNodeId());
        addActive(m_FsmRuntime.GetLastFiredTransitionNodeId());
        addActive(m_FsmRuntime.GetLastFiredTargetStateNodeId());
        Canvas()->SetRuntimeActiveNodes(activeNodes);
        Canvas()->AddRuntimeTransitionPulses(m_FsmRuntime.GetLastFiredTransitionLinkIds());
        return;
    }

    constexpr float kPreviewPulseInterval = 0.28f;
    m_RuntimeGraphPreviewTimer += std::max(0.f, dt);
    if (m_RuntimeGraphPreviewTimer < kPreviewPulseInterval)
        return;
    m_RuntimeGraphPreviewTimer = std::fmod(m_RuntimeGraphPreviewTimer, kPreviewPulseInterval);

    const size_t linkCount = RootModel().Links.size();
    for (size_t attempt = 0; attempt < linkCount; ++attempt)
    {
        const Graph::Edge& link = RootModel().Links[m_RuntimeGraphPreviewNextLink % linkCount];
        m_RuntimeGraphPreviewNextLink = (m_RuntimeGraphPreviewNextLink + 1) % linkCount;
        if (link.Id.empty())
            continue;

        Canvas()->AddRuntimeTransitionPulses({link.Id});
        Canvas()->SetRuntimeActiveNodes({link.SourceNodeId, link.TargetNodeId});
        break;
    }
}

void GameLogicGraphPanel::PollFsmContinueInput()
{
    if (!Canvas())
        return;

    const bool clicked = m_FsmCanvasPressPending;
    m_FsmCanvasPressPending = false;

    Input::InputSystem* input = EngineCore::GetInstance().GetInputSystem();
    const bool keyed = input && (input->WasKeyPressed(Input::kKeyCode_Enter)
                                 || input->WasKeyPressed(Input::kKeyCode_NumPadEnter)
                                 || input->WasKeyPressed(Input::kKeyCode_Space));
    if (!keyed && !clicked)
        return;

    if (keyed)
    {
        UIManager* ui = GetOwnerManager();
        bool overCanvas = false;
        for (UIElement* p = ui ? ui->GetHoveredElement() : nullptr; p; p = p->GetParent())
        {
            if (p == Canvas())
            {
                overCanvas = true;
                break;
            }
        }
        if (!overCanvas && !clicked)
            return;
    }

    m_FsmRuntime.TriggerEvent(kFsmContinueEvent);
}

void GameLogicGraphPanel::PlayFsmPreviewAudioForState(const std::string& stateNodeId)
{
    Audio::AudioSystem* audioSys = EngineCore::GetInstance().GetAudioSystem();
    if (!audioSys || !audioSys->IsInitialized())
        return;

    const Graph::Node* node = RootModel().FindNode(stateNodeId);
    if (!node || node->TypeId != "State")
        return;

    GUID clipToPlay{};

    if (auto randIt = node->Parameters.find("playRandomSounds"); randIt != node->Parameters.end())
    {
        std::vector<GUID> usable;
        std::string chunk;
        auto flush = [&]()
        {
            while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.front())))
                chunk.erase(chunk.begin());
            while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.back())))
                chunk.pop_back();
            if (!chunk.empty())
            {
                GUID g(chunk);
                if (!g.IsNull())
                    usable.push_back(g);
            }
            chunk.clear();
        };
        const std::string randomSounds = randIt->second.ToString();
        for (char c : randomSounds)
        {
            if (c == ';' || c == ',')
                flush();
            else
                chunk.push_back(c);
        }
        flush();
        if (!usable.empty())
        {
            thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<size_t> dist(0, usable.size() - 1);
            clipToPlay = usable[dist(rng)];
        }
    }

    if (clipToPlay.IsNull())
    {
        auto singleIt = node->Parameters.find("playSound");
        if (singleIt == node->Parameters.end())
            return;
        clipToPlay = GUID(singleIt->second.ToString());
    }

    if (!clipToPlay.IsNull())
        audioSys->Play2D(clipToPlay);
}

bool GameLogicGraphPanel::BuildSampleGraph(Graph::Model& model)
{
    GraphNodeRegistry& registry = GraphNodeRegistry::Get();
    model.KindId = std::string(Graph::kKindIdGameLogic);
    model.Nodes.clear();
    model.Links.clear();
    model.Variables.clear();
    model.Viewport = Graph::Viewport{-80.f, -40.f, 1.f};

    std::uint64_t nextVariableOrder = 0;
    auto addVariable = [&](std::string name, std::string type, std::string value, bool isPublic)
    {
        Graph::Variable variable;
        variable.Name = std::move(name);
        variable.Type = std::move(type);
        variable.Value = std::move(value);
        variable.IsPublic = isPublic;
        variable.CreatedOrder = nextVariableOrder++;
        model.Variables.push_back(std::move(variable));
    };

    addVariable("BirdState", "String", "Flying", true);
    addVariable("FlapForce", "Float", "8.5", true);
    addVariable("SkyHour", "Float", "18.25", true);

    auto addNode = [&](const std::string& typeId, float x, float y) -> std::string
    {
        std::string id = model.GenerateNodeId();
        Graph::Node node = registry.CreateNode(model.KindId, typeId, id, x, y);
        model.Nodes.push_back(std::move(node));
        return id;
    };
    auto link = [&](const std::string& srcId, const std::string& srcPort,
                    const std::string& tgtId, const std::string& tgtPort)
    {
        Graph::Edge L;
        L.Id = model.GenerateLinkId();
        L.SourceNodeId = srcId;
        L.SourcePortId = srcPort;
        L.TargetNodeId = tgtId;
        L.TargetPortId = tgtPort;
        model.Links.push_back(std::move(L));
    };

    const std::string entryId = addNode("Entry", 80.f, 80.f);
    const std::string sequenceId = addNode("Sequence", 320.f, 80.f);
    const std::string skyId = addNode("SetSkyTimeOfDay", 560.f, 40.f);
    const std::string findCameraId = addNode("FindEntityByName", 560.f, 240.f);
    const std::string cameraFollowId = addNode("ThirdPersonCameraFollow", 840.f, 40.f);
    const std::string setStateId = addNode("SetVariable", 1120.f, 80.f);

    const std::string flapInputId = addNode("GetInputAction", 560.f, 440.f);
    const std::string flapBranchId = addNode("Branch", 840.f, 360.f);
    const std::string flapForceId = addNode("MakeVector3", 840.f, 560.f);
    const std::string addForceId = addNode("AddForce", 1120.f, 320.f);
    const std::string flapSoundId = addNode("PlaySound", 1400.f, 320.f);

    const std::string moveAxisId = addNode("GetInputAxis", 840.f, 760.f);
    const std::string axisAbsId = addNode("FloatAbs", 1120.f, 760.f);
    const std::string moveCheckId = addNode("CompareFloat", 1120.f, 560.f);
    const std::string cruiseVelocityId = addNode("MakeVector3", 1120.f, 960.f);
    const std::string setVelocityId = addNode("SetVelocity", 1400.f, 560.f);

    const std::string collisionId = addNode("CollisionEvent", 560.f, 1160.f);
    const std::string damageSoundId = addNode("PlaySound", 840.f, 1120.f);
    const std::string triggerId = addNode("TriggerEvent", 840.f, 1360.f);
    const std::string reachedEventId = addNode("SendEvent", 1400.f, 1360.f);

    link(entryId, "out", sequenceId, "in");
    link(sequenceId, "out0", skyId, "in");
    link(skyId, "out", cameraFollowId, "in");
    link(cameraFollowId, "out", setStateId, "in");
    link(findCameraId, "entity", cameraFollowId, "camera");

    link(sequenceId, "out1", flapBranchId, "in");
    link(flapInputId, "pressed", flapBranchId, "condition");
    link(flapBranchId, "true", addForceId, "in");
    link(flapForceId, "vector", addForceId, "force");
    link(addForceId, "out", flapSoundId, "in");

    link(flapBranchId, "false", moveCheckId, "in");
    link(moveAxisId, "value", axisAbsId, "value");
    link(axisAbsId, "result", moveCheckId, "a");
    link(moveCheckId, "true", setVelocityId, "in");
    link(cruiseVelocityId, "vector", setVelocityId, "velocity");

    link(sequenceId, "out2", collisionId, "in");
    link(collisionId, "hit", damageSoundId, "in");
    link(collisionId, "miss", triggerId, "in");
    link(triggerId, "entered", reachedEventId, "in");

    if (Graph::Node* n = model.FindNode(skyId))
    {
        n->Parameters["hours"] = "18.25";
        n->Parameters["animate"] = "true";
        n->Parameters["cycleSeconds"] = "180";
    }
    if (Graph::Node* n = model.FindNode(findCameraId))
        n->Parameters["name"] = "Main Camera";
    if (Graph::Node* n = model.FindNode(cameraFollowId))
    {
        n->Parameters["target"] = "self";
        n->Parameters["offset"] = "0, 2.6, -7";
        n->Parameters["lookOffset"] = "0, 1.25, 0";
        n->Parameters["positionSmoothing"] = "8";
        n->Parameters["rotationSmoothing"] = "12";
    }
    if (Graph::Node* n = model.FindNode(setStateId))
    {
        n->Parameters["variableName"] = "BirdState";
        n->Parameters["value"] = "Flying";
    }
    if (Graph::Node* n = model.FindNode(flapInputId))
        n->Parameters["action"] = "Bird.Flap";
    if (Graph::Node* n = model.FindNode(flapForceId))
    {
        n->Parameters["x"] = "0";
        n->Parameters["y"] = "8.5";
        n->Parameters["z"] = "2";
    }
    if (Graph::Node* n = model.FindNode(flapSoundId))
    {
        n->Parameters["clipGuid"] = "Stuff/sfx/abstract-gong-5-171209.mp3";
        n->Parameters["volume"] = "0.35";
        n->Parameters["pitch"] = "1.35";
        n->Parameters["loop"] = "false";
        n->Parameters["spatialized"] = "true";
        n->Parameters["bus"] = "2";
    }
    if (Graph::Node* n = model.FindNode(moveAxisId))
        n->Parameters["axis"] = "Bird.MoveX";
    if (Graph::Node* n = model.FindNode(moveCheckId))
    {
        n->Parameters["b"] = "0.1";
        n->Parameters["comparison"] = "greater";
    }
    if (Graph::Node* n = model.FindNode(cruiseVelocityId))
    {
        n->Parameters["x"] = "0";
        n->Parameters["y"] = "0";
        n->Parameters["z"] = "4.5";
    }
    if (Graph::Node* n = model.FindNode(collisionId))
    {
        n->Parameters["phase"] = "enter";
        n->Parameters["tag"] = "Hazard";
    }
    if (Graph::Node* n = model.FindNode(damageSoundId))
    {
        n->Parameters["clipGuid"] = "Stuff/sfx/dizzy-ellectric-bolt-spell-1-186768.mp3";
        n->Parameters["volume"] = "0.75";
        n->Parameters["pitch"] = "0.9";
        n->Parameters["loop"] = "false";
        n->Parameters["spatialized"] = "true";
        n->Parameters["bus"] = "2";
    }
    if (Graph::Node* n = model.FindNode(triggerId))
    {
        n->Parameters["phase"] = "enter";
        n->Parameters["tag"] = "Perch";
    }
    if (Graph::Node* n = model.FindNode(reachedEventId))
        n->Parameters["eventName"] = "PterodactylReachedPerch";

    return true;
}

void GameLogicGraphPanel::CollectImpliedVariables(std::vector<Graph::Variable>& variables,
                                                      std::unordered_set<std::string>& names) const
{
    for (const auto& node : RootModel().Nodes)
    {
        if (!IsGameLogicVariableNode(node))
            continue;
        auto it = node.Parameters.find("variableName");
        if (it == node.Parameters.end())
            continue;
        const std::string name = it->second.ToString();
        if (name.empty() || names.count(name))
            continue;
        Graph::Variable variable;
        variable.Name = name;
        variable.Type = "float";
        variable.Value = "0";
        variable.CreatedOrder = std::numeric_limits<std::uint64_t>::max() - variables.size();
        variables.push_back(std::move(variable));
        names.insert(name);
    }
}

} // namespace GameEngine
