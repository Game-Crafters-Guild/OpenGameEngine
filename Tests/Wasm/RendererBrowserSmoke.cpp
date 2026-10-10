// Browser rendering smoke for the wasm target (web platform plan, Phase 4).
// The engine's WebGPU backend brings up the real browser device: async
// adapter/device request, canvas surface + swapchain through the surface
// bridge, then a rAF-driven loop that clears to an animated color and draws
// one triangle through the backend's pipeline path (WGSL — browser WebGPU
// has no SPIR-V ingestion). One log line per milestone; a heartbeat every
// kHeartbeatFrames keeps the console proving liveness.

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <emscripten/emscripten.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{

using namespace GameEngine::Rendering;

constexpr int kHeartbeatFrames = 60;

// Clip-space triangle from the vertex index; solid orange. Entry points are
// "main" to match the backend's shared entry-point convention.
constexpr const char kTriangleVertWgsl[] = R"(
@vertex
fn main(@builtin(vertex_index) index : u32) -> @builtin(position) vec4f {
    var positions = array<vec2f, 3>(
        vec2f(-0.8, -0.8),
        vec2f(0.8, -0.8),
        vec2f(0.0, 0.8));
    return vec4f(positions[index], 0.0, 1.0);
}
)";

constexpr const char kTriangleFragWgsl[] = R"(
@fragment
fn main() -> @location(0) vec4f {
    return vec4f(1.0, 0.45, 0.1, 1.0);
}
)";

std::shared_ptr<const std::vector<uint8_t>> WgslBlob(const char* text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    // Exclude the trailing NUL: the blob is sized UTF-8 text.
    return std::make_shared<const std::vector<uint8_t>>(bytes, bytes + std::char_traits<char>::length(text));
}

struct SmokeState
{
    GLFWwindow* Window = nullptr;
    std::unique_ptr<IDevice> Device;
    WindowTargetHandle WindowTarget;
    PipelineHandle Pipeline;
    uint32_t Width = 0;
    uint32_t Height = 0;
    int Frame = 0;
    bool FirstFramePresented = false;
};

SmokeState g_State;

[[noreturn]] void Fail(const char* what)
{
    std::printf("SMOKE FAIL: %s\n", what);
    std::exit(1);
}

void Frame()
{
    SmokeState& s = g_State;

    // The canvas can be resized by CSS at any time: re-poll each frame and
    // recreate the swapchain on change.
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(s.Window, &width, &height);
    if (width > 0 && height > 0 &&
        (static_cast<uint32_t>(width) != s.Width || static_cast<uint32_t>(height) != s.Height))
    {
        s.Width = static_cast<uint32_t>(width);
        s.Height = static_cast<uint32_t>(height);
        if (s.Frame > 0)
        {
            s.Device->RecreateWindowTargetSwapchain(s.WindowTarget, s.Width, s.Height);
        }
    }

    if (!s.Device->BeginFrame())
    {
        return;
    }

    const TextureHandle backbuffer = s.Device->GetCurrentSwapchainImageHandle();
    if (!backbuffer.IsValid())
    {
        return;
    }

    auto commandList = s.Device->CreateCommandList(IDevice::QueueType::Graphics);
    commandList->Begin();

    RenderPassDesc pass{};
    pass.colorTargets[0] = backbuffer;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    const float phase = static_cast<float>(s.Frame) * 0.02f;
    pass.clearColorValue[0][0] = 0.1f;
    pass.clearColorValue[0][1] = 0.2f + 0.15f * std::sin(phase);
    pass.clearColorValue[0][2] = 0.45f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    commandList->BeginRenderPass(pass);
    commandList->SetPipeline(s.Pipeline);
    commandList->SetViewport(0.0f, 0.0f, static_cast<float>(s.Width), static_cast<float>(s.Height));
    commandList->SetScissor(0, 0, s.Width, s.Height);
    commandList->Draw(3);
    commandList->EndRenderPass();
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    s.Device->ExecuteCommandLists(lists);
    s.Device->Present();

    if (!s.FirstFramePresented)
    {
        s.FirstFramePresented = true;
        std::printf("SMOKE: first-frame-presented (%ux%u)\n", s.Width, s.Height);
    }
    ++s.Frame;
    if (s.Frame % kHeartbeatFrames == 0)
    {
        std::printf("SMOKE: heartbeat frame=%d\n", s.Frame);
    }
}

} // namespace

int main()
{
    // Deliberately no Log::Initialize: the uninitialized logger's synchronous
    // console fallback prints on the calling thread, so no drain-thread write
    // races the runtime's unflushed-stdio check at the first ASYNCIFY unwind.
    std::printf("SMOKE: boot (threads=%d)\n",
#if defined(GE_WASM_SINGLE_THREAD)
                0
#else
                1
#endif
    );

    if (glfwInit() != GLFW_TRUE)
    {
        Fail("glfwInit");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Initial canvas backing size; the per-frame poll follows CSS resizes.
    g_State.Window = glfwCreateWindow(1280, 720, "RendererBrowserSmoke", nullptr, nullptr);
    if (g_State.Window == nullptr)
    {
        Fail("glfwCreateWindow (canvas)");
    }

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::WebGPU;
    desc.enableSwapchain = true;
    desc.vsync = true;
    desc.applicationName = "RendererBrowserSmoke";
    g_State.Device = DeviceFactory::CreateDevice(desc);
    if (g_State.Device == nullptr || !g_State.Device->Initialize(desc))
    {
        Fail("WebGPU device init (adapter/device request)");
    }
    std::printf("SMOKE: adapter+device ready\n");

    g_State.WindowTarget = g_State.Device->CreateWindowTarget(g_State.Window, 1280, 720);
    if (!g_State.WindowTarget.IsValid() || !g_State.Device->SetActiveWindowTarget(g_State.WindowTarget))
    {
        Fail("canvas surface / swapchain creation");
    }
    std::printf("SMOKE: surface+swapchain ready\n");

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = WgslBlob(kTriangleVertWgsl);
    pipelineDesc.PixelShader = WgslBlob(kTriangleFragWgsl);
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "BrowserSmoke.Triangle";
    const GraphicsPipelineId pipelineId = g_State.Device->InternGraphicsPipeline(pipelineDesc);

    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = g_State.Device->GetSwapchainTextureFormat();
    formatKey.ColorCount = 1;
    g_State.Pipeline = g_State.Device->GetOrCreateGraphicsPipeline(pipelineId, formatKey);
    if (!g_State.Pipeline.IsValid())
    {
        Fail("triangle pipeline creation (WGSL)");
    }
    std::printf("SMOKE: pipeline ready\n");

    // rAF drive: the browser presents when each callback returns (the same
    // seam Application::RunPlatformLoop targets).
    emscripten_set_main_loop(&Frame, 0, /*simulate_infinite_loop*/ 0);
    return 0;
}
