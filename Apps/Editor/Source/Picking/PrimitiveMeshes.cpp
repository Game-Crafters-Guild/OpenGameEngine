#include "Picking/PrimitiveMeshes.h"

#include <mutex>
#include <unordered_map>

#include "Assets/ModelAsset.h"
#include "Engine/Rendering/PrimitiveGenerator.h"

namespace GameEngine::Editor::Picking
{

const Mesh* ResolvePrimitiveMesh(const GUID& guid)
{
    using PG = ::GameEngine::Engine::Renderer::PrimitiveGenerator;
    if (!PG::IsPrimitive(guid))
        return nullptr;

    static std::mutex s_Mutex;
    static std::unordered_map<GUID, Mesh, std::hash<GUID>> s_Cache;

    std::lock_guard lock(s_Mutex);
    auto it = s_Cache.find(guid);
    if (it != s_Cache.end())
        return &it->second;

    Mesh mesh;
    if (guid == PG::CubeGuid())          mesh = PG::GenerateCube();
    else if (guid == PG::SphereGuid())   mesh = PG::GenerateSphere();
    else if (guid == PG::CapsuleGuid())  mesh = PG::GenerateCapsule();
    else if (guid == PG::PlaneGuid())        mesh = PG::GeneratePlane();
    else if (guid == PG::PlaneSpriteUvGuid()) mesh = PG::GeneratePlaneSpriteUv();
    else                                      return nullptr;

    auto ins = s_Cache.emplace(guid, std::move(mesh));
    return &ins.first->second;
}

}
