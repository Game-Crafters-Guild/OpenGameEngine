#include <cstdio>
#include <cmath>
#include <cstdlib>


#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UI/UIManager.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"

#include "FileWatcher/FileWatcher.h"
#include <filesystem>
#include <algorithm>

using namespace GameEngine;
#include "UI/UIPlatform.h"

namespace {
class GLFWPlatform final : public GameEngine::UI::IPlatformApi {
    GLFWwindow* m_Window;
public:
    explicit GLFWPlatform(GLFWwindow* w) : m_Window(w) {}
    std::string GetClipboardText() const override {
        const char* s = glfwGetClipboardString(m_Window);
        return s ? std::string(s) : std::string();
    }
    void SetClipboardText(const char* utf8) override {
        glfwSetClipboardString(m_Window, utf8 ? utf8 : "");
    }
};
}

using namespace GameEngine::Rendering;

int main() {
#if !defined(HAS_GLFW)
    std::printf("GLFW not available; UIWindowDemo disabled.\n");
    return 0;
#else
    // Verbose error callback to diagnose early exits
    glfwSetErrorCallback([](int code, const char* desc){
        std::fprintf(stderr, "[GLFW Error] %d: %s\n", code, desc ? desc : "");
    });
    const uint32_t kWidth = 1280, kHeight = 720;

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW.\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "UI Window Demo", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window.\n");
        glfwTerminate();
        return 1;
    }

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = true;
    desc.applicationName = "UIWindowDemo";

    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc)) {
        std::fprintf(stderr, "Failed to create/initialize device.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (!device->CreateAndActivateWindowTarget((void*)window, kWidth, kHeight)) {
        std::fprintf(stderr, "Failed to create window target.\n");
        device->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Scope UI and render graph resources so they destruct before device->Shutdown().
    {
    // Immediate-mode render graph: pools persist across frames; a fresh RGFrame is
    // declared every iteration. The UI overlay pass clears + owns the backbuffer.
    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transient(device.get());
    RenderGraph::RGUploadRing ring(device.get(), std::max(2u, device->GetFramesInFlight()), 1u << 20);
    uint64_t frameIndex = 0;

    UIManager ui(device.get());
    // Provide platform abstraction (clipboard etc.) without exposing GLFW to UI module
    GLFWPlatform platform(window);
    ui.SetPlatform(&platform);
    // Load layout and stylesheet from files (data-driven, no in-code UI construction)
    {
        const char* xmlCandidates[] = {
            "Assets/UI/UIDemoLayout.xml",
            "Assets/UI/HotReloadTestCSS.xml",
            "Assets/UI/HotReloadTest.xml"
        };
        const char* cssCandidates[] = {
            "Assets/UI/UIDemoStyles.css",
            "Assets/UI/HotReloadTestCSS.css"
        };
        bool layoutLoaded = false; const char* layoutUsed = nullptr;
        for (const char* p : xmlCandidates) { if (ui.LoadLayoutFromFile(p)) { std::printf("[UI Demo] Loaded layout: %s\n", p); layoutLoaded = true; layoutUsed = p; break; } }
        bool styleLoaded = false; const char* cssUsed = nullptr;
        for (const char* p : cssCandidates) { if (ui.AttachStyleFromFile(p)) { std::printf("[UI Demo] Loaded stylesheet: %s\n", p); styleLoaded = true; cssUsed = p; break; } }
        if (!layoutLoaded) std::printf("[UI Demo] Warning: failed to load any UI layout; UI will be empty.\n");
        if (!styleLoaded)  std::printf("[UI Demo] Warning: failed to load any UI stylesheet; defaults will apply.\n");
        if (layoutLoaded) {
            namespace fs = std::filesystem;
            fs::path lpath(layoutUsed ? layoutUsed : "");
            fs::path spath(cssUsed ? cssUsed : "");
            fs::path watchDir = lpath.has_parent_path() ? lpath.parent_path() : fs::path(".");
            const std::string layoutName = lpath.filename().string();
            const std::string cssName = spath.filename().string();

            static std::unique_ptr<GameEngine::FileWatcher> sWatcher;
            if (!sWatcher) sWatcher = std::make_unique<GameEngine::FileWatcher>();
            else sWatcher->StopWatching();
            sWatcher->ClearExtensionFilters();
            sWatcher->AddExtensionFilter(".xml");
            sWatcher->AddExtensionFilter(".uxml");
            sWatcher->AddExtensionFilter(".css");
            sWatcher->SetPollingInterval(250);
            sWatcher->SetCallback([&ui, watchDir, layoutName, cssName](const GameEngine::FileChangeEvent& ev){
                auto fname = ev.Path.filename().string();
                auto lower = [](std::string s){ for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
                std::string f = lower(fname);
                if (!layoutName.empty() && f == lower(layoutName)) {
                    std::printf("[UI Demo] Hot-reload layout: %s\n", ev.Path.string().c_str());
                    (void)ui.LoadLayoutFromFile((watchDir / layoutName).string());
                } else if (!cssName.empty() && f == lower(cssName)) {
                    std::printf("[UI Demo] Hot-reload stylesheet: %s\n", ev.Path.string().c_str());
                    (void)ui.AttachStyleFromFile((watchDir / cssName).string());
                }
            });
            if (!sWatcher->StartWatching(watchDir, /*recursive*/false)) {
                std::printf("[UI Demo] Failed to start hot-reload watcher in %s\n", watchDir.string().c_str());
    // Demo: subscribe simple click handlers for buttons
    {
        // Helper to find an element by id each time (works across hot-reloads)
        auto findById = [&ui](const std::string& targetId) -> UIElement* {
            std::function<UIElement*(UIElement*)> dfs = [&](UIElement* e) -> UIElement* {
                if (!e) return nullptr;
                if (e->GetId() == targetId) return e;
                for (auto& ch : e->GetChildren()) {
                    if (auto* r = dfs(ch.get())) return r;
                }
                return nullptr;
            };
            return dfs(ui.GetRootElement());
        };

        if (auto* e1 = findById("btn1")) {
            if (auto* b1 = dynamic_cast<GameEngine::Button*>(e1)) {
                b1->RegisterEventHandler(kEventButtonClick, [&ui](UIEvent&){
                    // Update the title label text when btn1 is clicked
                    auto find = [&ui](const std::string& id)->UIElement*{
                        std::function<UIElement*(UIElement*)> dfs = [&](UIElement* e)->UIElement*{
                            if (!e) return nullptr; if (e->GetId()==id) return e;
                            for (auto& ch : e->GetChildren()) { if (auto* r = dfs(ch.get())) return r; }
                            return nullptr;
                        };
                        return dfs(ui.GetRootElement());
                    };
                    if (auto* tl = find("titleLabel")) {
                        if (auto* lbl = dynamic_cast<GameEngine::Label*>(tl)) {
                            lbl->SetText("Primary clicked");
                        }
                    }
                    if (auto* ce = find("clickEcho")) {
                        if (auto* lbl = dynamic_cast<GameEngine::Label*>(ce)) {
                            lbl->SetText("Primary clicked");
                        }
                    }
                    std::printf("[UI Demo] btn1 clicked\n");
                });
            }
        }
        if (auto* e2 = findById("btn2")) {
            if (auto* b2 = dynamic_cast<GameEngine::Button*>(e2)) {
                b2->RegisterEventHandler(kEventButtonClick, [&ui](UIEvent&){
                    auto find = [&ui](const std::string& id)->UIElement*{
                        std::function<UIElement*(UIElement*)> dfs = [&](UIElement* e)->UIElement*{
                            if (!e) return nullptr; if (e->GetId()==id) return e;
                            for (auto& ch : e->GetChildren()) { if (auto* r = dfs(ch.get())) return r; }
                            return nullptr;
                        };
                        return dfs(ui.GetRootElement());
                    };
                    if (auto* ce = find("clickEcho")) {
                        if (auto* lbl = dynamic_cast<GameEngine::Label*>(ce)) {
                            lbl->SetText("Secondary clicked");
                        }
                    }
                    std::printf("[UI Demo] btn2 clicked\n");
                });
            }
        }
    }

            }
        }
    }


    // Wire basic mouse input for :hover/:active/:focus
    glfwSetWindowUserPointer(window, &ui);
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y){
        if (auto* u = reinterpret_cast<UIManager*>(glfwGetWindowUserPointer(w))) u->OnMouseMove((float)x, (float)y);
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoff, double yoff){
        if (auto* u = reinterpret_cast<UIManager*>(glfwGetWindowUserPointer(w))) u->OnScroll((float)xoff, (float)yoff);
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int /*mods*/){
        if (auto* u = reinterpret_cast<UIManager*>(glfwGetWindowUserPointer(w))) u->OnMouseButton(button, action == GLFW_PRESS);
    });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int codepoint){
        if (auto* u = reinterpret_cast<UIManager*>(glfwGetWindowUserPointer(w))) u->OnChar(codepoint);
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int /*scancode*/, int action, int mods){
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            if (auto* u = reinterpret_cast<UIManager*>(glfwGetWindowUserPointer(w))) u->OnKey(key, action, mods);
        }
    });
    glfwSetWindowCloseCallback(window, [](GLFWwindow* /*w*/){
        std::printf("[UI Demo] Window close requested.\n");
    });

    double lastTime = glfwGetTime();
    const char* minSecsEnv = std::getenv("UI_DEMO_MIN_SECONDS");
    // Exit immediately on close by default; override with UI_DEMO_MIN_SECONDS if needed
    double minRunSeconds = 0.0;
    if (minSecsEnv && *minSecsEnv) {
        double v = std::atof(minSecsEnv);
        if (v > 0.0) minRunSeconds = v;
    }
    double startTime = lastTime;
    std::printf("[UI Demo] Entering main loop (min %.2fs).\n", minRunSeconds);

    while (true) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        if (!device->BeginFrame()) {
            continue; // device may be resizing
        }

        RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(frameIndex++);
        const RenderGraph::RGTexture backbuffer = frame.ImportBackbuffer();

        ui.Update(dt);
        // RenderRG declares the UI overlay pass, which clears (black) + owns the
        // backbuffer and draws the UI. Execute submits; the device presents.
        // The backbuffer is display-bound: its space follows the output mode.
        ui.RenderRG(frame, backbuffer,
                    UI::UITargetSpace::ForDisplay(device->GetActiveHdrOutputMode()));

        frame.Execute();
        device->Present();

        // Allow at least minRunSeconds before honoring close
        if (glfwWindowShouldClose(window) && (now - startTime) >= minRunSeconds) {
            std::printf("[UI Demo] Exiting after %.2fs.\n", now - startTime);
            break;
        }
    }
    } // UI + RenderGraph resources released here

    device->Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
#endif
}

