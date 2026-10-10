#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::ShaderGraph
{

struct SgParsedFile
{
    std::string TagBlock;
    std::string Body;
    bool IsGraphFile = false;
};

std::vector<std::pair<std::string, std::string>> ParseTagAttributes(const std::string& line);
std::optional<std::string> FindTagAttribute(const std::vector<std::pair<std::string, std::string>>& attrs,
                                            const std::string& key);

SgParsedFile ParseShaderGraphFile(const std::filesystem::path& path);
SgParsedFile ParseShaderGraphSource(const std::string& source);
bool IsShaderGraphSource(const std::string& source);

SgGraphDocument ParseGraphDocumentFromTags(const std::string& tagBlock);
std::string SerializeGraphDocumentTags(const SgGraphDocument& doc);

SgNodeDefinition ParseHelperNodeFromSource(const std::filesystem::path& path,
                                            const std::string& source,
                                            const std::string& typeId,
                                            const std::vector<std::pair<std::string, std::string>>& fileTags);

} // namespace GameEngine::ShaderGraph
