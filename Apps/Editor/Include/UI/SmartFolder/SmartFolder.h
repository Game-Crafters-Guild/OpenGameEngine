#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace GameEngine {

/// Filter criteria for a smart folder
struct SmartFolderFilter {
    /// Type of filter to apply
    enum class Type {
        FileType,      ///< Match by file extension (e.g., ".png", ".cs")
        NameContains,  ///< Match if filename contains substring
        Regex,         ///< Match against regular expression
        Tag,           ///< Match if asset has this tag (value = tag name)
        VCSStatus      ///< Match by version control status (value = status name)
    };

    Type FilterType = Type::NameContains;
    std::string Value;  ///< Extension, substring, or regex pattern

    bool operator==(const SmartFolderFilter& other) const {
        return FilterType == other.FilterType && Value == other.Value;
    }
};

/// How to combine multiple filters
enum class FilterCombineMode {
    And,  ///< File must match ALL filters
    Or    ///< File must match ANY filter
};

/// A smart folder that shows files matching filter criteria
struct SmartFolder {
    std::string Id;                              ///< Unique identifier (UUID)
    std::string Name;                            ///< Display name
    std::filesystem::path DirectoryScope;        ///< Root directory to search (empty = entire project)
    std::vector<SmartFolderFilter> Filters;      ///< Filter criteria
    FilterCombineMode CombineMode = FilterCombineMode::Or;
    bool IsGlobal = false;                       ///< true = stored globally, false = per-project

    bool operator==(const SmartFolder& other) const {
        return Id == other.Id;
    }
};

/// Generate a unique ID for a new smart folder
std::string GenerateSmartFolderId();

} // namespace GameEngine
