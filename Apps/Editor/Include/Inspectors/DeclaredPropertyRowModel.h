#pragma once

#include <array>
#include <string>

namespace GameEngine
{
struct MaterialDocument;

namespace Rendering
{
struct ShaderProperty;
struct ShaderPropertyTable;
}

namespace Editor
{

// What a declared-property row shows, and whether it shows at all. Split from
// the row widgets because it is pure string and value logic over the document
// and the surface's `// @property` table — the half that decides what the
// artist sees, and the half a test can hold still.

// The current value of a declared property: the document's override when the
// key is present (coerced to the declared width), otherwise the declared
// default. Returns true when the document authored it.
bool ReadDeclaredValue(const MaterialDocument& doc, const Rendering::ShaderProperty& property,
                       std::array<float, 4>& out);

// visibleIf=name | name=value | name!=value. `name` resolves against the
// declared properties first, then the document fields alphaMode and
// lightingModel. An unknown name shows the row rather than hiding an editable
// value, and an empty expression is no condition at all.
bool EvaluateVisibleIf(const std::string& expression, const Rendering::ShaderPropertyTable& table,
                       const MaterialDocument& doc);

} // namespace Editor
} // namespace GameEngine
