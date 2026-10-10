#pragma once

#include "UI/SmartFolder/SmartFolder.h"
#include <vector>
#include <filesystem>

namespace GameEngine {

class AssetRegistry;

/// Evaluates smart folder filters and returns matching files
class SmartFolderFilterEvaluator {
public:
    /// Evaluate a smart folder and return all matching file paths
    /// @param folder The smart folder with filter criteria
    /// @param assetsRoot The root assets directory to search
    /// @param registry Optional AssetRegistry for indexed lookup (faster, respects ignore rules)
    /// @return Vector of absolute paths to matching files
    std::vector<std::filesystem::path> Evaluate(
        const SmartFolder& folder,
        const std::filesystem::path& assetsRoot,
        AssetRegistry* registry = nullptr) const;
    
    /// Check if a single file matches the smart folder filters
    bool Matches(const std::filesystem::path& file, const SmartFolder& folder) const;
    
private:
    /// Check if a file matches a single filter
    bool MatchesFilter(const std::filesystem::path& file, const SmartFolderFilter& filter) const;
    
    /// Collect files using the AssetRegistry (indexed lookup)
    void CollectFilesFromRegistry(AssetRegistry* registry,
                                  const SmartFolder& folder,
                                  const std::filesystem::path& searchRoot,
                                  const std::filesystem::path& assetsRoot,
                                  std::vector<std::filesystem::path>& outFiles) const;
    
    /// Recursively collect all files in a directory (filesystem fallback)
    void CollectFilesFromFilesystem(const std::filesystem::path& dir, 
                                    std::vector<std::filesystem::path>& outFiles) const;
};

} // namespace GameEngine
