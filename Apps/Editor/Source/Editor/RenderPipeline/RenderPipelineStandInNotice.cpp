#include "Editor/RenderPipeline/RenderPipelineStandInNotice.h"

#include "EditorContext.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderServices.h"
#include "UI/Controls/InspectorNotice.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";
constexpr const char* kStyleAssetPath = "UI/controls/ViewOverlay/RenderPipelineStandInNotice.css";
void SetShown(UIElement& element, bool shown)
{
    if (shown)
        element.RemoveClass(kHiddenClass);
    else
        element.AddClass(kHiddenClass);
}

// The names, quoted and separated by commas.
std::string QuotedList(const std::vector<std::string>& names)
{
    std::string list;
    for (const std::string& name : names)
        list += (list.empty() ? "'" : ", '") + name + "'";
    return list;
}
} // namespace

std::string RenderPipelineStandInNotice::Describe(const Engine::Renderer::PipelineStandIn& standIn)
{
    const std::string requested = "'" + standIn.RequestedPath.generic_string() + "' can't be used";
    if (!standIn.StandInName.empty())
        return requested + ", so this view draws the engine's default pipeline instead. " + standIn.Reason;
    return requested + ", and the engine's default pipeline can't stand in for it, so this view draws nothing. " +
           standIn.Reason + " " + standIn.StandInFailure;
}

std::string RenderPipelineStandInNotice::DescribeWait(const Engine::Renderer::PipelineStandIn& standIn,
                                                      int64_t seconds)
{
    const bool onePass = standIn.PendingPasses.size() == 1;
    std::string waiting = "'" + standIn.RequestedPath.generic_string() + "' uses " + (onePass ? "a pass" : "passes") +
                          " from the project's scripts (" + QuotedList(standIn.PendingPasses) +
                          "), which are still building (" + std::to_string(seconds) + " s so far)";
    if (!standIn.WaitingReaders.empty())
        waiting += "; " + QuotedList(standIn.WaitingReaders) +
                   (standIn.WaitingReaders.size() == 1 ? " reads " : " read ") + (onePass ? "its" : "their") +
                   " output and " + (standIn.WaitingReaders.size() == 1 ? "waits" : "wait") + " with " +
                   (onePass ? "it" : "them");
    waiting += ". ";
    const bool oneWaiting = standIn.PendingPasses.size() + standIn.WaitingReaders.size() == 1;
    if (standIn.LastVersionDraws)
        return waiting + "The pipeline's output comes from " + (oneWaiting ? "that pass" : "them") +
               ", so this view keeps the pipeline's last version until then.";
    if (standIn.StandInName.empty() && standIn.StandInFailure.empty())
        return waiting + "This view draws the rest of the pipeline until then; " +
               (oneWaiting ? "that pass joins" : "they join") + " once the scripts are built.";
    if (!standIn.StandInName.empty())
        return waiting + "The pipeline's output comes from " + (oneWaiting ? "that pass" : "them") +
               ", so this view draws the engine's default pipeline until then; the project's pipeline replaces it "
               "once its scripts are built.";
    return waiting + "The engine's default pipeline can't stand in, so this view draws nothing until then. " +
           standIn.StandInFailure;
}

RenderPipelineStandInNotice::RenderPipelineStandInNotice(const EditorContext& context)
    : m_Context(context)
{
}

void RenderPipelineStandInNotice::Present(ViewOverlayView /*view*/, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    auto notice = std::make_unique<EditorUI::InspectorNotice>(std::string(), EditorUI::InspectorNotice::Kind::Warning);
    notice->AddClass("inspector-notice-opaque");
    notice->AddClass("render-pipeline-notice");
    notice->AddClass("render-pipeline-stand-in-notice");
    notice->AddClass(kHiddenClass);
    notice->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    notice->SetTitle("Render pipeline refused");
    m_Notices.push_back(UIElement::MakeWeakRef(notice.get()));
    layer.AddChild(std::move(notice));

    auto wait = std::make_unique<EditorUI::InspectorNotice>(std::string(), EditorUI::InspectorNotice::Kind::Information);
    wait->AddClass("inspector-notice-opaque");
    wait->AddClass("render-pipeline-notice");
    wait->AddClass("render-pipeline-waiting-note");
    wait->AddClass(kHiddenClass);
    wait->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    wait->SetTitle("Waiting for scripts");
    m_WaitNotes.push_back(UIElement::MakeWeakRef(wait.get()));
    layer.AddChild(std::move(wait));
    // Notices presented while a stand-in draws or a wait runs show it from the next update.
    m_ShownOnce = false;
}

void RenderPipelineStandInNotice::Update()
{
    m_Notices.erase(std::remove_if(m_Notices.begin(), m_Notices.end(),
                                   [](const UIElement::WeakRef<EditorUI::InspectorNotice>& notice)
                                   { return notice.Get() == nullptr; }),
                    m_Notices.end());
    m_WaitNotes.erase(std::remove_if(m_WaitNotes.begin(), m_WaitNotes.end(),
                                     [](const UIElement::WeakRef<EditorUI::InspectorNotice>& note)
                                     { return note.Get() == nullptr; }),
                      m_WaitNotes.end());

    Engine::Renderer::RenderServices* services = m_Context.RenderServices;
    const Engine::Renderer::PipelineStandIn* standIn = services ? &services->Spine().ActivePipelineStandIn() : nullptr;
    const uint64_t generation = standIn ? standIn->Generation : 0;
    const bool waiting = standIn && standIn->WaitingForModules;
    const auto now = std::chrono::steady_clock::now();
    if (waiting && m_ShownWaitSeconds < 0)
        m_WaitStarted = now;
    const int64_t waitSeconds =
        waiting ? std::chrono::duration_cast<std::chrono::seconds>(now - m_WaitStarted).count() : -1;
    if (m_ShownOnce && services == m_ShownServices && generation == m_ShownGeneration &&
        waitSeconds == m_ShownWaitSeconds)
        return;
    m_ShownOnce = true;
    m_ShownServices = services;
    m_ShownGeneration = generation;
    m_ShownWaitSeconds = waitSeconds;

    ShowRefusal(standIn && standIn->IsActive() && !waiting ? Describe(*standIn) : std::string());
    ShowWait(waiting ? DescribeWait(*standIn, waitSeconds) : std::string());
}

void RenderPipelineStandInNotice::ShowRefusal(const std::string& text)
{
    for (const UIElement::WeakRef<EditorUI::InspectorNotice>& weak : m_Notices)
    {
        EditorUI::InspectorNotice* notice = weak.Get();
        notice->SetText(text);
        SetShown(*notice, !text.empty());
    }
}

void RenderPipelineStandInNotice::ShowWait(const std::string& text)
{
    for (const UIElement::WeakRef<EditorUI::InspectorNotice>& weak : m_WaitNotes)
    {
        EditorUI::InspectorNotice* note = weak.Get();
        note->SetText(text);
        SetShown(*note, !text.empty());
    }
}

} // namespace GameEngine::Editor
