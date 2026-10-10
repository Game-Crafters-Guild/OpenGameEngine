#include "Assets/Parsers/UILayoutAssetParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine
{

namespace
{

// Find every `<attrName>="<value>"` occurrence in the body. The match is
// intentionally loose — it doesn't validate XML structure or element
// scope, and trades false positives (a `style="foo"` literal inside text
// content) for simplicity. False positives become path-form edges that
// don't resolve in the registry; they're benign noise.
//
// Required: the attribute name must be preceded by whitespace (or a tag
// open `<`) so we don't match e.g. `font-style="italic"` when looking
// for `style=`.
template <typename Emit>
void ScanAttribute(std::string_view body, std::string_view attrName, Emit emit)
{
    const std::string needle = std::string(attrName) + "=\"";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string_view::npos)
    {
        // Boundary check: char before `attrName` must be whitespace or '<'.
        if (pos > 0)
        {
            const char prev = body[pos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n' && prev != '<')
            {
                ++pos;
                continue;
            }
        }
        const size_t valueStart = pos + needle.size();
        const size_t valueEnd = body.find('"', valueStart);
        if (valueEnd == std::string_view::npos)
            return; // unterminated; bail
        emit(body.substr(valueStart, valueEnd - valueStart));
        pos = valueEnd + 1;
    }
}

} // namespace

bool UILayoutAssetParser::ExtractDependencies(const GUID& referrer,
                                              const AssetMetadata& metadata,
                                              DepEdgeSink& sink) const
{
    const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
    if (body.empty())
        return false;

    uint32_t styleOrdinal = 0;
    uint32_t layoutOrdinal = 0;

    ScanAttribute(body, "style", [&](std::string_view value)
    {
        if (value.empty())
            return;
        DepEdge e;
        e.Referrer = referrer;
        e.Kind = DepEdgeKind::UILayoutStyle;
        e.FieldLocator = "style[" + std::to_string(styleOrdinal) + "]";
        e.Ordinal = styleOrdinal++;
        if (ParserExtraction::LooksLikeGuid(value))
            e.Target = GUID(std::string(value).c_str());
        else
            e.TargetPath = ParserExtraction::NormalizeAuthoredPath(value);
        if (IsValidDepEdge(e))
            sink.Emit(std::move(e));
    });

    ScanAttribute(body, "layout", [&](std::string_view value)
    {
        if (value.empty())
            return;
        DepEdge e;
        e.Referrer = referrer;
        // No dedicated kind for sub-layout includes; Other is fine. The
        // FieldLocator distinguishes it from style refs for the retarget UX.
        e.Kind = DepEdgeKind::Other;
        e.FieldLocator = "layout[" + std::to_string(layoutOrdinal) + "]";
        e.Ordinal = layoutOrdinal++;
        if (ParserExtraction::LooksLikeGuid(value))
            e.Target = GUID(std::string(value).c_str());
        else
            e.TargetPath = ParserExtraction::NormalizeAuthoredPath(value);
        if (IsValidDepEdge(e))
            sink.Emit(std::move(e));
    });

    return true;
}

} // namespace GameEngine
