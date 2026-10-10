#include "Inspectors/DeclaredPropertyRowModel.h"

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace GameEngine::Editor
{

// The current value of a declared property: the document's override when the
// key is present (coerced to the declared width), otherwise the declared
// default. Returns true when the document authored it.
bool ReadDeclaredValue(const MaterialDocument& doc, const Rendering::ShaderProperty& p,
                       std::array<float, 4>& out)
{
    out = p.Default;
    auto it = doc.properties.find(p.Name);
    if (it == doc.properties.end())
        return false;
    const MaterialValue& v = it->second;
    if (const float* f = std::get_if<float>(&v))
        out[0] = *f;
    else if (const int32_t* i = std::get_if<int32_t>(&v))
        out[0] = static_cast<float>(*i);
    else if (const bool* b = std::get_if<bool>(&v))
        out[0] = *b ? 1.0f : 0.0f;
    else if (const std::vector<float>* arr = std::get_if<std::vector<float>>(&v))
    {
        const size_t n = std::min<size_t>(arr->size(), p.ComponentCount());
        std::copy(arr->begin(), arr->begin() + static_cast<std::ptrdiff_t>(n), out.begin());
    }
    return true;
}

// visibleIf=name | name=value | name!=value. `name` resolves against the declared
// properties first, then the document fields alphaMode and lightingModel. An
// unknown name shows the row rather than hiding an editable value.
bool EvaluateVisibleIf(const std::string& expr, const Rendering::ShaderPropertyTable& table,
                       const MaterialDocument& doc)
{
    if (expr.empty())
        return true;
    std::string name = expr;
    std::string expected;
    bool negate = false;
    if (const size_t ne = expr.find("!="); ne != std::string::npos)
    {
        name = expr.substr(0, ne);
        expected = expr.substr(ne + 2);
        negate = true;
    }
    else if (const size_t eq = expr.find('='); eq != std::string::npos)
    {
        name = expr.substr(0, eq);
        expected = expr.substr(eq + 1);
    }

    std::string current;
    bool truthy = false;
    if (const Rendering::ShaderProperty* p = table.Find(name))
    {
        std::array<float, 4> v{};
        ReadDeclaredValue(doc, *p, v);
        truthy = v[0] != 0.0f;
        if (p->Type == Rendering::ShaderPropertyType::Enum)
        {
            const int index = static_cast<int>(v[0]);
            if (index >= 0 && static_cast<size_t>(index) < p->EnumValues.size())
                current = p->EnumValues[static_cast<size_t>(index)];
        }
        else if (p->Type == Rendering::ShaderPropertyType::Bool)
            current = truthy ? "true" : "false";
        else
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v[0]));
            current = buf;
        }
    }
    else if (name == "alphaMode")
    {
        current = MaterialAlphaModeToString(doc.alphaMode);
        truthy = true;
    }
    else if (name == "lightingModel")
    {
        current = doc.lightingModel;
        truthy = true;
    }
    else
        return true;

    if (expected.empty())
        return truthy;
    const bool equal = ToLowerAscii(current) == ToLowerAscii(expected);
    return negate ? !equal : equal;
}

} // namespace GameEngine::Editor
