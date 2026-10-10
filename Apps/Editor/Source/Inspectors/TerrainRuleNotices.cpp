#include "Inspectors/TerrainRuleNotices.h"

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Terrain/TerrainRuleConditionVocabulary.h"
#include "UI/Controls/InspectorNotice.h"

#include <memory>
#include <string>

namespace GameEngine::Editor
{

void AddSurfaceRulesEmptyNotice(UIElement* parent, uint32 ruleCount)
{
    if (!parent || ruleCount > 0)
        return;

    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "No rules yet, so this effect writes nothing. Add Rule below, then give the rule a "
        "condition to say where it applies."));
}

void AddRuleRowStateNotice(UIElement* parent, const Components::TerrainSurfaceRule& rule,
                           uint32 ruleIndex)
{
    if (!parent)
        return;

    const std::string inert = TerrainRuleVocabulary::RuleInertReason(rule);
    if (!inert.empty())
    {
        parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(inert));
        return;
    }

    // Not a warning: an unconditional row is a legitimate authoring choice (a
    // base material under everything else is exactly this), it just covers more
    // than a reader of the row alone would expect.
    if (rule.ConditionCount == 0)
        InspectorUI::AddInfoCard(parent,
                                 std::string(TerrainRuleVocabulary::UnconditionalRuleNote()));

    (void)ruleIndex; // The row's own heading already numbers it.
}

std::string RuleCapReason(uint32 ruleCount)
{
    if (ruleCount < Components::kMaxTerrainSurfaceRules)
        return {};
    return "This effect holds the maximum of "
         + std::to_string(Components::kMaxTerrainSurfaceRules)
         + " rules. To add more, split the rule set across a second Surface Rules effect on "
           "another volume - they composite in stack order.";
}

std::string ConditionCapReason(uint32 ruleIndex, uint32 conditionCount)
{
    if (conditionCount < Components::kMaxTerrainRuleConditions)
        return {};
    return "Rule " + std::to_string(ruleIndex + 1) + " holds the maximum of "
         + std::to_string(Components::kMaxTerrainRuleConditions)
         + " conditions. To narrow it further, split it into a second rule targeting the same "
           "material - the two rules' weights add.";
}

void AddRuleCapNotice(UIElement* parent, uint32 ruleCount)
{
    const std::string reason = RuleCapReason(ruleCount);
    if (!parent || reason.empty())
        return;
    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(reason));
}

void AddConditionCapNotice(UIElement* parent, uint32 ruleIndex, uint32 conditionCount)
{
    const std::string reason = ConditionCapReason(ruleIndex, conditionCount);
    if (!parent || reason.empty())
        return;
    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(reason));
}

} // namespace GameEngine::Editor
