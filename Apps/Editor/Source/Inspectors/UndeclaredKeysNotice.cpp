#include "Inspectors/UndeclaredKeysNotice.h"

#include "Inspectors/MaterialInspectorSections.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Types/NearestName.h"

#include <algorithm>
#include <sstream>

namespace GameEngine::Editor
{
namespace
{

// "user0".."user15" / "userVec0".."userVec3" — the generic lanes, and only
// those: a name like "userScale" is a typo the notice should still report.
bool IsGenericLaneKey(const std::string& key)
{
    if (key.rfind("user", 0) != 0)
        return false;
    const Rendering::LegacyMaterialLane* lane = Rendering::FindLegacyMaterialLane(key);
    return lane != nullptr && lane->Lane >= Rendering::kGenericLaneFirst;
}

} // namespace


std::vector<std::string> UndeclaredKeyLines(const std::vector<std::string>& keys,
                                            const std::vector<std::string>& declaredNames,
                                            const Rendering::ShaderPropertyTable* table)
{
    const MaterialDocument seeded = MaterialDocument::CreateDefaultPBR("");
    std::vector<std::string> lines;
    for (const std::string& key : keys)
    {
        if (key == "opacity" || seeded.properties.count(key) != 0)
            continue;
        // On a surface with no declarations the generic-lane keys DO reach the
        // shader: MaterialRegistry wires user0..15 / userVec0..3 to the four
        // lanes such a surface reads as Mat.uUser*, which is what the graph
        // editor's live preview and the converted-surface corpus author. They
        // have no inspector row, which is a different complaint from the one
        // this notice makes. A declared surface replaces that whole wiring, so
        // there the keys really are dead and the line below stands.
        if (!table && IsGenericLaneKey(key))
            continue;
        const Rendering::ShaderProperty* entry = table ? table->Find(key) : nullptr;
        if (entry && !entry->HasLane)
        {
            lines.push_back(key + " — an adapter constant; add // @property " +
                            Rendering::ShaderPropertyTypeName(entry->Type) + " " + key +
                            " ... to the surface to author it");
        }
        else
        {
            lines.push_back(key + NearestNameSuffix(key, declaredNames));
        }
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

void AddUndeclaredKeysNotice(UIElement* content, const std::vector<std::string>& keys,
                             const std::vector<std::string>& declaredNames,
                             const Rendering::ShaderPropertyTable* table)
{
    const std::vector<std::string> lines = UndeclaredKeyLines(keys, declaredNames, table);
    if (lines.empty())
        return;
    std::ostringstream oss;
    oss << "Not declared by the surface (stored, but never reaches the shader):\n";
    for (const std::string& line : lines)
        oss << " - " << line << "\n";
    oss << "Remove the key, or add a // @property line for it.";
    AddMaterialLine(content, oss.str());
}

} // namespace GameEngine::Editor
