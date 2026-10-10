#pragma once

#include <string>
#include <vector>

namespace GameEngine
{
class UIElement;

namespace Rendering
{
struct ShaderPropertyTable;
}

namespace Editor
{

// One line per document key that never reaches the shader, ready for the
// material inspector's notice:
//  - a key matching no declaration gets the nearest declared name,
//  - a key matching a laneless declaration (an adapter read no surface stores —
//    the composer folded it to its default) gets the declare-it fix-it.
// Keys the transitional StandardPBR parse-time fill seeds into every document
// are exempt — reporting them would flag every material. Pure function of its
// inputs so the wording is testable without UI; pass no table when the surface
// has no declared-property table (the legacy project-surface path).
std::vector<std::string> UndeclaredKeyLines(const std::vector<std::string>& keys,
                                            const std::vector<std::string>& declaredNames,
                                            const Rendering::ShaderPropertyTable* table);

// Appends the "Not declared by the surface" notice, or nothing when no key
// remains after the exemptions.
void AddUndeclaredKeysNotice(UIElement* content, const std::vector<std::string>& keys,
                             const std::vector<std::string>& declaredNames,
                             const Rendering::ShaderPropertyTable* table);

} // namespace Editor
} // namespace GameEngine
