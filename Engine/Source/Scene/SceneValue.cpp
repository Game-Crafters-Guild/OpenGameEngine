#include "Scene/SceneValue.h"

#include "Types/FormatNumber.h"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace GameEngine::Scene
{
namespace
{
// Scene whitespace around a value token. LF belongs here as much as CR does: a value read from a
// CRLF file, or the last one before the terminator, arrives with its line ending attached.
static std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

static bool SplitCsvTuple(std::string_view inner, std::vector<std::string_view>& outParts)
{
    outParts.clear();
    std::string_view s = inner;
    while (true)
    {
        const size_t comma = s.find(',');
        if (comma == std::string_view::npos)
        {
            outParts.push_back(Trim(s));
            break;
        }
        outParts.push_back(Trim(s.substr(0, comma)));
        s = s.substr(comma + 1);
    }
    return !outParts.empty();
}

static bool IsIdentChar(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '-' || c == '.';
}

static bool SplitTopLevelList(std::string_view inner, std::vector<std::string_view>& outParts)
{
    outParts.clear();
    inner = Trim(inner);
    if (inner.empty())
        return true;

    bool inString = false;
    int depthParen = 0;
    int depthBracket = 0;
    size_t start = 0;

    for (size_t i = 0; i < inner.size(); ++i)
    {
        const char c = inner[i];
        if (c == '"' && (i == 0 || inner[i - 1] != '\\'))
        {
            inString = !inString;
            continue;
        }
        if (inString)
            continue;
        if (c == '(') { ++depthParen; continue; }
        if (c == ')') { --depthParen; continue; }
        if (c == '[') { ++depthBracket; continue; }
        if (c == ']') { --depthBracket; continue; }

        if (c == ',' && depthParen == 0 && depthBracket == 0)
        {
            outParts.push_back(Trim(inner.substr(start, i - start)));
            start = i + 1;
        }
    }
    outParts.push_back(Trim(inner.substr(start)));
    return true;
}
} // namespace

bool IsGuidText(std::string_view s)
{
    constexpr size_t kGuidTextLength = 36; // 32 hex digits + 4 dashes
    if (s.size() != kGuidTextLength)
        return false;
    for (size_t i = 0; i < kGuidTextLength; ++i)
    {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return false;
        }
        else if (!std::isxdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

template <class T>
IntegerTokenResult ParseIntegerToken(std::string_view text, T& value)
{
    text = Trim(text);
    // from_chars leaves '+' out of its grammar; scene decimal integers have always accepted one.
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
        // One sign only — from_chars would read the remainder of "+-1" as -1.
        if (!text.empty() && (text.front() == '+' || text.front() == '-'))
            return IntegerTokenResult::Malformed;
    }
    if (text.empty())
        return IntegerTokenResult::Malformed;

    T parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (error == std::errc::result_out_of_range)
        return IntegerTokenResult::OutOfRange;
    if (error != std::errc{} || end != text.data() + text.size())
        return IntegerTokenResult::Malformed;
    value = parsed;
    return IntegerTokenResult::Ok;
}

template IntegerTokenResult ParseIntegerToken<std::int8_t>(std::string_view, std::int8_t&);
template IntegerTokenResult ParseIntegerToken<std::int16_t>(std::string_view, std::int16_t&);
template IntegerTokenResult ParseIntegerToken<std::int32_t>(std::string_view, std::int32_t&);
template IntegerTokenResult ParseIntegerToken<std::int64_t>(std::string_view, std::int64_t&);
template IntegerTokenResult ParseIntegerToken<std::uint8_t>(std::string_view, std::uint8_t&);
template IntegerTokenResult ParseIntegerToken<std::uint16_t>(std::string_view, std::uint16_t&);
template IntegerTokenResult ParseIntegerToken<std::uint32_t>(std::string_view, std::uint32_t&);
template IntegerTokenResult ParseIntegerToken<std::uint64_t>(std::string_view, std::uint64_t&);

bool ParseQuotedString(std::string_view s, std::string& out)
{
    s = Trim(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return false;
    out.assign(s.substr(1, s.size() - 2));
    return true;
}

bool ParseFloat(std::string_view s, float& out)
{
    s = Trim(s);
    if (s.empty())
        return false;
    std::string tmp(s);
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end && end != tmp.c_str();
}

bool ParseFloat3(std::string_view s, Float3& out)
{
    s = Trim(s);
    if (s.size() < 5 || s.front() != '(' || s.back() != ')')
        return false;
    const std::string_view inner = s.substr(1, s.size() - 2);
    std::vector<std::string_view> parts;
    SplitCsvTuple(inner, parts);
    if (parts.size() != 3)
        return false;
    return ParseFloat(parts[0], out.X) && ParseFloat(parts[1], out.Y) && ParseFloat(parts[2], out.Z);
}

bool ParseFloat4(std::string_view s, Float4& out)
{
    s = Trim(s);
    if (s.size() < 7 || s.front() != '(' || s.back() != ')')
        return false;
    const std::string_view inner = s.substr(1, s.size() - 2);
    std::vector<std::string_view> parts;
    SplitCsvTuple(inner, parts);
    if (parts.size() != 4)
        return false;
    return ParseFloat(parts[0], out.X) && ParseFloat(parts[1], out.Y) && ParseFloat(parts[2], out.Z) && ParseFloat(parts[3], out.W);
}

bool ParseIdentifier(std::string_view s, std::string& out)
{
    s = Trim(s);
    if (s.empty())
        return false;
    for (char c : s)
    {
        if (!IsIdentChar(c))
            return false;
    }
    out.assign(s);
    return true;
}

static bool ParseInt64(std::string_view s, long long& out)
{
    std::int64_t v = 0;
    if (ParseIntegerToken(s, v) != IntegerTokenResult::Ok)
        return false;
    out = v;
    return true;
}

static bool ParseDouble(std::string_view s, double& out)
{
    s = Trim(s);
    if (s.empty())
        return false;
    std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (!end || end == tmp.c_str())
        return false;
    out = v;
    return true;
}

bool ParseValue(std::string_view s, SceneValue& out, std::string* outError)
{
    s = Trim(s);
    if (s.empty())
    {
        if (outError)
            *outError = "Empty value";
        return false;
    }

    // String
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    {
        out.Kind = SceneValueKind::String;
        if (!ParseQuotedString(s, out.StringValue))
        {
            if (outError)
                *outError = "Invalid quoted string";
            return false;
        }
        return true;
    }

    // Bool
    if (s == "true" || s == "false")
    {
        out.Kind = SceneValueKind::Bool;
        out.BoolValue = (s == "true");
        return true;
    }

    // Guid ref: &{...}
    if (s.size() >= 4 && s[0] == '&' && s[1] == '{' && s.back() == '}')
    {
        out.Kind = SceneValueKind::GuidRef;
        out.StringValue.assign(s.substr(2, s.size() - 3));
        return true;
    }

    // Bare GUID token: the sigil-free 8-4-4-4-12 form the debug server's set_component
    // hands a schema (it renders a JSON string as raw text). Must precede the number and
    // identifier fallbacks below, which would otherwise land every GUID shape as a bare
    // Identifier — something the schemas must then reject rather than resolve.
    if (IsGuidText(s))
    {
        out.Kind = SceneValueKind::GuidRef;
        out.StringValue.assign(s);
        return true;
    }

    // File ref: @"path"
    if (s.size() >= 2 && s[0] == '@')
    {
        if (s[1] != '"')
        {
            if (outError)
                *outError = "File ref must be @\"path\"";
            return false;
        }
        out.Kind = SceneValueKind::FileRef;
        std::string path;
        if (!ParseQuotedString(s.substr(1), path))
        {
            if (outError)
                *outError = "Invalid file ref";
            return false;
        }
        out.StringValue = std::move(path);
        return true;
    }

    // Resource ref: #id
    if (s.size() >= 2 && s[0] == '#')
    {
        out.Kind = SceneValueKind::ResourceRef;
        std::string id;
        if (!ParseIdentifier(s.substr(1), id))
        {
            if (outError)
                *outError = "Invalid resource ref";
            return false;
        }
        out.StringValue = std::move(id);
        return true;
    }

    // Entity ref: $id
    if (s.size() >= 2 && s[0] == '$')
    {
        out.Kind = SceneValueKind::EntityRef;
        std::string id;
        if (!ParseIdentifier(s.substr(1), id))
        {
            if (outError)
                *outError = "Invalid entity ref";
            return false;
        }
        out.StringValue = std::move(id);
        return true;
    }

    // Tuple: (a, b, ...)
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')')
    {
        out.Kind = SceneValueKind::Tuple;
        out.Items.clear();
        std::vector<std::string_view> parts;
        SplitTopLevelList(s.substr(1, s.size() - 2), parts);
        out.Items.reserve(parts.size());
        for (auto part : parts)
        {
            if (Trim(part).empty())
                continue;
            SceneValue elem{};
            std::string err;
            if (!ParseValue(part, elem, &err))
            {
                if (outError)
                    *outError = "Invalid tuple element: " + err;
                return false;
            }
            out.Items.push_back(std::move(elem));
        }
        return true;
    }

    // Bracket form — disambiguate AssetRef from Array.
    // AssetRef:  [path="..." guid="..."]   (whitespace-separated key="quoted" pairs)
    // Array:     [a, b, ...]               (comma-separated values)
    // Tell them apart by looking for `=` before any unquoted top-level `,`.
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
    {
        const std::string_view inner = Trim(s.substr(1, s.size() - 2));

        bool inString = false;
        bool sawAssign = false;
        bool sawComma = false;
        int depthParen = 0;
        int depthBracket = 0;
        for (size_t i = 0; i < inner.size(); ++i)
        {
            const char c = inner[i];
            if (c == '"' && (i == 0 || inner[i - 1] != '\\'))
            {
                inString = !inString;
                continue;
            }
            if (inString)
                continue;
            if (c == '(') { ++depthParen; continue; }
            if (c == ')') { --depthParen; continue; }
            if (c == '[') { ++depthBracket; continue; }
            if (c == ']') { --depthBracket; continue; }
            if (depthParen == 0 && depthBracket == 0)
            {
                if (c == '=') { sawAssign = true; break; }
                if (c == ',') { sawComma = true; break; }
            }
        }

        if (sawAssign && !sawComma)
        {
            // AssetRef: parse "key=\"value\"" pairs separated by whitespace.
            // Recognized keys: "path", "guid".  Unknown keys are an error.
            // Stored as items[0]=FileRef(path), items[1]=GuidRef(guid). Either
            // entry may be empty; at least one must be non-empty.
            out.Kind = SceneValueKind::AssetRef;
            out.Items.clear();
            out.Items.resize(2);
            out.Items[0].Kind = SceneValueKind::FileRef;
            out.Items[1].Kind = SceneValueKind::GuidRef;

            size_t i = 0;
            auto skipWs = [&]() { while (i < inner.size() && (inner[i] == ' ' || inner[i] == '\t')) ++i; };
            while (i < inner.size())
            {
                skipWs();
                if (i >= inner.size())
                    break;

                // key
                const size_t keyStart = i;
                while (i < inner.size() && IsIdentChar(inner[i]))
                    ++i;
                if (i == keyStart)
                {
                    if (outError)
                        *outError = "AssetRef: expected key (path or guid)";
                    return false;
                }
                const std::string_view key = inner.substr(keyStart, i - keyStart);

                skipWs();
                if (i >= inner.size() || inner[i] != '=')
                {
                    if (outError)
                        *outError = "AssetRef: expected '=' after key '" + std::string(key) + "'";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= inner.size() || inner[i] != '"')
                {
                    if (outError)
                        *outError = "AssetRef: value for '" + std::string(key) + "' must be a quoted string";
                    return false;
                }
                const size_t valStart = ++i;
                while (i < inner.size() && inner[i] != '"')
                    ++i;
                if (i >= inner.size())
                {
                    if (outError)
                        *outError = "AssetRef: unterminated quoted string for '" + std::string(key) + "'";
                    return false;
                }
                const std::string_view val = inner.substr(valStart, i - valStart);
                ++i; // past closing quote

                if (key == "path")
                    out.Items[0].StringValue.assign(val);
                else if (key == "guid")
                    out.Items[1].StringValue.assign(val);
                else
                {
                    if (outError)
                        *outError = "AssetRef: unknown key '" + std::string(key) + "' (expected path or guid)";
                    return false;
                }
            }

            if (out.Items[0].StringValue.empty() && out.Items[1].StringValue.empty())
            {
                if (outError)
                    *outError = "AssetRef: at least one of path or guid must be specified";
                return false;
            }
            return true;
        }

        // Array: [a, b, ...]
        out.Kind = SceneValueKind::Array;
        out.Items.clear();
        std::vector<std::string_view> parts;
        SplitTopLevelList(s.substr(1, s.size() - 2), parts);
        out.Items.reserve(parts.size());
        for (auto part : parts)
        {
            if (Trim(part).empty())
                continue;
            SceneValue elem{};
            std::string err;
            if (!ParseValue(part, elem, &err))
            {
                if (outError)
                    *outError = "Invalid array element: " + err;
                return false;
            }
            out.Items.push_back(std::move(elem));
        }
        return true;
    }

    // Number (int/float)
    {
        bool hasDotOrExp = false;
        for (char c : s)
        {
            if (c == '.' || c == 'e' || c == 'E')
            {
                hasDotOrExp = true;
                break;
            }
        }
        if (hasDotOrExp)
        {
            double v = 0.0;
            if (ParseDouble(s, v))
            {
                out.Kind = SceneValueKind::Float;
                out.FloatValue = v;
                return true;
            }
        }
        else
        {
            long long v = 0;
            if (ParseInt64(s, v))
            {
                out.Kind = SceneValueKind::Int;
                out.IntValue = v;
                return true;
            }
        }
    }

    // Identifier fallback
    {
        std::string id;
        if (ParseIdentifier(s, id))
        {
            out.Kind = SceneValueKind::Identifier;
            out.StringValue = std::move(id);
            return true;
        }
    }

    if (outError)
        *outError = "Unrecognized value syntax";
    return false;
}

std::string FormatQuoted(std::string_view s)
{
    // No escaping in v1.
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    out.append(s);
    out.push_back('"');
    return out;
}

std::string FormatFloat(float value)
{
    std::string out;
    AppendFloat(out, value);
    return out;
}

std::string FormatFloat2(float x, float y)
{
    std::string out;
    out.reserve(24);
    out.push_back('(');
    AppendFloat(out, x); out += ", ";
    AppendFloat(out, y);
    out.push_back(')');
    return out;
}

std::string FormatFloat3(float x, float y, float z)
{
    std::string out;
    out.reserve(32);
    out.push_back('(');
    AppendFloat(out, x); out += ", ";
    AppendFloat(out, y); out += ", ";
    AppendFloat(out, z);
    out.push_back(')');
    return out;
}

std::string FormatFloat4(float x, float y, float z, float w)
{
    std::string out;
    out.reserve(40);
    out.push_back('(');
    AppendFloat(out, x); out += ", ";
    AppendFloat(out, y); out += ", ";
    AppendFloat(out, z); out += ", ";
    AppendFloat(out, w);
    out.push_back(')');
    return out;
}

std::string_view AssetRefPath(const SceneValue& v)
{
    if (v.Kind != SceneValueKind::AssetRef || v.Items.size() < 1)
        return {};
    return v.Items[0].StringValue;
}

std::string_view AssetRefGuid(const SceneValue& v)
{
    if (v.Kind != SceneValueKind::AssetRef || v.Items.size() < 2)
        return {};
    return v.Items[1].StringValue;
}

std::string FormatAssetRef(std::string_view path, std::string_view guid)
{
    // Brackets + `path=""` + ` ` + `guid=""` = 22 chars; round to 24 for slack.
    constexpr size_t kAssetRefBracketOverhead = 24;
    std::string out;
    out.reserve(path.size() + guid.size() + kAssetRefBracketOverhead);
    out.push_back('[');
    bool wroteAny = false;
    if (!path.empty())
    {
        out += "path=\"";
        out += path;
        out += "\"";
        wroteAny = true;
    }
    if (!guid.empty())
    {
        if (wroteAny)
            out.push_back(' ');
        out += "guid=\"";
        out += guid;
        out += "\"";
    }
    out.push_back(']');
    return out;
}

} // namespace GameEngine::Scene

