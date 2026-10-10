#include "Editor/Entities/EditorComponentTraits.h"

#include "ECS/ComponentRegistry.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// Human-readable component name for diagnostics; the numeric id is the
// fallback for types whose registration is already gone.
std::string ComponentDisplayName(ECS::ComponentTypeId typeId)
{
    if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
        return info->Name;
    return "id " + std::to_string(typeId);
}

// A registered component type that `hosts` claims and that another component's
// traits (not `hostTypeId`'s own, which a re-registration replaces) already
// host, with that other host; false when the claims are disjoint.
bool FindSectionClaimedByAnotherHost(const EditorComponentTraitsRegistry& registry,
                                     ECS::ComponentTypeId hostTypeId,
                                     const std::function<bool(ECS::ComponentTypeId)>& hosts,
                                     ECS::ComponentTypeId& outHostedTypeId,
                                     ECS::ComponentTypeId& outOtherHostTypeId)
{
    std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>> otherHosts;
    for (auto& [typeId, traits] : registry.Snapshot())
    {
        if (typeId != hostTypeId && traits.HostsInspectorSection)
            otherHosts.emplace_back(typeId, std::move(traits));
    }
    if (otherHosts.empty())
        return false;
    for (const std::string& name : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const ECS::ComponentTypeId typeId = ECS::ComponentRegistry::GetComponentTypeId(name);
        if (!hosts(typeId))
            continue;
        for (const auto& [otherHostTypeId, otherTraits] : otherHosts)
        {
            if (otherTraits.HostsInspectorSection(typeId))
            {
                outHostedTypeId = typeId;
                outOtherHostTypeId = otherHostTypeId;
                return true;
            }
        }
    }
    return false;
}
} // namespace

EditorComponentTraitsRegistry& EditorComponentTraitsRegistry::Get()
{
    static EditorComponentTraitsRegistry s_Instance;
    return s_Instance;
}

void EditorComponentTraitsRegistry::Register(ECS::ComponentTypeId typeId, EditorComponentTraits traits)
{
    std::lock_guard<std::mutex> registration(m_RegistrationMutex);
    ECS::ComponentTypeId hostedTypeId{};
    ECS::ComponentTypeId otherHostTypeId{};
    if (traits.HostsInspectorSection &&
        FindSectionClaimedByAnotherHost(*this, typeId, traits.HostsInspectorSection, hostedTypeId,
                                        otherHostTypeId))
    {
        Logger::Log::Error("EditorComponentTraits: '{}' and '{}' both host the inspector section of '{}'. "
                           "A section has one host, so the traits of '{}' are not registered; make the "
                           "two HostsInspectorSection claims disjoint.",
                           ComponentDisplayName(otherHostTypeId), ComponentDisplayName(typeId),
                           ComponentDisplayName(hostedTypeId), ComponentDisplayName(typeId));
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Traits[typeId] = std::move(traits);
    m_ModuleOwners[typeId] = ECS::GetActiveRegistrationModule();
}

bool EditorComponentTraitsRegistry::TryGet(ECS::ComponentTypeId typeId, EditorComponentTraits& outTraits) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Traits.find(typeId);
    if (it == m_Traits.end())
        return false;
    outTraits = it->second;
    return true;
}

bool EditorComponentTraitsRegistry::TryGetSectionHost(ECS::ComponentTypeId hostedTypeId,
                                                      ECS::ComponentTypeId& outHostTypeId,
                                                      EditorComponentTraits& outHostTraits) const
{
    // The predicates run outside the lock: they are owner code, free to consult
    // registries of their own.
    std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>> hosts;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const auto& [typeId, traits] : m_Traits)
        {
            if (traits.HostsInspectorSection)
                hosts.emplace_back(typeId, traits);
        }
    }
    for (auto& [typeId, traits] : hosts)
    {
        if (traits.HostsInspectorSection(hostedTypeId))
        {
            outHostTypeId = typeId;
            outHostTraits = std::move(traits);
            return true;
        }
    }
    return false;
}

std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>> EditorComponentTraitsRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return {m_Traits.begin(), m_Traits.end()};
}

void EditorComponentTraitsRegistry::AppendModulePins(std::string_view moduleId,
                                                     std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& [typeId, stamp] : m_ModuleOwners)
    {
        if (stamp.ModuleId == moduleId)
            outPins.push_back("component traits '" + ComponentDisplayName(typeId) + "'");
    }
}

} // namespace GameEngine::Editor
