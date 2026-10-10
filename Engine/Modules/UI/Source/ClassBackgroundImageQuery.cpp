#include "UI/ClassBackgroundImageQuery.h"

#include "UI/Controls/Button.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"

#include <vector>

namespace GameEngine {
namespace UIStyleQuery {

std::string ResolveClassBackgroundImageUrl(std::span<const StylesheetHandle> sheets,
                                           std::string_view className)
{
    if (className.empty())
        return {};

    std::vector<const Stylesheet*> cascade;
    cascade.reserve(sheets.size());
    for (const StylesheetHandle& sheet : sheets)
    {
        if (sheet)
            cascade.push_back(sheet.get());
    }
    if (cascade.empty())
        return {};

    Button probe;
    probe.AddClass(std::string(className));

    const UIParsing::ElementState state;
    const ResolvedStyle style = UIParsing::CSSParser::ComputeStyleFor(probe, cascade, state);

    const BackgroundImageSource& source = style.Visual.BackgroundImage.Source;
    if (source.Kind != BackgroundImageSource::SourceKind::Path || source.Value.empty())
        return {};
    if (source.SourceAlias.empty())
        return source.Value;
    return source.SourceAlias + ":" + source.Value;
}

} // namespace UIStyleQuery
} // namespace GameEngine
