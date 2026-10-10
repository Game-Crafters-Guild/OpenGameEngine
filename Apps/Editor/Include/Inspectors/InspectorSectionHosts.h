#pragma once

#include "ECS/ComponentFieldRegistry.h" // ECS::ComponentTypeId

#include <vector>

namespace GameEngine::Editor
{

// Orders an entity's inspector sections so that each section host, a component
// whose traits host other sections (EditorComponentTraits::HostsInspectorSection),
// comes before the first section it hosts: the host's section has to exist when
// its entries are attached to it. A host in `sections` moves to the place of its
// first entry; every other section keeps its order, so a host and its entries
// sit where the first of them sorted.
void LeadWithSectionHosts(std::vector<ECS::ComponentTypeId>& sections);

} // namespace GameEngine::Editor
