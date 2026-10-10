#include "Assets/Parsers/SceneAssetParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

constexpr std::string_view kNullGuid = "00000000-0000-0000-0000-000000000000";

std::string_view Trim(std::string_view s) noexcept
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
        --e;
    return s.substr(b, e - b);
}

// Strip surrounding double-quotes, if any.
std::string_view UnquoteIfPresent(std::string_view s) noexcept
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// Match `[entity id="..."]`. On success, returns the inner id; on failure,
// returns empty string_view.
std::string_view TryParseEntityHeader(std::string_view line)
{
    const std::string_view trimmed = Trim(line);
    if (trimmed.size() < 4 || trimmed.front() != '[' || trimmed.back() != ']')
        return {};
    constexpr std::string_view kPrefix = "entity";
    const std::string_view body = Trim(trimmed.substr(1, trimmed.size() - 2));
    if (body.size() < kPrefix.size() ||
        body.substr(0, kPrefix.size()) != kPrefix)
    {
        return {};
    }
    // Find id="..." attribute.
    const auto idEq = body.find("id=");
    if (idEq == std::string_view::npos)
        return {};
    std::string_view rest = body.substr(idEq + 3);
    if (rest.empty() || rest.front() != '"')
        return {};
    rest.remove_prefix(1);
    const auto closeQuote = rest.find('"');
    if (closeQuote == std::string_view::npos)
        return {};
    return rest.substr(0, closeQuote);
}

// Attribute lookup inside a bracketed AssetRef body: `key="value"`. Returns an
// empty view when the key is absent or its value is not double-quoted.
std::string_view FindBracketAttr(std::string_view body, std::string_view key)
{
    size_t i = 0;
    while ((i = body.find(key, i)) != std::string_view::npos)
    {
        const bool tokenStart = i == 0 || body[i - 1] == ' ' || body[i - 1] == '\t';
        size_t j = i + key.size();
        if (!tokenStart || j >= body.size() || body[j] != '=')
        {
            i = j;
            continue;
        }
        ++j;
        if (j >= body.size() || body[j] != '"')
        {
            i = j;
            continue;
        }
        const size_t start = j + 1;
        const size_t end = body.find('"', start);
        if (end == std::string_view::npos)
            return {};
        return body.substr(start, end - start);
    }
    return {};
}

// Match `[resource id="..." path="..." guid="..."]`. On success, returns the
// header body between the brackets; on failure, returns empty string_view.
std::string_view TryParseResourceHeader(std::string_view line)
{
    const std::string_view trimmed = Trim(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
        return {};
    constexpr std::string_view kPrefix = "resource";
    const std::string_view body = Trim(trimmed.substr(1, trimmed.size() - 2));
    if (body.size() <= kPrefix.size() || body.substr(0, kPrefix.size()) != kPrefix ||
        (body[kPrefix.size()] != ' ' && body[kPrefix.size()] != '\t'))
    {
        return {};
    }
    return body;
}

// The edges of one asset reference that carries a GUID, a path, or both, resolved as the
// runtime resolves it (ResolveResourcePath and TryResolveAssetReference in SceneIO.cpp):
// the GUID first, the path when the GUID is absent, null, or one the registry does not
// know. The parser cannot see the registry, and the registry caches the edges past this
// call, so a reference that carries both becomes two edges, the GUID's first. A live
// GUID and its path name one asset, which the walk visits once; a path no file is at
// resolves to no dependency. Neither, as in an empty `[]`, is no edge.
void EmitAssetRefEdges(const DepEdge& base, std::string_view guidStr, std::string_view pathStr,
                       uint32_t& ordinal, DepEdgeSink& sink)
{
    if (ParserExtraction::LooksLikeGuid(guidStr) && guidStr != kNullGuid)
    {
        DepEdge edge = base;
        edge.Target = GUID(std::string(guidStr).c_str());
        edge.Ordinal = ordinal++;
        if (IsValidDepEdge(edge))
            sink.Emit(std::move(edge));
    }
    if (!pathStr.empty())
    {
        DepEdge edge = base;
        edge.TargetPath = ParserExtraction::NormalizeAuthoredPath(pathStr);
        edge.Ordinal = ordinal++;
        if (IsValidDepEdge(edge))
            sink.Emit(std::move(edge));
    }
}

// The section a body line belongs to, for its field locator: "entities[<id>]" under an
// `[entity id=...]` header, "blueprints[<id>]" under a `[blueprint id=... source=...]`
// instance header, whose lines override the spawned blueprint's root. Empty under any
// other header ([scene], [resource], [subscene], ...), whose lines are not component
// fields. A section header is a trimmed line in brackets, as the scene reader reads it.
bool TryParseSectionLocator(std::string_view line, std::string& outLocator)
{
    const std::string_view trimmed = Trim(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
        return false;
    outLocator.clear();
    if (const std::string_view entityId = TryParseEntityHeader(line); !entityId.empty())
    {
        outLocator = "entities[" + std::string(entityId) + "]";
        return true;
    }
    constexpr std::string_view kBlueprint = "blueprint";
    const std::string_view body = Trim(trimmed.substr(1, trimmed.size() - 2));
    if (body.size() > kBlueprint.size() && body.substr(0, kBlueprint.size()) == kBlueprint &&
        (body[kBlueprint.size()] == ' ' || body[kBlueprint.size()] == '\t'))
    {
        const std::string_view id = FindBracketAttr(body, "id");
        if (!id.empty() && !FindBracketAttr(body, "source").empty())
            outLocator = "blueprints[" + std::string(id) + "]";
    }
    return true;
}

// Match `Component.field = value` (allows whitespace around `=`). Returns
// (locatorPart, valuePart) or empty pair if not a field-assignment line.
std::pair<std::string_view, std::string_view> TryParseFieldAssignment(std::string_view line)
{
    const std::string_view trimmed = Trim(line);
    // Skip section headers, comments, empty lines.
    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '[')
        return {};
    const auto eq = trimmed.find('=');
    if (eq == std::string_view::npos)
        return {};
    const std::string_view lhs = Trim(trimmed.substr(0, eq));
    std::string_view rhs = Trim(trimmed.substr(eq + 1));

    // Strip a trailing `; ...` inline comment, but only if the `;` is OUTSIDE
    // a quoted value. INI-style comments are well-defined as starting at
    // either column 0 or after at least one whitespace; the
    // outside-quotes-only rule catches both.
    bool inQuotes = false;
    for (size_t i = 0; i < rhs.size(); ++i)
    {
        const char c = rhs[i];
        if (c == '"')
            inQuotes = !inQuotes;
        else if (c == ';' && !inQuotes)
        {
            rhs = Trim(rhs.substr(0, i));
            break;
        }
    }

    // LHS must look like Component.field (one dot, both sides non-empty).
    const auto dot = lhs.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot == lhs.size() - 1)
        return {};
    return {lhs, rhs};
}

// A mark-up entity's components that are conversation or shape rather than scene
// content: the title, and the spline that is a path mark-up's shape. They are not
// editor-only on any other entity.
constexpr std::string_view kMarkupComponent = "Markup";
constexpr std::array<std::string_view, 2> kMarkupOwnedComponents = {"Name", "Spline"};

// The component a section line belongs to, as the scene reader reads it: `Component` of
// a `Component.field = value` line, and of a blueprint override's `-Component` removal,
// `+Component` addition or bare `Component` addition line. Empty for a header, a
// comment, a blank line, or anything else.
std::string_view LineComponent(std::string_view line)
{
    std::string_view trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '[')
        return {};
    if (trimmed.front() == '-' || trimmed.front() == '+')
        return Trim(trimmed.substr(1));
    if (trimmed.find('=') == std::string_view::npos && trimmed.find('.') == std::string_view::npos)
        return trimmed;
    const std::string_view lhs = TryParseFieldAssignment(line).first;
    return lhs.substr(0, lhs.find('.'));
}

bool IsMarkupOwnedComponent(std::string_view component)
{
    return std::any_of(kMarkupOwnedComponents.begin(), kMarkupOwnedComponents.end(),
                       [component](std::string_view owned) { return EqualsIgnoreCase(component, owned); });
}

// Appends the lines [first, last) of one section to `out`, without the lines a game
// export drops. Under an [entity] or [blueprint] instance header (`componentSection`)
// those are the editor-only components' lines and, when the section carries a
// `Markup` line, the mark-up's own Name and Spline lines; any other section is kept.
void AppendExportedSection(const std::vector<std::string_view>& lines, size_t first, size_t last,
                           bool componentSection,
                           const std::function<bool(std::string_view)>& isEditorOnly, std::string& out)
{
    bool carriesMarkup = false;
    if (componentSection)
    {
        for (size_t i = first; i < last && !carriesMarkup; ++i)
            carriesMarkup = EqualsIgnoreCase(LineComponent(lines[i]), kMarkupComponent);
    }

    for (size_t i = first; i < last; ++i)
    {
        if (componentSection)
        {
            const std::string_view component = LineComponent(lines[i]);
            if (!component.empty())
            {
                if ((carriesMarkup && IsMarkupOwnedComponent(component)) || isEditorOnly(component))
                    continue;
            }
        }
        out.append(lines[i]);
        if (i + 1 < lines.size())
            out.push_back('\n');
    }
}

} // namespace

bool SceneAssetParser::ExtractDependencies(const GUID& referrer,
                                           const AssetMetadata& metadata,
                                           DepEdgeSink& sink) const
{
    const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
    if (body.empty())
        return false;

    std::string currentSection; // "" outside [entity ...] and [blueprint ...] instance sections
    uint32_t ordinal = 0;

    std::string_view view(body);
    size_t lineStart = 0;
    while (lineStart <= view.size())
    {
        const size_t lineEnd = view.find('\n', lineStart);
        const std::string_view line = view.substr(
            lineStart,
            (lineEnd == std::string_view::npos ? view.size() : lineEnd) - lineStart);
        lineStart = (lineEnd == std::string_view::npos) ? view.size() + 1 : lineEnd + 1;

        // Resource headers are section headers, so the field walk below never sees them.
        if (const std::string_view resourceHeader = TryParseResourceHeader(line); !resourceHeader.empty())
        {
            currentSection.clear();
            DepEdge edge;
            edge.Referrer = referrer;
            edge.Kind = DepEdgeKind::SceneResource;
            edge.FieldLocator = "resources[" + std::string(FindBracketAttr(resourceHeader, "id")) + "]";
            EmitAssetRefEdges(edge, FindBracketAttr(resourceHeader, "guid"),
                              FindBracketAttr(resourceHeader, "path"), ordinal, sink);
            continue;
        }

        // Every section header ends the section before it. Field assignments only
        // matter inside [entity ...] and [blueprint ...] instance sections; the
        // [scene ...] header block and the other sections are skipped.
        if (TryParseSectionLocator(line, currentSection))
            continue;
        if (currentSection.empty())
            continue;

        const auto [lhs, rhs] = TryParseFieldAssignment(line);
        if (lhs.empty())
            continue;

        const std::string_view rawValue = UnquoteIfPresent(rhs);
        if (rawValue.empty())
            continue;

        DepEdge edge;
        edge.Referrer = referrer;
        edge.Kind = DepEdgeKind::SceneEntityComponent;
        edge.FieldLocator = currentSection + "." + std::string(lhs);

        if (ParserExtraction::LooksLikeGuid(rawValue))
        {
            // Drop null-GUID placeholders (very common in template scenes —
            // "00000000-0000-..." means "field is empty").
            if (rawValue == kNullGuid)
                continue;
            edge.Target = GUID(std::string(rawValue).c_str());
        }
        else if (rawValue.front() == '[' && rawValue.back() == ']')
        {
            // Canonical AssetRef form FormatAssetReferenceForSave writes:
            // `[path="Materials/foo.material" guid="..."]` (either attribute
            // may be absent). The GUID is the authoritative, rename-safe
            // reference; the path is what the load falls back to when the
            // GUID is unknown (EmitAssetRefEdges). Without this branch the
            // whole bracketed blob used to land as a garbage TargetPath and
            // the scene shipped with zero dependency edges. An empty AssetRef
            // `[]` is a cleared slot.
            const std::string_view refBody = rawValue.substr(1, rawValue.size() - 2);
            EmitAssetRefEdges(edge, FindBracketAttr(refBody, "guid"), FindBracketAttr(refBody, "path"), ordinal,
                              sink);
            continue;
        }
        else
        {
            // Heuristic: treat as a path only if it contains a path separator
            // or a known asset extension. Plain identifiers like primitive
            // names ("Plane", "Cube"), enum strings, numeric literals, vec
            // tuples, etc. are not asset refs and would generate noise.
            const bool hasSlash = rawValue.find('/') != std::string_view::npos ||
                                  rawValue.find('\\') != std::string_view::npos;
            if (!hasSlash)
                continue;
            // Normalize backslashes.
            std::string normalized(rawValue);
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            edge.TargetPath = std::move(normalized);
        }

        edge.Ordinal = ordinal++;
        if (IsValidDepEdge(edge))
            sink.Emit(std::move(edge));
    }

    return true;
}

void SceneAssetParser::CollectComponentNames(std::string_view body, std::unordered_set<std::string>& outNames)
{
    std::string currentSection; // "" outside [entity ...] and [blueprint ...] instance sections
    size_t lineStart = 0;
    while (lineStart <= body.size())
    {
        const size_t lineEnd = body.find('\n', lineStart);
        const std::string_view line = body.substr(
            lineStart, (lineEnd == std::string_view::npos ? body.size() : lineEnd) - lineStart);
        lineStart = (lineEnd == std::string_view::npos) ? body.size() + 1 : lineEnd + 1;

        if (TryParseSectionLocator(line, currentSection) || currentSection.empty())
            continue;
        const std::string_view lhs = TryParseFieldAssignment(line).first;
        if (!lhs.empty())
            outNames.emplace(lhs.substr(0, lhs.find('.')));
    }
}

std::string SceneAssetParser::StripEditorOnlyComponents(std::string_view body,
                                                        const std::function<bool(std::string_view)>& isEditorOnly)
{
    std::vector<std::string_view> lines;
    size_t lineStart = 0;
    while (lineStart <= body.size())
    {
        const size_t lineEnd = body.find('\n', lineStart);
        lines.push_back(body.substr(lineStart, (lineEnd == std::string_view::npos ? body.size() : lineEnd) - lineStart));
        lineStart = (lineEnd == std::string_view::npos) ? body.size() + 1 : lineEnd + 1;
    }

    std::string out;
    out.reserve(body.size());
    size_t sectionStart = 0;
    bool componentSection = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string nextSection;
        if (!TryParseSectionLocator(lines[i], nextSection))
            continue;
        AppendExportedSection(lines, sectionStart, i, componentSection, isEditorOnly, out);
        sectionStart = i;
        componentSection = !nextSection.empty();
    }
    AppendExportedSection(lines, sectionStart, lines.size(), componentSection, isEditorOnly, out);
    return out;
}

} // namespace GameEngine
