// The order the Inspector builds an entity's sections in when one component hosts
// others (a volume and the effects of its stack): the host's section has to exist
// before an entry attaches to it, and nothing else may move, because the order is
// what the user sees and drags.

#include <gtest/gtest.h>

#include "Editor/Entities/EditorComponentTraits.h"
#include "Inspectors/InspectorSectionHosts.h"

#include <vector>

namespace
{
using GameEngine::ECS::ComponentTypeId;
namespace ed = GameEngine::Editor;

constexpr ComponentTypeId kHost = 0xE1A0;
constexpr ComponentTypeId kEntryA = 0xE1A1;
constexpr ComponentTypeId kEntryB = 0xE1A2;
constexpr ComponentTypeId kOtherX = 0xE1B0;
constexpr ComponentTypeId kOtherY = 0xE1B1;
constexpr ComponentTypeId kOtherZ = 0xE1B2;

bool IsTestEntry(ComponentTypeId typeId)
{
    return typeId == kEntryA || typeId == kEntryB;
}

void RegisterTestHost()
{
    ed::EditorComponentTraits traits;
    traits.HostsInspectorSection = IsTestEntry;
    ed::EditorComponentTraitsRegistry::Get().Register(kHost, std::move(traits));
}
} // namespace

TEST(InspectorSectionHosts, HostMovesToItsFirstEntryAndNothingElseMoves)
{
    RegisterTestHost();
    std::vector<ComponentTypeId> sections{kOtherX, kEntryA, kOtherY, kEntryB, kHost, kOtherZ};

    ed::LeadWithSectionHosts(sections);

    EXPECT_EQ(sections, (std::vector<ComponentTypeId>{kOtherX, kHost, kEntryA, kOtherY, kEntryB, kOtherZ}));
}

TEST(InspectorSectionHosts, SectionsStayWhenTheHostLeadsOrIsAbsent)
{
    RegisterTestHost();
    const std::vector<ComponentTypeId> hostLeads{kHost, kOtherX, kEntryA, kEntryB};
    const std::vector<ComponentTypeId> hostAbsent{kOtherX, kEntryB, kOtherY, kEntryA};

    std::vector<ComponentTypeId> sections = hostLeads;
    ed::LeadWithSectionHosts(sections);
    EXPECT_EQ(sections, hostLeads);

    sections = hostAbsent;
    ed::LeadWithSectionHosts(sections);
    EXPECT_EQ(sections, hostAbsent);
}
