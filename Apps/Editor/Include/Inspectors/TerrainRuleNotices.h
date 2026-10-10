#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine
{
class UIElement;
namespace Components
{
struct TerrainSurfaceRule;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// The Inspector notices a Surface Rules effect needs, kept out of the row
// builder so their wording is reachable from a test — the same reason
// TerrainVolumeNotices and AddGeneratedEntityNotice exist as their own TU.
//
// Every one of these is a state that makes a control stop meaning what it
// implies, and the standing rule from the Shape::Global review applies: a row
// that is ignored says so AT THE ROW. A summary at the top of the effect is not
// a substitute, because at a real inspector width the row the author is dragging
// is several hundred pixels away from it.

// The effect writes nothing because it has no rows. Appended under the effect
// header, where the Add Rule button that fixes it also lives.
void AddSurfaceRulesEmptyNotice(UIElement* parent, uint32 ruleCount);

// This row's own state, rendered at the row: why it is ignored, or — for a row
// with no conditions — what "unconditional" costs. Silent for a row that writes
// under conditions, which is the ordinary case.
void AddRuleRowStateNotice(UIElement* parent, const Components::TerrainSurfaceRule& rule,
                           uint32 ruleIndex);

// Why Add Rule / Add Condition is inert at the cap. Returned as strings because
// the SAME sentence has to reach two places — the disabled button's tooltip and
// the notice beside it — and a cap explained one way on the control and another
// way underneath it is two answers to one question.
//
// Empty when not at the cap, so a caller can pass the result straight through as
// "disabled reason or nothing".
std::string RuleCapReason(uint32 ruleCount);
std::string ConditionCapReason(uint32 ruleIndex, uint32 conditionCount);

// The rule set is full. Named at the Add Rule affordance, with the same way out
// the scene loader's over-cap error gives, so the two answers agree.
void AddRuleCapNotice(UIElement* parent, uint32 ruleCount);

// This row's conditions are full. Named at that row's Add Condition affordance —
// the cap is per rule, so a set with one full row and seven empty ones must not
// read as though the whole effect is full.
void AddConditionCapNotice(UIElement* parent, uint32 ruleIndex, uint32 conditionCount);

} // namespace GameEngine::Editor
