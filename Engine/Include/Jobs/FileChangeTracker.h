#pragma once

#include "Types/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace GameEngine {

/**
 * @brief Tracks file changes for incremental compilation
 * 
 * This class provides efficient file change detection using hash-based validation
 * and timestamp optimization. It's designed to support incremental compilation
 * by identifying exactly which files have changed and classifying the type of change.
 */
class FileChangeTracker {
public:
    /**
     * @brief Types of changes that can occur to a file
     */
    enum class ChangeType {
        None,           // No changes detected
        Trivial,        // Comments, whitespace, formatting
        Local,          // Method bodies, private members, local variables
        Interface,      // Public API changes, method signatures
        Structural      // New classes, namespaces, using statements
    };

    /**
     * @brief Metadata about a file and its changes
     */
    struct FileMetadata {
        std::string FilePath;
        std::string ContentHash;        // SHA-256 hash of file content
        std::time_t LastModified;       // File system timestamp
        std::size_t FileSize;           // File size in bytes
        ChangeType ChangeKind;          // Type of change detected
        bool IsNewFile;                 // True if this is a newly created file
        bool IsDeleted;                 // True if this file was deleted

        FileMetadata()
            : LastModified(0), FileSize(0), ChangeKind(ChangeType::None),
              IsNewFile(false), IsDeleted(false) {}
    };

    /**
     * @brief Result of change detection operation
     */
    struct ChangeDetectionResult {
        std::vector<FileMetadata> ChangedFiles;
        std::vector<std::string> NewFiles;
        std::vector<std::string> DeletedFiles;
        std::chrono::milliseconds DetectionTime;
        bool HasStructuralChanges;

        ChangeDetectionResult() : DetectionTime(0), HasStructuralChanges(false) {}
    };

public:
    /**
     * @brief Constructor
     * @param cacheFilePath Path to store the file metadata cache
     */
    explicit FileChangeTracker(const std::string& cacheFilePath = "");

    /**
     * @brief Destructor - saves cache to disk
     */
    ~FileChangeTracker();

    /**
     * @brief Detect changes in the specified directory
     * @param sourceDirectory Directory to scan for C# source files
     * @param recursive Whether to scan subdirectories recursively
     * @return ChangeDetectionResult containing all detected changes
     */
    ChangeDetectionResult DetectChanges(const std::string& sourceDirectory, bool recursive = true);

    /**
     * @brief Update the cache with current file states
     * @param files List of files to update in cache
     */
    void UpdateCache(const std::vector<FileMetadata>& files);

    /**
     * @brief Clear all cached file metadata
     */
    void ClearCache();

    /**
     * @brief Save cache to disk
     * @return True if save was successful
     */
    bool SaveCache();

    /**
     * @brief Load cache from disk
     * @return True if load was successful
     */
    bool LoadCache();

    /**
     * @brief Get cached metadata for a specific file
     * @param filePath Path to the file
     * @return Pointer to metadata if found, nullptr otherwise
     */
    const FileMetadata* GetCachedMetadata(const std::string& filePath) const;

    /**
     * @brief Check if incremental compilation is recommended
     * @param result Change detection result to analyze
     * @return True if incremental compilation should be used
     */
    static bool ShouldUseIncrementalCompilation(const ChangeDetectionResult& result);

    /**
     * @brief Get human-readable description of change type
     * @param changeType The change type to describe
     * @return String description of the change type
     */
    static std::string GetChangeTypeDescription(ChangeType changeType);

private:
    std::unordered_map<std::string, FileMetadata> m_FileCache;
    std::string m_CacheFilePath;
    mutable std::mutex m_CacheMutex;

    /**
     * @brief Calculate SHA-256 hash of file content
     * @param filePath Path to the file
     * @return Hex string representation of the hash
     */
    std::string CalculateFileHash(const std::string& filePath);

    /**
     * @brief Classify the type of change between old and new content
     * @param oldContent Previous file content
     * @param newContent Current file content
     * @return ChangeType classification
     */
    ChangeType ClassifyChange(const std::string& oldContent, const std::string& newContent);

    /**
     * @brief Check if a file should be tracked (e.g., .cs files)
     * @param filePath Path to check
     * @return True if file should be tracked
     */
    bool ShouldTrackFile(const std::string& filePath) const;

    /**
     * @brief Get file metadata from filesystem
     * @param filePath Path to the file
     * @return FileMetadata with current file information
     */
    FileMetadata GetCurrentFileMetadata(const std::string& filePath);

    /**
     * @brief Scan directory for source files
     * @param directory Directory to scan
     * @param recursive Whether to scan recursively
     * @return List of source file paths found
     */
    std::vector<std::string> ScanSourceFiles(const std::string& directory, bool recursive);

    /**
     * @brief Analyze C# content to determine change type
     * @param content C# source code content
     * @return Set of detected language constructs
     */
    struct ContentAnalysis {
        std::unordered_set<std::string> UsingStatements;
        std::unordered_set<std::string> Namespaces;
        std::unordered_set<std::string> ClassNames;
        std::unordered_set<std::string> PublicMethods;
        std::unordered_set<std::string> PublicProperties;
        bool HasStructuralElements;
    };
    
    ContentAnalysis AnalyzeContent(const std::string& content);

    /**
     * @brief Compare two content analyses to determine change type
     * @param oldAnalysis Analysis of previous content
     * @param newAnalysis Analysis of current content
     * @return ChangeType based on differences
     */
    ChangeType CompareContentAnalysis(const ContentAnalysis& oldAnalysis, const ContentAnalysis& newAnalysis);

    /**
     * @brief Serialize cache to JSON format
     * @return JSON string representation of cache
     */
    std::string SerializeCache() const;

    /**
     * @brief Deserialize cache from JSON format
     * @param jsonData JSON string to deserialize
     * @return True if deserialization was successful
     */
    bool DeserializeCache(const std::string& jsonData);
};

} // namespace GameEngine
