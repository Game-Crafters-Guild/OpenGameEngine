#include "EZTreeECS/EZTreeService.h"

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::EZTreeECS
{

EZTreeService& EZTreeService::Get()
{
    static EZTreeService service;
    return service;
}

EZTreeService* EZTreeService::TryGet()
{
    return &Get();
}

void EZTreeService::Initialize(Engine::Renderer::RenderServices* renderServices)
{
    m_RenderServices = renderServices;
}

void EZTreeService::Shutdown()
{
    m_RenderServices = nullptr;
}

Rendering::MeshGPUHandle EZTreeService::RegenerateMesh(const GUID& key,
                                                       const EZTree::TreeOptions& options,
                                                       EZTree::GeneratedTree* outTree)
{
    EZTree::Generator generator;
    EZTree::GeneratedTree tree = generator.Generate(options);
    Rendering::MeshGPUHandle handle{};
    if (m_RenderServices)
    {
        handle = m_RenderServices->GetMeshGPURegistry().RegisterSubmesh(
            Rendering::MeshGPUKey{key, 0u},
            tree.combined,
            true);
    }
    if (outTree)
        *outTree = std::move(tree);
    return handle;
}

void EZTreeService::ReleaseMesh(const GUID& key)
{
    if (!m_RenderServices || key.IsNull())
        return;
    m_RenderServices->GetMeshGPURegistry().UnregisterModel(key);
}

} // namespace GameEngine::EZTreeECS
