#include "Inspectors/InspectorSectionHosts.h"

#include "Editor/Entities/EditorComponentTraits.h"

#include <algorithm>
#include <iterator>

namespace GameEngine::Editor
{

void LeadWithSectionHosts(std::vector<ECS::ComponentTypeId>& sections)
{
    const EditorComponentTraitsRegistry& registry = EditorComponentTraitsRegistry::Get();
    for (auto entry = sections.begin(); entry != sections.end(); ++entry)
    {
        ECS::ComponentTypeId hostTypeId{};
        EditorComponentTraits hostTraits;
        if (!registry.TryGetSectionHost(*entry, hostTypeId, hostTraits))
            continue;
        // A host already ahead of this entry is not found after it.
        const auto host = std::find(std::next(entry), sections.end(), hostTypeId);
        if (host != sections.end())
            entry = std::rotate(entry, host, std::next(host)); // the entry, now just after its host
    }
}

} // namespace GameEngine::Editor
