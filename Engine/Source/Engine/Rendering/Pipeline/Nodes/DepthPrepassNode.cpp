#include "Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
bool DepthPrepassNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object())
        {
            if (j.contains("depth") && j["depth"].is_string())
                m_DepthRef = j["depth"].get<std::string>();
            if (j.contains("clearDepthValue") && j["clearDepthValue"].is_number())
                m_ClearDepthValue = j["clearDepthValue"].get<float>();
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void DepthPrepassNode::DeclareForView(ViewDeclare& d)
{
    RenderGraph::RGTexture depth = d.ViewDepth;
    if (!m_DepthRef.empty() && m_DepthRef != Names::View::Depth)
    {
        const RenderGraph::RGTexture o = d.ResolveTexture(m_DepthRef);
        if (o.IsValid() && d.ViewDepth.IsValid() && o.Id != d.ViewDepth.Id)
        {
            // The override replaces the view depth as the world pass's depth
            // attachment — it must match extent + sample count (same guard
            // and per-view warn dedup as the old path, descs read from the
            // frame instead of the retained graph).
            const auto& od = d.Frame.Graph().ResourceDesc(o.Id);
            const auto& vd = d.Frame.Graph().ResourceDesc(d.ViewDepth.Id);
            if (od.SampleCount != vd.SampleCount || od.Width != vd.Width ||
                od.Height != vd.Height)
            {
                if (m_DepthMismatchWarned.insert(d.View.id).second)
                    LOG_ERROR(
                        "DepthPrepass '{}': depth override '{}' ({}x{}, {}x MSAA) does not "
                        "match the view depth ({}x{}, {}x MSAA); ignoring it.",
                        m_Id, m_DepthRef, od.Width, od.Height, od.SampleCount, vd.Width,
                        vd.Height, vd.SampleCount);
            }
            else
            {
                m_DepthMismatchWarned.erase(d.View.id);
                depth = o;
                // Every LATER node in blueprint order (world, depth resolve)
                // sees the override through the blackboard. The pre-pass seeded
                // View.DepthResolved with the ORIGINAL view depth; track the
                // override here too, or a skipped/absent DepthResolve leaves
                // world's ge_sceneDepth bound to the never-written original.
                d.PublishTexture(Names::View::Depth, o);
                d.PublishTexture(Names::View::DepthResolved, o);
            }
        }
    }
    if (!depth.IsValid())
        return;
    d.Services.AddWorldDepthPrepassForView(d.Frame, d.View.id, depth, m_ClearDepthValue);
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
