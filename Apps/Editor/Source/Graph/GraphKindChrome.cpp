#include "Graph/GraphKindChrome.h"

#include "Graph/GraphNodeRegistry.h"

#include <algorithm>
#include <cctype>

namespace GameEngine {
namespace {

std::string LowerAscii(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string EnsureLeadingDot(std::string_view ext)
{
    if (ext.empty())
        return ".graph";
    if (ext.front() == '.')
        return std::string(ext);
    return "." + std::string(ext);
}

} // namespace

std::vector<Graph::GraphTypeDesc> RegisteredGraphTypes()
{
    (void)GraphNodeRegistry::Get();
    return Graph::GraphTypeRegistry::Get().All();
}

GraphKindFileFilter SaveFilterForKind(std::string_view kindId)
{
    (void)GraphNodeRegistry::Get();
    if (const Graph::GraphTypeDesc* desc = Graph::GraphTypeRegistry::Get().Find(kindId))
    {
        const std::string ext = EnsureLeadingDot(desc->FileExtension);
        return {desc->DisplayName, "*" + ext, ext};
    }
    return {"Graph", "*.graph", ".graph"};
}

std::string UntitledGraphTitle(std::string_view kindId)
{
    (void)GraphNodeRegistry::Get();
    if (const Graph::GraphTypeDesc* desc = Graph::GraphTypeRegistry::Get().Find(kindId))
        return "Untitled " + desc->DisplayName;
    return "Untitled Graph";
}

std::string OpenGraphsFilterName()
{
    return "Graphs";
}

std::string OpenGraphsFilterPattern()
{
    (void)GraphNodeRegistry::Get();
    std::vector<std::string> patterns;
    for (const Graph::GraphTypeDesc& desc : Graph::GraphTypeRegistry::Get().All())
    {
        const std::string pattern = "*" + EnsureLeadingDot(desc.FileExtension);
        if (std::find(patterns.begin(), patterns.end(), pattern) == patterns.end())
            patterns.push_back(pattern);
    }
    std::string joined;
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        if (i > 0)
            joined += ';';
        joined += patterns[i];
    }
    return joined;
}

bool ExtensionOpensInGraphPanel(std::string_view extension)
{
    return !KindIdFromGraphExtension(extension).empty();
}

std::string KindIdFromGraphExtension(std::string_view extension)
{
    const std::string ext = LowerAscii(extension);
    (void)GraphNodeRegistry::Get();
    for (const Graph::GraphTypeDesc& desc : Graph::GraphTypeRegistry::Get().All())
    {
        if (LowerAscii(EnsureLeadingDot(desc.FileExtension)) == ext)
            return desc.Id;
    }
    return {};
}

} // namespace GameEngine
