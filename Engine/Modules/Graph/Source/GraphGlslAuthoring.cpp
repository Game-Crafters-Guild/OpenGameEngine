#include "Graph/GraphGlslAuthoring.h"

#include "Graph/SgGraphModelBridge.h"

#include "Rendering/ShaderGraph/SgTagParser.h"

#include <sstream>
#include <string>

namespace GameEngine {
namespace Graph {
namespace {

std::string TrimCopy(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string StripCommentPrefix(std::string_view line)
{
    const std::string trimmed = TrimCopy(line);
    if (trimmed.rfind("//", 0) != 0)
        return trimmed;
    std::string_view body = std::string_view(trimmed).substr(2);
    if (!body.empty() && body.front() == ' ')
        body.remove_prefix(1);
    return std::string(body);
}

} // namespace

std::optional<std::string> ExtractAuthoringJson(std::string_view comments)
{
    std::istringstream stream{std::string(comments)};
    std::string line;
    bool inFence = false;
    std::string json;
    while (std::getline(stream, line))
    {
        const std::string body = StripCommentPrefix(line);
        if (!inFence)
        {
            if (body == kGlslJsonFenceBegin)
                inFence = true;
            continue;
        }
        if (body == kGlslJsonFenceEnd)
            return json;
        json += body;
        json += '\n';
    }
    if (inFence)
        return std::string{};
    return std::nullopt;
}

std::string AppendAuthoringJsonFence(std::string tagBlock, const std::string& json)
{
    if (!tagBlock.empty() && tagBlock.back() != '\n')
        tagBlock += '\n';
    tagBlock += "// ";
    tagBlock += kGlslJsonFenceBegin;
    tagBlock += '\n';
    std::istringstream stream(json);
    std::string line;
    while (std::getline(stream, line))
    {
        tagBlock += "// ";
        tagBlock += line;
        tagBlock += '\n';
    }
    tagBlock += "// ";
    tagBlock += kGlslJsonFenceEnd;
    tagBlock += '\n';
    return tagBlock;
}

bool LoadModelFromShaderGraphComments(std::string_view tagBlock, Model& outModel)
{
    if (const std::optional<std::string> json = ExtractAuthoringJson(tagBlock))
    {
        if (!FromJson(*json, outModel))
            return false;
        outModel.KindId.assign(kKindIdMaterial);
        // The JSON fence is authoritative for the model, but unknown @sg-* tag
        // lines only live in the tag block; collect them so a save re-emits them.
        outModel.UnknownSgTags =
            ShaderGraph::ParseGraphDocumentFromTags(std::string(tagBlock)).UnknownTags;
        return true;
    }

    const ShaderGraph::SgGraphDocument document =
        ShaderGraph::ParseGraphDocumentFromTags(std::string(tagBlock));
    outModel = SgDocumentToGraphModel(document);
    outModel.KindId.assign(kKindIdMaterial);
    return true;
}

} // namespace Graph
} // namespace GameEngine
