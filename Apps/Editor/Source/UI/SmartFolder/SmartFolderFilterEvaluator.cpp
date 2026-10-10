#include "UI/SmartFolder/SmartFolderFilterEvaluator.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include <regex>
#include <algorithm>
#include <cctype>

namespace GameEngine {

namespace {

std::string NormalizePathForCompare(const std::filesystem::path& p)
{
    // Avoid filesystem hits (canonical/weakly_canonical). SmartFolder evaluation can touch a lot
    // of assets and must stay fast; lexically_normal is purely string-based.
    std::string s = p.lexically_normal().generic_string(); // '/' separators
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}

bool HasPathPrefix(const std::string& path, const std::string& prefix)
{
    if (prefix.empty())
        return true;
    if (path.size() < prefix.size())
        return false;
    if (path.compare(0, prefix.size(), prefix) != 0)
        return false;
    // Require directory boundary: exact match, or next char is '/'.
    if (path.size() == prefix.size())
        return true;
    return path[prefix.size()] == '/';
}

VCSFileStatus ParseVCSStatusString(const std::string& s)
{
    if (s == "Modified") return VCSFileStatus::Modified;
    if (s == "Added") return VCSFileStatus::Added;
    if (s == "Deleted") return VCSFileStatus::Deleted;
    if (s == "Unversioned") return VCSFileStatus::Unversioned;
    if (s == "Conflict") return VCSFileStatus::Conflict;
    if (s == "LockedByMe") return VCSFileStatus::LockedByMe;
    if (s == "LockedByOthers") return VCSFileStatus::LockedByOthers;
    if (s == "ServerHasChanges") return VCSFileStatus::ServerHasChanges;
    if (s == "Ignored") return VCSFileStatus::Ignored;
    if (s == "Clean") return VCSFileStatus::Clean;
    return VCSFileStatus::NotConfigured;
}

} // anonymous namespace

std::vector<std::filesystem::path> SmartFolderFilterEvaluator::Evaluate(
    const SmartFolder& folder,
    const std::filesystem::path& assetsRoot,
    AssetRegistry* registry) const
{
    std::vector<std::filesystem::path> result;
    
    // Determine the search root
    std::filesystem::path searchRoot = assetsRoot;
    if (!folder.DirectoryScope.empty()) {
        // If directoryScope is relative, make it absolute
        if (folder.DirectoryScope.is_relative()) {
            searchRoot = assetsRoot / folder.DirectoryScope;
        } else {
            searchRoot = folder.DirectoryScope;
        }
    }
    
    // Collect all files from AssetRegistry
    std::vector<std::filesystem::path> allFiles;
    
    if (registry) {
        CollectFilesFromRegistry(registry, folder, searchRoot, assetsRoot, allFiles);
    }
    
    // Fallback to filesystem scanning when registry lookup yields no candidates.
    // This keeps smart folders working when registry/source mappings are temporarily
    // stale or when paths are outside currently indexed roots.
    if (allFiles.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(searchRoot, ec) && std::filesystem::is_directory(searchRoot, ec)) {
            CollectFilesFromFilesystem(searchRoot, allFiles);
        }
    }
    
    // Filter files
    struct PreparedFilter
    {
        SmartFolderFilter::Type type{};
        std::string valueLower;
        std::string fileTypeExtLower; // includes leading '.'
        std::optional<std::regex> regex;
        bool regexValid = false;
        VCSFileStatus vcsStatus = VCSFileStatus::NotConfigured;
    };

    // Preserve existing semantics: an empty-valued filter matches everything.
    bool hasAlwaysTrueFilter = false;
    std::vector<PreparedFilter> prepared;
    prepared.reserve(folder.Filters.size());
    for (const auto& f : folder.Filters)
    {
        if (f.Value.empty())
        {
            hasAlwaysTrueFilter = true;
            continue;
        }
        PreparedFilter pf{};
        pf.type = f.FilterType;
        pf.valueLower = ToLowerAscii(f.Value);
        if (f.FilterType == SmartFolderFilter::Type::FileType)
        {
            pf.fileTypeExtLower = pf.valueLower;
            if (!pf.fileTypeExtLower.empty() && pf.fileTypeExtLower[0] != '.')
                pf.fileTypeExtLower = "." + pf.fileTypeExtLower;
        }
        else if (f.FilterType == SmartFolderFilter::Type::Regex)
        {
            try
            {
                pf.regex.emplace(f.Value, std::regex::ECMAScript | std::regex::icase);
                pf.regexValid = true;
            }
            catch (const std::regex_error&)
            {
                pf.regexValid = false;
            }
        }
        else if (f.FilterType == SmartFolderFilter::Type::VCSStatus)
        {
            pf.vcsStatus = ParseVCSStatusString(f.Value);
        }
        prepared.emplace_back(std::move(pf));
    }

    IVCSIntegration* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();

    auto matchesPrepared = [&](const std::filesystem::path& file) -> bool
    {
        if (folder.Filters.empty())
            return true; // No filters = match everything

        // Empty filters match everything; for OR-mode that means "match everything".
        if (folder.CombineMode == FilterCombineMode::Or && hasAlwaysTrueFilter)
            return true;

        const std::string filename = file.filename().string();
        const std::string filenameLower = ToLowerAscii(filename);
        std::string extLower;
        if (file.has_extension())
        {
            extLower = ToLowerAscii(file.extension().string());
        }

        auto matchesOne = [&](const PreparedFilter& pf) -> bool
        {
            switch (pf.type)
            {
                case SmartFolderFilter::Type::FileType:
                    if (extLower.empty())
                        return false;
                    return extLower == pf.fileTypeExtLower;
                case SmartFolderFilter::Type::NameContains:
                    return filenameLower.find(pf.valueLower) != std::string::npos;
                case SmartFolderFilter::Type::Regex:
                    if (!pf.regexValid || !pf.regex.has_value())
                        return false;
                    return std::regex_search(filename, *pf.regex);
                case SmartFolderFilter::Type::Tag:
                {
                    if (!registry)
                        return false;
                    std::string tagMeta;
                    if (!registry->TryGetMetaValue(file, "tags", tagMeta) || tagMeta.empty())
                        return false;
                    for (size_t i = 0; i < tagMeta.size(); )
                    {
                        size_t j = tagMeta.find(',', i);
                        if (j == std::string::npos)
                            j = tagMeta.size();
                        std::string part = tagMeta.substr(i, j - i);
                        const size_t s = part.find_first_not_of(" \t\r\n");
                        if (s != std::string::npos)
                        {
                            const size_t e = part.find_last_not_of(" \t\r\n");
                            part = part.substr(s, e == std::string::npos ? part.size() - s : e - s + 1);
                        }
                        else
                            part.clear();
                        if (!part.empty() && ToLowerAscii(part) == pf.valueLower)
                            return true;
                        i = j + (j < tagMeta.size() ? 1 : 0);
                    }
                    return false;
                }
                case SmartFolderFilter::Type::VCSStatus:
                {
                    if (!vcs)
                        return false;
                    return vcs->GetFileStatus(file) == pf.vcsStatus;
                }
            }
            return false;
        };

        if (folder.CombineMode == FilterCombineMode::And)
        {
            for (const auto& pf : prepared)
            {
                if (!matchesOne(pf))
                    return false;
            }
            // AND-mode with only empty filters should match everything.
            return true;
        }

        // OR-mode: any filter must match; if we only had empty filters, we already returned true above.
        for (const auto& pf : prepared)
        {
            if (matchesOne(pf))
                return true;
        }
        return false;
    };

    for (const auto& file : allFiles)
    {
        if (matchesPrepared(file))
        {
            result.push_back(file);
        }
    }
    
    // Sort by filename for consistent ordering
    std::sort(result.begin(), result.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename().string() < b.filename().string();
        });
    
    return result;
}

bool SmartFolderFilterEvaluator::Matches(const std::filesystem::path& file, const SmartFolder& folder) const
{
    // Note: Evaluate() uses a prepared (cached) filter path; keep this method for API
    // compatibility and correctness, but it does not cache regex compilation.
    if (folder.Filters.empty())
        return true; // No filters = match everything

    if (folder.CombineMode == FilterCombineMode::And)
    {
        for (const auto& filter : folder.Filters)
        {
            if (!MatchesFilter(file, filter))
            {
                return false;
            }
        }
        return true;
    }

    for (const auto& filter : folder.Filters)
    {
        if (MatchesFilter(file, filter))
        {
            return true;
        }
    }
    return false;
}

bool SmartFolderFilterEvaluator::MatchesFilter(const std::filesystem::path& file, const SmartFolderFilter& filter) const
{
    if (filter.Value.empty()) {
        return true; // Empty filter matches everything
    }

    const std::string filename = file.filename().string();
    const std::string filenameLower = ToLowerAscii(filename);
    const std::string valueLower = ToLowerAscii(filter.Value);

    switch (filter.FilterType) {
        case SmartFolderFilter::Type::FileType: {
            // Match file extension
            if (!file.has_extension()) {
                return false;
            }
            std::string ext = file.extension().string();
            std::string extLower = ToLowerAscii(ext);
            
            // Handle both ".png" and "png" formats
            std::string filterExt = valueLower;
            if (!filterExt.empty() && filterExt[0] != '.') {
                filterExt = "." + filterExt;
            }
            
            return extLower == filterExt;
        }
        
        case SmartFolderFilter::Type::NameContains: {
            // Case-insensitive substring match
            return filenameLower.find(valueLower) != std::string::npos;
        }
        
        case SmartFolderFilter::Type::Regex: {
            try {
                std::regex pattern(filter.Value, std::regex::ECMAScript | std::regex::icase);
                return std::regex_search(filename, pattern);
            }
            catch (const std::regex_error&) {
                // Invalid regex, don't match
                return false;
            }
        }
        case SmartFolderFilter::Type::Tag:
            // Tag requires registry; only Evaluate() supports Tag filtering
            return false;
        case SmartFolderFilter::Type::VCSStatus:
        {
            IVCSIntegration* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            if (!vcs)
                return false;
            return vcs->GetFileStatus(file) == ParseVCSStatusString(filter.Value);
        }
    }
    
    return false;
}

void SmartFolderFilterEvaluator::CollectFilesFromRegistry(AssetRegistry* registry,
                                                          const SmartFolder& folder,
                                                          const std::filesystem::path& searchRoot,
                                                          const std::filesystem::path& assetsRoot,
                                                          std::vector<std::filesystem::path>& outFiles) const
{
    if (!registry)
        return;

    // Normalize search root for comparison
    // Get the workspace root (parent of assets root) for resolving relative paths
    std::filesystem::path workspaceRoot = assetsRoot.parent_path();

    const std::string searchRootStr = NormalizePathForCompare(searchRoot);
    
    // A single file-extension filter maps to one registry type. Querying only that
    // type skips the assets that cannot match before any metadata is touched.
    std::optional<AssetType> restrictedType;
    if (folder.Filters.size() == 1 &&
        folder.Filters.front().FilterType == SmartFolderFilter::Type::FileType &&
        !folder.Filters.front().Value.empty())
    {
        std::string extension = ToLowerAscii(folder.Filters.front().Value);
        if (extension.front() != '.')
            extension.insert(extension.begin(), '.');
        const AssetType type = GetAssetTypeFromExtension(extension);
        if (type != AssetType::Unknown)
            restrictedType = type;
    }

    // One lock-consistent snapshot of every registered asset, whatever its type —
    // a smart folder filters on the path and on file properties, so an asset whose
    // type the registry never resolved is still a candidate. Asking per type
    // instead would need a list of types here, and a type missing from that list
    // is a whole category of asset that no smart folder can ever show.
    const Vector<AssetIndexRecord> records = registry->GetAssetIndexSnapshot();

    for (const auto& metadata : records)
    {
        if (restrictedType.has_value() && metadata.Type != *restrictedType)
            continue;

        // Get absolute path for the asset
        std::filesystem::path assetPath = metadata.Path;
        if (assetPath.is_relative())
        {
            // Try with workspace root first (most common case)
            assetPath = workspaceRoot / assetPath;
        }

        const std::string assetPathStr = NormalizePathForCompare(assetPath);
        if (HasPathPrefix(assetPathStr, searchRootStr))
        {
            // Asset is within search root
            outFiles.push_back(assetPath.lexically_normal());
        }
    }
}

void SmartFolderFilterEvaluator::CollectFilesFromFilesystem(const std::filesystem::path& dir,
                                                            std::vector<std::filesystem::path>& outFiles) const
{
    std::error_code ec;
    
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        
        if (ec) {
            ec.clear();
            continue;
        }
        
        const auto& entry = *it;
        
        // Skip hidden files/directories
        std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename[0] == '.') {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        
        // Only include regular files
        if (entry.is_regular_file(ec)) {
            outFiles.push_back(entry.path());
        }
    }
}

} // namespace GameEngine
