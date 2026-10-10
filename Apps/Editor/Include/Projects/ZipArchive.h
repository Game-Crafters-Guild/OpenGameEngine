#pragma once

#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

// Extract a .zip archive into destination. When innerRoot is non-empty, only
// entries under that archive-relative folder are extracted, with the prefix
// stripped (a manifest zip source's "path"). Entry names are validated:
// traversal ("..") and absolute names are rejected so a hostile archive can
// never write outside destination. Returns false with outError on failure.
bool ExtractZipArchive(const std::filesystem::path& zipFile,
                       const std::filesystem::path& destination,
                       const std::string& innerRoot,
                       std::string* outError);

} // namespace GameEngine::Editor
