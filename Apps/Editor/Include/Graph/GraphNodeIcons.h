#pragma once

#include <string>
#include <string_view>

namespace GameEngine {

class UIElement;

/**
 * Kind-agnostic icon mechanism for graph nodes. Stems come from each kind's
 * registration site (NodeTypeMeta::IconStem, GraphNodeRegistry::CategoryIconStem);
 * this namespace only turns a stem into a path or a CSS class.
 */
namespace GraphNodeIcons {

/** "editor:Icons/GraphNodes/<stem>.svg" (empty stem = empty string). Native
    context menus only — UIElements take the CSS class below instead. */
std::string IconPathForStem(std::string_view stem);

/** Swaps the element's "gn-icon--<stem>" class (elements are pooled and
    rebound, so any previous stem class is removed first; empty stem clears). */
void ApplyIconClass(UIElement& element, std::string_view stem);

} // namespace GraphNodeIcons

} // namespace GameEngine
