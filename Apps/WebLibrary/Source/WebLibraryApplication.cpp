#include "WebLibraryApplication.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Platform/Window.h"

#include <utility>

namespace GameEngine::WebLibrary
{

namespace
{

// The render pipeline the engine module ships with (the compatibility profile's graph).
constexpr const char* kRenderPipeline = "RenderPipelines/Web.rendergraph";

RuntimeHostDesc MakeHostDesc(const ApplicationConfig& config)
{
    RuntimeHostDesc desc;
    desc.Title = config.Name;
    desc.WindowWidth = config.WindowWidth;
    desc.WindowHeight = config.WindowHeight;
    desc.RenderPipeline = kRenderPipeline;
    return desc;
}

} // namespace

WebLibraryApplication::WebLibraryApplication(const ApplicationConfig& config)
    : Application(config)
{
    RuntimeHostHooks hooks;
    hooks.OnDeviceFailed = [this] { RequestExit(); };
    m_Host = std::make_unique<RuntimeHost>(*this, MakeHostDesc(config), std::move(hooks));
}

WebLibraryApplication::~WebLibraryApplication()
{
    Shutdown();
}

bool WebLibraryApplication::Start(std::string& outError)
{
    if (!Initialize())
    {
        outError = "the engine failed to initialize; the browser console has the engine's log.";
        return false;
    }
    if (!m_Host->InitWindow())
    {
        outError = "the window could not be created on the canvas.";
        return false;
    }

    // The engine's own content (shaders, the render pipeline) is unpacked under the asset
    // root, so the 'editor' source is the same root mounted under its own alias, as in a web
    // Player dist.
    auto& engine = EngineCore::GetInstance();
    auto& assets = engine.GetAssetManager();
    if (const auto editorSource =
            BuildPlayerEditorSourceDesc(engine.GetResolvedAssetRoot(), PathUtils::GetInstallAssetsRoot()))
        assets.RegisterSource(*editorSource);

    if (!m_Host->InitRendering())
    {
        outError = "the WebGPU device could not be created on the canvas.";
        return false;
    }
    if (auto* renderServices = GetRenderServices())
        Engine::Renderer::PrimitiveGenerator::RegisterAll(*renderServices);

    assets.WaitForStartupScan(kAssetSourceAliasProject);

    if (m_Host->LoadRenderPipeline() != Engine::Renderer::PipelineResolveFailure::None)
    {
        outError = std::string("the render pipeline '") + kRenderPipeline +
                   "' could not be loaded; serve opengine-core.gepak beside the engine module.";
        return false;
    }
    if (!engine.EnsurePrimaryWorld())
    {
        outError = "the primary world could not be created.";
        return false;
    }
    return true;
}

void WebLibraryApplication::Resize(int width, int height)
{
    if (Platform::Window* window = m_Host->GetWindow())
        window->SetWindowSize(width, height);
}

Engine::Renderer::RenderServices* WebLibraryApplication::GetRenderServices() const
{
    Engine::Renderer::RenderDeviceContext* context = m_Host->GetRenderDeviceContext();
    return context ? context->GetRenderServices() : nullptr;
}

void WebLibraryApplication::Update(float64 deltaTime)
{
    m_LastRenderDeltaTime = static_cast<float>(deltaTime);
}

void WebLibraryApplication::Render()
{
    m_Host->Render(m_LastRenderDeltaTime);
}

void WebLibraryApplication::OnShutdown()
{
    m_Host->Shutdown();
}

} // namespace GameEngine::WebLibrary
