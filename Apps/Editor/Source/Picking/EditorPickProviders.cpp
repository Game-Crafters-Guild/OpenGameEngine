#include "Picking/EditorPickProviders.h"

#include "ECS/ComponentRegistry.h"

namespace GameEngine::Editor::Picking
{

EditorPickProviderRegistry& EditorPickProviderRegistry::Get()
{
    static EditorPickProviderRegistry s_Instance;
    return s_Instance;
}

void EditorPickProviderRegistry::Register(ECS::ComponentTypeId typeId, EditorPickProvider provider)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Providers[typeId] = std::move(provider);
    m_ModuleOwners[typeId] = ECS::GetActiveRegistrationModule();
}

std::vector<EditorPickProvider> EditorPickProviderRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<EditorPickProvider> out;
    out.reserve(m_Providers.size());
    for (const auto& [typeId, provider] : m_Providers)
        out.push_back(provider);
    return out;
}

void EditorPickProviderRegistry::AppendModulePins(std::string_view moduleId,
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
            outPins.push_back("pick provider '" + info->Name + "'");
        else
            outPins.push_back("pick provider 'id " + std::to_string(typeId) + "'");
    }
}

} // namespace GameEngine::Editor::Picking
