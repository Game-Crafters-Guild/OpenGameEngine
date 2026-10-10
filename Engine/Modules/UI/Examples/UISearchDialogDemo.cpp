#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Types/StringUtils.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UI/UIManager.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/StyleProperties.h"
#include "UI/UIPlatform.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Minimal GLFW clipboard bridge for IPlatformApi.
class GLFWPlatform final : public GameEngine::UI::IPlatformApi
{
    GLFWwindow* m_Window;

public:
    explicit GLFWPlatform(GLFWwindow* w) : m_Window(w) {}
    std::string GetClipboardText() const override
    {
        const char* s = glfwGetClipboardString(m_Window);
        return s ? std::string(s) : std::string();
    }
    void SetClipboardText(const char* utf8) override
    {
        glfwSetClipboardString(m_Window, utf8 ? utf8 : "");
    }
};

// Simple synchronous search provider with mock data (simulating an asset browser).
class MockAssetProvider : public ISearchProvider
{
public:
    MockAssetProvider()
    {
        // Populate with representative asset entries
        struct Entry
        {
            const char* Name;
            const char* Detail;
            const char* IconClass;
        };
        static constexpr Entry kEntries[] = {
            {"Albedo.png", "Textures/PBR/Albedo.png", "icon-texture"},
            {"Wood_Normal.tga", "Textures/PBR/Wood_Normal.tga", "icon-texture"},
            {"Brick_AO.png", "Textures/PBR/Brick_AO.png", "icon-texture"},
            {"Water_Roughness.png", "Textures/Environment/Water_Roughness.png", "icon-texture"},
            {"GrassTile.png", "Textures/Terrain/GrassTile.png", "icon-texture"},
            {"SkyboxHDR.hdr", "Textures/Environment/SkyboxHDR.hdr", "icon-texture"},
            {"StoneWall.mat", "Materials/StoneWall.mat", "icon-material"},
            {"GlossyMetal.mat", "Materials/GlossyMetal.mat", "icon-material"},
            {"WaterSurface.mat", "Materials/WaterSurface.mat", "icon-material"},
            {"Toon.shader", "Shaders/Toon.shader", "icon-shader"},
            {"PBR_Standard.shader", "Shaders/PBR_Standard.shader", "icon-shader"},
            {"UnlitTransparent.shader", "Shaders/UnlitTransparent.shader", "icon-shader"},
            {"Character.glb", "Models/Character.glb", "icon-model"},
            {"Tree_Oak.glb", "Models/Foliage/Tree_Oak.glb", "icon-model"},
            {"Sword.fbx", "Models/Weapons/Sword.fbx", "icon-model"},
            {"Building_House.glb", "Models/Architecture/Building_House.glb", "icon-model"},
            {"PlayerController.cs", "Scripts/PlayerController.cs", "icon-script"},
            {"EnemyAI.cs", "Scripts/AI/EnemyAI.cs", "icon-script"},
            {"InventorySystem.cs", "Scripts/UI/InventorySystem.cs", "icon-script"},
            {"Footstep_Dirt.wav", "Audio/SFX/Footstep_Dirt.wav", "icon-audio"},
            {"AmbientForest.ogg", "Audio/Music/AmbientForest.ogg", "icon-audio"},
            {"Level_01.scene", "Scenes/Level_01.scene", "icon-scene"},
            {"MainMenu.scene", "Scenes/MainMenu.scene", "icon-scene"},
            {"Walk.anim", "Animations/Character/Walk.anim", "icon-animation"},
            {"Idle.anim", "Animations/Character/Idle.anim", "icon-animation"},
            {"Attack_Slash.anim", "Animations/Combat/Attack_Slash.anim", "icon-animation"},
        };

        SearchItemId nextId = 1;
        for (const auto& entry : kEntries)
        {
            SearchResultItem item;
            item.Id = nextId++;
            item.Label = entry.Name;
            item.Detail = entry.Detail;
            item.Icon = SearchIcon::FromClass(entry.IconClass);
            item.UserData = std::string(entry.Detail);
            m_Items.push_back(std::move(item));
        }
    }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        std::vector<SearchResultItem> results;

        for (const auto& item : m_Items)
        {
            if (query.empty() ||
                ContainsIgnoreCase(item.Label, query) ||
                ContainsIgnoreCase(item.Detail, query))
            {
                results.push_back(item);
            }
        }

        sink(std::move(results), /*isComplete=*/true);
    }

    void CancelSearch() override {}

    std::string GetPlaceholderText() const override { return "Search assets..."; }

private:
    std::vector<SearchResultItem> m_Items;
};

} // namespace

int main()
{
#if !defined(HAS_GLFW)
    std::printf("GLFW not available; UISearchDialogDemo disabled.\n");
    return 0;
#else
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "[GLFW Error] %d: %s\n", code, desc ? desc : "");
    });

    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;

    if (!glfwInit())
    {
        std::fprintf(stderr, "Failed to initialize GLFW.\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window =
        glfwCreateWindow(kWidth, kHeight, "SearchDialog Demo (Ctrl+P to open)", nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "Failed to create GLFW window.\n");
        glfwTerminate();
        return 1;
    }

    // ---- Device + RenderGraph ----
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = true;
    desc.applicationName = "UISearchDialogDemo";

    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
    {
        std::fprintf(stderr, "Failed to create/initialize device.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (!device->CreateAndActivateWindowTarget((void*)window, kWidth, kHeight))
    {
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

    // ---- UIManager ----
    UIManager ui(device.get());
    GLFWPlatform platform(window);
    ui.SetPlatform(&platform);

    // Load a minimal dark theme (tokens) so CSS variables resolve correctly.
    // Try the Editor's tokens first, then a relative path for builds.
    const char* themeCandidates[] = {
        "Assets/UI/theme/tokens.css",
        "../../../Apps/Editor/Assets/UI/theme/tokens.css",
        "../../Apps/Editor/Assets/UI/theme/tokens.css",
    };
    for (const char* p : themeCandidates)
    {
        if (ui.AttachStyleFromFile(p))
        {
            std::printf("[SearchDialog Demo] Loaded theme: %s\n", p);
            break;
        }
    }

    // Load the SearchDialog stylesheet
    const char* cssCandidates[] = {
        "Assets/UI/controls/SearchDialog.css",
        "../../../Apps/Editor/Assets/UI/controls/SearchDialog.css",
        "../../Apps/Editor/Assets/UI/controls/SearchDialog.css",
    };
    for (const char* p : cssCandidates)
    {
        if (ui.AttachStyleFromFile(p))
        {
            std::printf("[SearchDialog Demo] Loaded SearchDialog CSS: %s\n", p);
            break;
        }
    }

    // ---- Build UI tree ----
    // Instruction label (shown behind the dialog)
    auto instructionLabel = std::make_unique<Label>();
    instructionLabel->SetText("Press Ctrl+P to open the search dialog");
    instructionLabel->Overrides()
        .Set(Style::Color, 0xFF888888u)
        .Set(Style::FontSize, StyleLength::Px(16.0f))
        .Set(Style::MarginTop, StyleLength::Px(20.0f))
        .Set(Style::MarginLeft, StyleLength::Px(20.0f));
    auto* instructionPtr = instructionLabel.get();

    auto statusLabel = std::make_unique<Label>();
    statusLabel->Overrides()
        .Set(Style::Color, 0xFF666666u)
        .Set(Style::FontSize, StyleLength::Px(13.0f))
        .Set(Style::MarginTop, StyleLength::Px(8.0f))
        .Set(Style::MarginLeft, StyleLength::Px(20.0f));
    auto* statusPtr = statusLabel.get();

    // SearchDialog + provider
    MockAssetProvider provider;

    auto dialog = std::make_unique<SearchDialog>();
    dialog->SetProvider(&provider);
    auto* dialogPtr = dialog.get();

    dialog->SetOnResult([statusPtr, instructionPtr](const SearchResultItem& item) {
        std::printf("[SearchDialog Demo] Selected: %s (%s)\n",
                    item.Label.c_str(), item.Detail.c_str());
        statusPtr->SetText("Selected: " + item.Label + " (" + item.Detail + ")");
        instructionPtr->SetText("Press Ctrl+P to search again");
    });
    dialog->SetOnCancel([statusPtr, instructionPtr]() {
        std::printf("[SearchDialog Demo] Cancelled\n");
        statusPtr->SetText("Search cancelled");
        instructionPtr->SetText("Press Ctrl+P to search again");
    });

    // Assemble tree: root has instructions + dialog overlay
    auto root = std::make_unique<UIElement>();
    root->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f));
    root->AddChild(std::move(instructionLabel));
    root->AddChild(std::move(statusLabel));
    root->AddChild(std::move(dialog));
    ui.SetRoot(std::move(root));

    // ---- GLFW input wiring ----
    struct UserData
    {
        UIManager* ui;
        SearchDialog* dialog;
    };
    static UserData userData{&ui, dialogPtr};
    glfwSetWindowUserPointer(window, &userData);

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        auto* ud = reinterpret_cast<UserData*>(glfwGetWindowUserPointer(w));
        ud->ui->OnMouseMove((float)x, (float)y);
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoff, double yoff) {
        auto* ud = reinterpret_cast<UserData*>(glfwGetWindowUserPointer(w));
        ud->ui->OnScroll((float)xoff, (float)yoff);
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int /*mods*/) {
        auto* ud = reinterpret_cast<UserData*>(glfwGetWindowUserPointer(w));
        ud->ui->OnMouseButton(button, action == GLFW_PRESS);
    });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int codepoint) {
        auto* ud = reinterpret_cast<UserData*>(glfwGetWindowUserPointer(w));
        ud->ui->OnChar(codepoint);
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int /*scancode*/, int action, int mods) {
        auto* ud = reinterpret_cast<UserData*>(glfwGetWindowUserPointer(w));

        // Ctrl+P toggles the search dialog
        if (key == GLFW_KEY_P && (mods & GLFW_MOD_CONTROL) && action == GLFW_PRESS)
        {
            if (ud->dialog->IsOpen())
                ud->dialog->Close();
            else
                ud->dialog->Show();
            return;
        }

        if (action == GLFW_PRESS || action == GLFW_REPEAT)
            ud->ui->OnKey(key, action, mods);
    });

    // ---- Main loop ----
    double lastTime = glfwGetTime();
    const char* minSecsEnv = std::getenv("UI_DEMO_MIN_SECONDS");
    double minRunSeconds = 0.0;
    if (minSecsEnv && *minSecsEnv)
    {
        double v = std::atof(minSecsEnv);
        if (v > 0.0)
            minRunSeconds = v;
    }
    double startTime = lastTime;
    std::printf("[SearchDialog Demo] Entering main loop. Press Ctrl+P to open search.\n");

    while (true)
    {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        if (!device->BeginFrame())
            continue;

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

        if (glfwWindowShouldClose(window) && (now - startTime) >= minRunSeconds)
        {
            std::printf("[SearchDialog Demo] Exiting after %.2fs.\n", now - startTime);
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
