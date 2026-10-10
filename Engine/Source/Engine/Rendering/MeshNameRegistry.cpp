#include "Engine/Rendering/MeshNameRegistry.h"

#include "Components/Rendering/MeshRenderer.h"

#include <mutex>
#include <unordered_map>

namespace GameEngine
{
namespace Engine
{
namespace Renderer
{
namespace
{

std::mutex g_MeshNameMutex;

// id -> the first spelling seen for it. HashMeshName folds ASCII case and an
// interior "_LOD<N>" token, so several spellings share one id BY DESIGN and all
// of them select the same submesh; which one this table keeps is therefore a
// cosmetic choice, and keeping the first makes a load/save pair stable.
std::unordered_map<StringId, std::string>& MeshNamesLocked()
{
    static std::unordered_map<StringId, std::string> names;
    return names;
}

} // namespace

StringId InternMeshName(std::string_view name)
{
    if (name.empty())
        return 0;

    const StringId id = Components::HashMeshName(name);
    const std::lock_guard<std::mutex> lock(g_MeshNameMutex);
    MeshNamesLocked().emplace(id, std::string(name));
    return id;
}

std::string FindMeshName(StringId id)
{
    if (id == 0)
        return {};

    const std::lock_guard<std::mutex> lock(g_MeshNameMutex);
    auto& names = MeshNamesLocked();
    const auto it = names.find(id);
    // By value: the table keeps growing under its own lock, and a view into it
    // would outlive the guard.
    return it != names.end() ? it->second : std::string{};
}

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
