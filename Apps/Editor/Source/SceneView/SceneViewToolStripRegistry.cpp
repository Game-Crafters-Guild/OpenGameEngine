#include "SceneView/SceneViewToolStripRegistry.h"

#include "SceneView/SceneViewTools.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

SceneViewToolStripRegistry& SceneViewToolStripRegistry::Get()
{
    static SceneViewToolStripRegistry registry;
    return registry;
}

void SceneViewToolStripRegistry::Register(SceneViewToolStripEntry entry)
{
    const auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                                 [&entry](const SceneViewToolStripEntry& existing) { return existing.Id == entry.Id; });
    if (it != m_Entries.end())
        *it = std::move(entry);
    else
        m_Entries.push_back(std::move(entry));
}

const SceneViewToolStripEntry* SceneViewToolStripRegistry::Find(std::string_view id) const
{
    const auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                                 [id](const SceneViewToolStripEntry& entry) { return entry.Id == id; });
    return it == m_Entries.end() ? nullptr : &*it;
}

std::string RegisteredToolRefusal(const SceneViewToolStripRegistry& registry, std::string_view id, ECS::World* world)
{
    const SceneViewToolStripEntry* entry = registry.Find(id);
    if (!entry || !entry->CreateTool)
        return "Unknown tool: " + std::string(id);
    if (entry->IsAvailable && !entry->IsAvailable(world))
        return entry->UnavailableReason.empty() ? entry->Tooltip + " is not available now" : entry->UnavailableReason;
    return {};
}

} // namespace GameEngine::Editor
