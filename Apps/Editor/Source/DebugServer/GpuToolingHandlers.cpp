#include "DebugServer/GpuToolingHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/NsightGraphicsCapture.h"
#include "EditorApplication.h"

#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

#include <nlohmann/json.hpp>

#include <memory>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{
// Nsight serializes a capture over many frames -- far longer than a RenderDoc
// .rdc -- and a multi-frame request stretches it further. Budget generously;
// the poll costs one counter read per frame.
constexpr int kNsightCapturePollBudget = 300;
constexpr uint32_t kDefaultNsightCaptureFrames = 1;
} // namespace

void RegisterGpuToolingHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // get_gpu_tooling — one answer to "what sits between this engine and the GPU
    // right now": whose hardware, whether GPU debug labels actually resolved on
    // this device, whether the validation layer is active, and which capture or
    // profiling tools have attached themselves.
    //
    // Every field is observed at runtime. debugUtilsEnabled reports that the
    // label entry points resolved on THIS device — an engine built with label
    // calls compiled into it reports false whenever they did not, which is the
    // fact a capture preflight needs and the one no compile-time constant can
    // give.
    //
    // toolingInfoAvailable false means the runtime could not be asked at all,
    // which is distinct from an empty attachedTools — that means it was asked
    // and reports nothing attached.
    server.RegisterHandler("get_gpu_tooling", [](const EditorDebugServer::RequestContext&) -> json
    {
        auto* renderServices = EngineCore::GetInstance().GetRenderServices();
        auto* device = renderServices ? renderServices->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No rendering device");

        const Rendering::RenderingDeviceCapabilities& caps = device->GetCapabilities();
        const Rendering::GpuToolingReport report = device->GetGpuToolingReport();

        json attachedTools = json::array();
        for (const Rendering::AttachedGraphicsTool& tool : report.AttachedTools)
        {
            json entry{{"name", tool.Name},
                       {"version", tool.Version},
                       {"purposes", tool.Purposes},
                       {"purposeBits", tool.PurposeBits},
                       {"description", tool.Description}};
            if (!tool.Layer.empty())
                entry["layer"] = tool.Layer;
            attachedTools.push_back(std::move(entry));
        }

        return json{{"vendor", Rendering::GpuVendorName(caps.vendor)},
                    {"vendorId", caps.vendorId},
                    {"hardware", device->GetHardwareDescription()},
                    {"debugUtilsEnabled", report.DebugLabelsAvailable},
                    {"validationLayerEnabled", report.ValidationLayerEnabled},
                    {"toolingInfoAvailable", report.ToolingQueryAvailable},
                    {"attachedTools", attachedTools}};
    });

    // nsight_capture — NVIDIA Nsight Graphics frame capture through the in-app
    // SDK. Only works when the editor was launched by ngfx-capture.exe: the
    // capture libraries arrive by injection, and this process never loads them
    // itself.
    //
    // get_gpu_tooling's attachedTools does NOT report Nsight: it hooks the
    // loader through ngfx-capture-interception.dll rather than registering a
    // Vulkan layer, so VK_EXT_tooling_info never sees it. This handler's own
    // availability answer is the check; to confirm injection out-of-band, look
    // for that DLL in the process module list.
    server.RegisterHandler("nsight_capture", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* nsight = app.m_NsightCapture.get();
        if (!nsight || !nsight->IsAvailable())
            return Editor::RefuseRequest("Nsight Graphics not available (editor not launched under ngfx-capture). "
                                         "Relaunch it with: node mcp/scripts/ngfx-capture-editor.mjs");

        const uint32_t countBefore = nsight->GetCaptureCount();

        // Answer a rejected request now. Deferring one would poll for a file
        // that is never coming and time the caller out with a generic message.
        if (auto rejection = nsight->TriggerCapture(ctx.params.value("frames", kDefaultNsightCaptureFrames)))
            return Editor::RefuseRequest(*rejection);

        // The capture file only exists once Nsight has finished serializing it,
        // several frames after the request. Poll the completed-file count
        // instead of calling the SDK's blocking wait, which would stall the
        // main thread for the whole serialization.
        //
        // The count is process-wide, so two captures in flight at once can each
        // resolve to the other's file. Same tradeoff as trigger_capture; capture
        // is a one-at-a-time debugging action in practice.
        auto state = std::make_shared<uint32_t>(countBefore);
        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto* nsight = app.m_NsightCapture.get();
                if (!nsight)
                    return false;
                const uint32_t current = nsight->GetCaptureCount();
                if (current <= *state)
                    return false; // not ready yet

                outResult = {
                    {"captured", true},
                    {"captureIndex", current - 1},
                    {"filePath", nsight->GetLastCapturePath()}
                };
                return true;
            },
            kNsightCapturePollBudget);

        return EditorDebugServer::DeferredMarker(); });
}

} // namespace GameEngine
