#include "InspectorRegistry.h"

#include "ECS/ComponentRegistry.h"

namespace GameEngine {

InspectorRegistry& InspectorRegistry::Get() {
    static InspectorRegistry s_Instance;
    return s_Instance;
}

void InspectorRegistry::AppendModulePins(std::string_view moduleId,
                                         std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& [typeId, stamp] : m_ModuleOwners)
    {
        if (stamp.ModuleId != moduleId)
            continue;
        if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            outPins.push_back("component inspector '" + info->Name + "'");
        else
            outPins.push_back("component inspector 'id " + std::to_string(typeId) + "'");
    }
    for (const auto& [type, stamp] : m_AssetInspectorOwners)
    {
        if (stamp.ModuleId == moduleId)
            outPins.push_back("asset inspector '" +
                              std::string(AssetTypeToString(static_cast<AssetType>(type))) + "'");
    }
}

bool InspectorRegistry::IsComponentInspectorOwnedBy(ECS::ComponentTypeId typeId,
                                                     std::string_view moduleId) const
{
    if (moduleId.empty())
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_ModuleOwners.find(typeId);
    return it != m_ModuleOwners.end() && it->second.ModuleId == moduleId;
}

bool InspectorRegistry::IsAssetInspectorOwnedBy(AssetType type, std::string_view moduleId) const
{
    if (moduleId.empty())
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_AssetInspectorOwners.find(static_cast<int>(type));
    return it != m_AssetInspectorOwners.end() && it->second.ModuleId == moduleId;
}

} // namespace GameEngine
