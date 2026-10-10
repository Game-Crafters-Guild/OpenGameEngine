#include "Rendering/Materials/ShaderPropertyTable.h"

#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace GameEngine::Rendering
{

namespace
{

constexpr uint32_t kComponentsPerLane = 4;
constexpr uint32_t kBytesPerComponent = 4;

// The bare flag words the grammar accepts after the name.
constexpr std::array<const char*, 2> kFlagWords = {"hdr", "hidden"};

bool IsFlagWord(std::string_view token)
{
    return std::any_of(kFlagWords.begin(), kFlagWords.end(),
                       [&](const char* w) { return token == w; });
}

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool IsIdentifier(std::string_view s)
{
    if (s.empty() || !IsIdentStart(s.front()))
        return false;
    return std::all_of(s.begin(), s.end(), IsIdentChar);
}

bool HasPrefix(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Names the composed program defines as macros around the surface: the Props
// accessor, the Mat block alias, and the lane aliases of
// material_param_lanes.glsl. A GE_Props member spelled like one would be
// rewritten by the macro before glslang saw it.
bool IsComposedProgramName(std::string_view name)
{
    return name == "Props" || name == "Mat" || name == "uBaseColor" || HasPrefix(name, "uParams") ||
           HasPrefix(name, "uUser");
}

std::string Trim(std::string_view v)
{
    size_t b = 0;
    size_t e = v.size();
    while (b < e && std::isspace(static_cast<unsigned char>(v[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1])))
        --e;
    return std::string(v.substr(b, e - b));
}

bool IsQuoted(std::string_view s)
{
    return s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                             (s.front() == '\'' && s.back() == '\''));
}

std::string Unquote(std::string_view s)
{
    return IsQuoted(s) ? std::string(s.substr(1, s.size() - 2)) : std::string(s);
}

// "pulseSpeed" -> "Pulse Speed", "ao" -> "AO", "fresnelRimCap" -> "Fresnel Rim Cap".
std::string DeriveDisplayName(std::string_view name)
{
    if (name == "ao")
        return "AO";
    std::string out;
    out.reserve(name.size() + 4);
    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (c == '_')
        {
            out.push_back(' ');
            continue;
        }
        const bool boundary = i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
                              !std::isupper(static_cast<unsigned char>(name[i - 1]));
        if (boundary)
            out.push_back(' ');
        out.push_back(i == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    }
    return out;
}

bool ParseType(std::string_view token, ShaderPropertyType& out)
{
    static constexpr std::pair<const char*, ShaderPropertyType> kTypes[] = {
        {"float", ShaderPropertyType::Float},       {"vec2", ShaderPropertyType::Vec2},
        {"vec3", ShaderPropertyType::Vec3},         {"vec4", ShaderPropertyType::Vec4},
        {"color", ShaderPropertyType::Color},       {"bool", ShaderPropertyType::Bool},
        {"int", ShaderPropertyType::Int},           {"enum", ShaderPropertyType::Enum},
    };
    for (const auto& [name, type] : kTypes)
    {
        if (token == name)
        {
            out = type;
            return true;
        }
    }
    return false;
}

bool ParseFloat(std::string_view text, float& out)
{
    const std::string s = Trim(text);
    if (s.empty())
        return false;
    // std::from_chars for float is unavailable on macOS before 26.0 with the
    // Xcode 26 SDK, so parse with strtof. Trim already stripped whitespace,
    // and isfinite rejects the inf/nan spellings strtof accepts.
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

bool ParseInt(std::string_view text, int& out)
{
    const std::string s = Trim(text);
    if (s.empty())
        return false;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

std::vector<std::string> SplitCommas(std::string_view text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t comma = text.find(',', start);
        parts.push_back(Trim(text.substr(start, (comma == std::string::npos ? text.size() : comma) - start)));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return parts;
}

// The tokenizer runs a value to the next key=; a bare flag written after a
// value ("default=1,0.5,0.2 hdr") therefore rides along inside it. Peel such
// trailing flag words off and hand them back as flags.
std::string PeelTrailingFlags(std::string value, std::vector<std::string>& flags)
{
    for (;;)
    {
        const std::string trimmed = Trim(value);
        const size_t space = trimmed.find_last_of(" \t");
        if (space == std::string::npos)
            return trimmed;
        const std::string tail = trimmed.substr(space + 1);
        if (!IsFlagWord(tail))
            return trimmed;
        flags.push_back(tail);
        value = trimmed.substr(0, space);
    }
}

struct LineScan
{
    std::string Text;
    uint32_t Line = 0;
};

// Every `// @property` declaration line, with the tag as the first token after
// `//` (a mid-prose mention is not a declaration) and `@propertyX` excluded.
std::vector<LineScan> FindDeclarationLines(const std::string& source)
{
    std::vector<LineScan> out;
    std::istringstream iss(source);
    std::string line;
    uint32_t lineNo = 0;
    while (std::getline(iss, line))
    {
        ++lineNo;
        const size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos || line.compare(s, 2, "//") != 0)
            continue;
        size_t at = s + 2;
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
            ++at;
        static constexpr std::string_view kTag = "@property";
        if (line.compare(at, kTag.size(), kTag) != 0)
            continue;
        const size_t after = at + kTag.size();
        if (after < line.size() && line[after] != ' ' && line[after] != '\t')
            continue;
        out.push_back({line.substr(s), lineNo});
    }
    return out;
}

struct Parser
{
    const ShaderPropertySource& Source;
    ShaderPropertyTable& Table;

    void Error(uint32_t line, std::string message)
    {
        Table.Errors.push_back({Source.File, line, std::move(message)});
    }

    void ParseLine(const LineScan& scan)
    {
        const auto attrs = ShaderGraph::ParseTagAttributes(scan.Text);
        std::vector<std::string> positional;
        std::vector<std::pair<std::string, std::string>> keyed;
        for (const auto& [k, v] : attrs)
        {
            if (k == "kind")
                continue;
            if (HasPrefix(k, "arg"))
                positional.push_back(v);
            else
                keyed.emplace_back(k, v);
        }

        ShaderProperty prop{};
        prop.Origin = Source.Origin;
        prop.SourceFile = Source.File;
        prop.SourceLine = scan.Line;

        if (positional.size() < 2)
        {
            Error(scan.Line, "@property needs a type and a name: // @property <type> <name> "
                             "[\"Display Name\"] [default=...] [range=min,max] ...");
            return;
        }
        // Texture types are the grammar's future, not its present: name the tag
        // that declares textures today rather than reporting an unknown type.
        if (positional[0] == "texture2D" || positional[0] == "textureCube")
        {
            Error(scan.Line, "texture properties are declared with // @texture <name> [srgb|linear] "
                             "for now; '" + positional[1] + "' cannot be a @property yet");
            return;
        }
        if (!ParseType(positional[0], prop.Type))
        {
            Error(scan.Line, "unknown property type '" + positional[0] +
                                 "' (float, vec2, vec3, vec4, color, bool, int, enum)");
            return;
        }
        prop.Name = positional[1];
        if (!IsIdentifier(prop.Name))
        {
            Error(scan.Line, "property name '" + prop.Name + "' is not a valid GLSL identifier");
            return;
        }
        if (HasPrefix(prop.Name, "ge_") || HasPrefix(prop.Name, "GE_") || HasPrefix(prop.Name, "gl_"))
        {
            Error(scan.Line, "property name '" + prop.Name +
                                 "' uses a reserved prefix (ge_, GE_, gl_); pick another name");
            return;
        }
        if (IsComposedProgramName(prop.Name))
        {
            Error(scan.Line, "property name '" + prop.Name + "' is reserved: Props, Mat, uBaseColor and names "
                             "starting with uParams or uUser are the composed shader's own; pick another name");
            return;
        }

        std::vector<std::string> flags;
        for (size_t i = 2; i < positional.size(); ++i)
        {
            const std::string& tok = positional[i];
            if (IsQuoted(tok))
            {
                if (!prop.DisplayName.empty())
                {
                    Error(scan.Line, "property '" + prop.Name + "' has two display names");
                    return;
                }
                prop.DisplayName = Unquote(tok);
            }
            else if (IsFlagWord(tok))
            {
                flags.push_back(tok);
            }
            else
            {
                Error(scan.Line, "property '" + prop.Name + "': unexpected token '" + tok +
                                     "' (a display name must be quoted; attributes are key=value)");
                return;
            }
        }

        std::string defaultText;
        bool hasDefault = false;
        for (auto& [key, rawValue] : keyed)
        {
            // A quoted value is verbatim content (tooltip="keep it hidden" ends in
            // prose, not the hidden flag); only a bare value runs to the next key=
            // and can carry trailing flag words to peel.
            const std::string value = IsQuoted(rawValue) ? rawValue : PeelTrailingFlags(rawValue, flags);
            if (key == "default")
            {
                defaultText = Unquote(value);
                hasDefault = true;
            }
            else if (key == "range")
            {
                const auto parts = SplitCommas(Unquote(value));
                if (parts.size() != 2 || !ParseFloat(parts[0], prop.RangeMin) ||
                    !ParseFloat(parts[1], prop.RangeMax) || prop.RangeMin >= prop.RangeMax)
                {
                    Error(scan.Line, "property '" + prop.Name + "': range must be range=min,max with min < max");
                    return;
                }
                prop.HasRange = true;
            }
            else if (key == "group")
                prop.Group = Unquote(value);
            else if (key == "visibleIf")
                prop.VisibleIf = Unquote(value);
            else if (key == "keyword")
            {
                const std::string keyword = Unquote(value);
                Error(scan.Line, "property '" + prop.Name + "': keyword= is not wired yet; enable the define with "
                                 "\"keywords\": [\"" + keyword + "\"] in the .material and read it as #ifdef GE_USER_" +
                                     keyword);
                return;
            }
            else if (key == "tooltip")
                prop.Tooltip = Unquote(value);
            else if (key == "values")
            {
                prop.EnumValues = SplitCommas(Unquote(value));
                if (prop.EnumValues.empty() || std::any_of(prop.EnumValues.begin(), prop.EnumValues.end(),
                                                           [](const std::string& s) { return s.empty(); }))
                {
                    Error(scan.Line, "property '" + prop.Name + "': values= needs a comma-separated label list");
                    return;
                }
            }
            else
            {
                Error(scan.Line, "property '" + prop.Name + "': unknown attribute '" + key +
                                     "=' (default, range, group, visibleIf, tooltip, values)");
                return;
            }
        }

        for (const std::string& flag : flags)
        {
            if (flag == "hdr")
                prop.Hdr = true;
            else if (flag == "hidden")
                prop.Hidden = true;
        }

        if (prop.Hdr && prop.Type != ShaderPropertyType::Color)
        {
            Error(scan.Line, "property '" + prop.Name + "': hdr applies to color properties only");
            return;
        }
        if (prop.HasRange && prop.Type != ShaderPropertyType::Float && prop.Type != ShaderPropertyType::Int)
        {
            Error(scan.Line, "property '" + prop.Name + "': range= applies to float and int properties only");
            return;
        }
        if (prop.Type == ShaderPropertyType::Enum && prop.EnumValues.empty())
        {
            Error(scan.Line, "property '" + prop.Name + "': enum needs values=A,B,C");
            return;
        }
        if (!ParseDefault(prop, hasDefault, defaultText, scan.Line))
            return;
        if (prop.DisplayName.empty())
            prop.DisplayName = DeriveDisplayName(prop.Name);

        for (const ShaderProperty& existing : Table.Properties)
        {
            if (existing.Name == prop.Name)
            {
                Error(scan.Line, "property '" + prop.Name + "' is declared twice in this file (first at line " +
                                     std::to_string(existing.SourceLine) + ")");
                return;
            }
        }
        Table.Properties.push_back(std::move(prop));
    }

    bool ParseDefault(ShaderProperty& prop, bool hasDefault, const std::string& text, uint32_t line)
    {
        switch (prop.Type)
        {
        case ShaderPropertyType::Bool:
        {
            if (!hasDefault)
                return true;
            const std::string lower = Trim(text);
            if (lower == "true" || lower == "1")
                prop.Default[0] = 1.0f;
            else if (lower == "false" || lower == "0")
                prop.Default[0] = 0.0f;
            else
            {
                Error(line, "property '" + prop.Name + "': bool default must be true or false");
                return false;
            }
            return true;
        }
        case ShaderPropertyType::Int:
        {
            if (!hasDefault)
                return true;
            int v = 0;
            if (!ParseInt(text, v))
            {
                Error(line, "property '" + prop.Name + "': int default must be an integer");
                return false;
            }
            prop.Default[0] = static_cast<float>(v);
            return true;
        }
        case ShaderPropertyType::Enum:
        {
            if (!hasDefault)
                return true;
            const std::string label = Trim(text);
            const auto it = std::find(prop.EnumValues.begin(), prop.EnumValues.end(), label);
            if (it != prop.EnumValues.end())
            {
                prop.Default[0] = static_cast<float>(it - prop.EnumValues.begin());
                return true;
            }
            int v = 0;
            if (ParseInt(label, v) && v >= 0 && static_cast<size_t>(v) < prop.EnumValues.size())
            {
                prop.Default[0] = static_cast<float>(v);
                return true;
            }
            Error(line, "property '" + prop.Name + "': enum default '" + label + "' is not one of its values");
            return false;
        }
        case ShaderPropertyType::Color:
        {
            prop.Default = {1.0f, 1.0f, 1.0f, 1.0f};
            if (!hasDefault)
                return true;
            const auto parts = SplitCommas(text);
            if (parts.size() != 3 && parts.size() != 4)
            {
                Error(line, "property '" + prop.Name + "': color default needs 3 (rgb) or 4 (rgba) numbers");
                return false;
            }
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (!ParseFloat(parts[i], prop.Default[i]))
                {
                    Error(line, "property '" + prop.Name + "': color default component '" + parts[i] +
                                    "' is not a number");
                    return false;
                }
            }
            prop.HasAlpha = parts.size() == 4;
            return true;
        }
        case ShaderPropertyType::Float:
        case ShaderPropertyType::Vec2:
        case ShaderPropertyType::Vec3:
        case ShaderPropertyType::Vec4:
        {
            if (!hasDefault)
                return true;
            const uint32_t n = prop.ComponentCount();
            const auto parts = SplitCommas(text);
            if (parts.size() != n)
            {
                Error(line, "property '" + prop.Name + "': " + ShaderPropertyTypeName(prop.Type) +
                                " default needs " + std::to_string(n) + " number(s)");
                return false;
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!ParseFloat(parts[i], prop.Default[i]))
                {
                    Error(line, "property '" + prop.Name + "': default component '" + parts[i] +
                                    "' is not a number");
                    return false;
                }
            }
            return true;
        }
        }
        return true;
    }
};

const char* OriginName(ShaderPropertyOrigin origin)
{
    switch (origin)
    {
    case ShaderPropertyOrigin::Adapter: return "adapter";
    case ShaderPropertyOrigin::Surface: return "surface";
    case ShaderPropertyOrigin::VertexModifier: return "vertex modifier";
    }
    return "shader";
}

// Greedy first-fit over the lane grid. A vec4 needs a whole lane, a vec3 the
// first three components of a lane (its .w stays free for a scalar), a vec2 an
// 8-byte-aligned pair, a scalar any free component.
bool PlaceProperty(ShaderProperty& prop, std::array<std::array<bool, kComponentsPerLane>,
                                                    kMaterialParamLaneCount>& used)
{
    const uint32_t n = prop.ComponentCount();
    for (uint32_t lane = 0; lane < kMaterialParamLaneCount; ++lane)
    {
        for (uint32_t c = 0; c + n <= kComponentsPerLane; ++c)
        {
            if ((n == 4 || n == 3) && c != 0)
                break;
            if (n == 2 && (c % 2) != 0)
                continue;
            bool free = true;
            for (uint32_t k = 0; k < n; ++k)
                free = free && !used[lane][c + k];
            if (!free)
                continue;
            for (uint32_t k = 0; k < n; ++k)
                used[lane][c + k] = true;
            prop.HasLane = true;
            prop.Lane = lane;
            prop.Component = c;
            prop.ByteOffset = (lane * kComponentsPerLane + c) * kBytesPerComponent;
            prop.ByteSize = n * kBytesPerComponent;
            return true;
        }
    }
    return false;
}

} // namespace

const char* ShaderPropertyTypeName(ShaderPropertyType type)
{
    switch (type)
    {
    case ShaderPropertyType::Float: return "float";
    case ShaderPropertyType::Vec2: return "vec2";
    case ShaderPropertyType::Vec3: return "vec3";
    case ShaderPropertyType::Vec4: return "vec4";
    case ShaderPropertyType::Color: return "color";
    case ShaderPropertyType::Bool: return "bool";
    case ShaderPropertyType::Int: return "int";
    case ShaderPropertyType::Enum: return "enum";
    }
    return "?";
}

uint32_t ShaderProperty::ComponentCount() const
{
    switch (Type)
    {
    case ShaderPropertyType::Vec2: return 2;
    case ShaderPropertyType::Vec3: return 3;
    case ShaderPropertyType::Vec4: return 4;
    case ShaderPropertyType::Color: return HasAlpha ? 4 : 3;
    case ShaderPropertyType::Float:
    case ShaderPropertyType::Bool:
    case ShaderPropertyType::Int:
    case ShaderPropertyType::Enum: return 1;
    }
    return 0;
}

const char* ShaderProperty::GlslType() const
{
    switch (Type)
    {
    case ShaderPropertyType::Float: return "float";
    case ShaderPropertyType::Vec2: return "vec2";
    case ShaderPropertyType::Vec3: return "vec3";
    case ShaderPropertyType::Vec4: return "vec4";
    case ShaderPropertyType::Color: return HasAlpha ? "vec4" : "vec3";
    case ShaderPropertyType::Bool: return "bool";
    case ShaderPropertyType::Int:
    case ShaderPropertyType::Enum: return "int";
    }
    return "float";
}

std::string ShaderPropertyDiagnostic::Format(const char* severity) const
{
    return File + ":" + std::to_string(Line) + ": " + severity + ": " + Message;
}

const ShaderProperty* ShaderPropertyTable::Find(std::string_view name) const
{
    for (const ShaderProperty& p : Properties)
        if (p.Name == name)
            return &p;
    return nullptr;
}

ShaderPropertyTable ParseDeclaredProperties(const ShaderPropertySource& source)
{
    ShaderPropertyTable table{};
    Parser parser{source, table};
    for (const LineScan& scan : FindDeclarationLines(source.Text))
        parser.ParseLine(scan);
    return table;
}

ShaderPropertyTable BuildShaderPropertyTable(const std::vector<ShaderPropertySource>& sources)
{
    ShaderPropertyTable table{};
    std::vector<bool> declaredBySurface; // parallel to table.Properties

    for (const ShaderPropertySource& source : sources)
    {
        ShaderPropertyTable parsed = ParseDeclaredProperties(source);
        table.Errors.insert(table.Errors.end(), parsed.Errors.begin(), parsed.Errors.end());
        const bool fromSurface = source.Origin != ShaderPropertyOrigin::Adapter;
        for (ShaderProperty& prop : parsed.Properties)
        {
            if (fromSurface)
                table.HasSurfaceDeclarations = true;
            auto existing = std::find_if(table.Properties.begin(), table.Properties.end(),
                                         [&](const ShaderProperty& p) { return p.Name == prop.Name; });
            if (existing == table.Properties.end())
            {
                table.Properties.push_back(std::move(prop));
                declaredBySurface.push_back(fromSurface);
                continue;
            }
            if (existing->Type != prop.Type || existing->HasAlpha != prop.HasAlpha)
            {
                table.Errors.push_back(
                    {prop.SourceFile, prop.SourceLine,
                     "property '" + prop.Name + "' is declared as " + existing->GlslType() + " by the " +
                         OriginName(existing->Origin) + " (" + existing->SourceFile + ":" +
                         std::to_string(existing->SourceLine) + ") but as " + prop.GlslType() +
                         " here; the two must agree"});
                continue;
            }
            // Same name, same type: one slot at the first declaration's position;
            // the later (surface-side) declaration owns the metadata.
            const size_t index = static_cast<size_t>(existing - table.Properties.begin());
            *existing = std::move(prop);
            declaredBySurface[index] = declaredBySurface[index] || fromSurface;
        }
    }

    if (table.Rejected())
        return table;

    std::array<std::array<bool, kComponentsPerLane>, kMaterialParamLaneCount> used{};
    uint32_t floatsRequested = 0;
    for (size_t i = 0; i < table.Properties.size(); ++i)
    {
        ShaderProperty& prop = table.Properties[i];
        if (!declaredBySurface[i])
            continue; // an adapter read no producer stores: a compile-time constant, no lane
        floatsRequested += prop.ComponentCount();
        if (!PlaceProperty(prop, used))
        {
            const uint32_t totalFloats = kMaterialParamLaneCount * kComponentsPerLane;
            const std::string reason =
                floatsRequested > totalFloats
                    ? "the declared properties need more than the " + std::to_string(totalFloats) +
                          " floats the material parameter block holds (" + std::to_string(floatsRequested) +
                          " requested so far)"
                    : "the block still has floats free (" + std::to_string(floatsRequested) + " of " +
                          std::to_string(totalFloats) + " requested) but no lane placement is left for a " +
                          prop.GlslType() + " — a vec4 or vec3 must start a lane and a vec2 needs an "
                          "aligned pair; reorder declarations (wider types first) or drop one";
            table.Errors.push_back({prop.SourceFile, prop.SourceLine,
                                    "property '" + prop.Name + "' does not fit: " + reason});
            return table;
        }
        table.LanesUsed = std::max(table.LanesUsed, prop.Lane + 1);
    }
    return table;
}

} // namespace GameEngine::Rendering
