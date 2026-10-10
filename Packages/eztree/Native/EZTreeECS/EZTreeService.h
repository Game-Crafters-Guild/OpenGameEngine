#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "EZTree/EZTreeGenerator.h"

#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;
}

namespace EZTreeECS
{

class EZTreeService
{
public:
    static EZTreeService& Get();
    static EZTreeService* TryGet();

    void Initialize(Engine::Renderer::RenderServices* renderServices);
    void Shutdown();

    Rendering::MeshGPUHandle RegenerateMesh(const GUID& key,
                                            const EZTree::TreeOptions& options,
                                            EZTree::GeneratedTree* outTree);
    void ReleaseMesh(const GUID& key);

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
};

} // namespace EZTreeECS
} // namespace GameEngine
