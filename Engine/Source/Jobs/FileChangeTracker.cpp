#include "Jobs/FileChangeTracker.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <sstream>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <mutex>
#include <functional>

namespace GameEngine {

FileChangeTracker::FileChangeTracker(const std::string& cacheFilePath)
    : m_CacheFilePath(cacheFilePath.empty() ? "file_change_cache.json" : cacheFilePath) {
    LoadCache();
}

FileChangeTracker::~FileChangeTracker() {
    SaveCache();
}

FileChangeTracker::ChangeDetectionResult FileChangeTracker::DetectChanges(
    const std::string& sourceDirectory, bool recursive) {
    
    auto startTime = std::chrono::high_resolution_clock::now();
    ChangeDetectionResult result;
    
    Logger::Log::Debug("[FileChangeTracker] Starting change detection in: {}", sourceDirectory);
    
    try {
        // Scan for current source files
        auto currentFiles = ScanSourceFiles(sourceDirectory, recursive);
        Logger::Log::Debug("[FileChangeTracker] Found {} source files", currentFiles.size());
        
        std::unordered_set<std::string> currentFileSet(currentFiles.begin(), currentFiles.end());
        std::unordered_set<std::string> cachedFileSet;
        
        // Build set of cached files
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            for (const auto& [filePath, metadata] : m_FileCache) {
                cachedFileSet.insert(filePath);
            }
        }
        
        // Find deleted files
        for (const auto& cachedFile : cachedFileSet) {
            if (currentFileSet.find(cachedFile) == currentFileSet.end()) {
                result.DeletedFiles.push_back(cachedFile);
                Logger::Log::Debug("[FileChangeTracker] Deleted file: {}", cachedFile);
            }
        }
        
        // Check each current file for changes
        for (const auto& filePath : currentFiles) {
            auto currentMetadata = GetCurrentFileMetadata(filePath);
            
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            auto cachedIt = m_FileCache.find(filePath);
            
            if (cachedIt == m_FileCache.end()) {
                // New file
                currentMetadata.IsNewFile = true;
                currentMetadata.ChangeKind = ChangeType::Structural;
                result.NewFiles.push_back(filePath);
                result.ChangedFiles.push_back(currentMetadata);
                result.HasStructuralChanges = true;
                Logger::Log::Debug("[FileChangeTracker] New file: {}", filePath);
            } else {
                const auto& cachedMetadata = cachedIt->second;
                
                // Quick timestamp check first
                if (currentMetadata.LastModified != cachedMetadata.LastModified ||
                    currentMetadata.FileSize != cachedMetadata.FileSize) {

                    // File potentially changed, verify with hash
                    if (currentMetadata.ContentHash != cachedMetadata.ContentHash) {
                        // Classification needs previous content we don't cache;
                        // every verified, still-readable change counts as
                        // structural.
                        if (SharedFileReader(std::filesystem::path(filePath)).IsOpen()) {
                            currentMetadata.ChangeKind = ChangeType::Structural;
                            result.ChangedFiles.push_back(currentMetadata);
                            result.HasStructuralChanges = true;

                            Logger::Log::Debug("[FileChangeTracker] Changed file: {} ({})",
                                        filePath, GetChangeTypeDescription(currentMetadata.ChangeKind));
                        }
                    }
                }
            }
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        result.DetectionTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        Logger::Log::Info("[FileChangeTracker] Change detection completed in {}ms: {} changed, {} new, {} deleted",
                    result.DetectionTime.count(), result.ChangedFiles.size(),
                    result.NewFiles.size(), result.DeletedFiles.size());
        
    } catch (const std::exception& e) {
        Logger::Log::Error("[FileChangeTracker] Error during change detection: {}", e.what());
    }
    
    return result;
}

void FileChangeTracker::UpdateCache(const std::vector<FileMetadata>& files) {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    
    for (const auto& file : files) {
        m_FileCache[file.FilePath] = file;
    }
    
    Logger::Log::Debug("[FileChangeTracker] Updated cache with {} files", files.size());
}

void FileChangeTracker::ClearCache() {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    m_FileCache.clear();
    Logger::Log::Debug("[FileChangeTracker] Cache cleared");
}

std::string FileChangeTracker::CalculateFileHash(const std::string& filePath) {
    Vector<uint8> buffer;
    if (!ReadFileBytesShared(std::filesystem::path(filePath), buffer)) {
        Logger::Log::Warning("[FileChangeTracker] Could not open file for hashing: {}", filePath);
        return "";
    }

    // FNV-1a hash (64-bit)
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    const uint64_t prime = 1099511628211ULL;  // FNV prime

    for (uint8 byte : buffer) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= prime;
    }

    // Convert to hex string
    std::stringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

FileChangeTracker::ChangeType FileChangeTracker::ClassifyChange(
    const std::string& oldContent, const std::string& newContent) {
    
    // For now, implement basic classification
    // TODO: Implement sophisticated C# syntax analysis
    
    auto oldAnalysis = AnalyzeContent(oldContent);
    auto newAnalysis = AnalyzeContent(newContent);
    
    return CompareContentAnalysis(oldAnalysis, newAnalysis);
}

bool FileChangeTracker::ShouldTrackFile(const std::string& filePath) const {
    std::filesystem::path path(filePath);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    return extension == ".cs";
}

FileChangeTracker::FileMetadata FileChangeTracker::GetCurrentFileMetadata(const std::string& filePath) {
    FileMetadata metadata;
    metadata.FilePath = filePath;
    
    try {
        if (std::filesystem::exists(filePath)) {
            auto fileTime = std::filesystem::last_write_time(filePath);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            metadata.LastModified = std::chrono::system_clock::to_time_t(sctp);

            metadata.FileSize = std::filesystem::file_size(filePath);
            metadata.ContentHash = CalculateFileHash(filePath);
        }
    } catch (const std::exception& e) {
        Logger::Log::Warning("[FileChangeTracker] Error getting metadata for {}: {}", filePath, e.what());
    }
    
    return metadata;
}

std::vector<std::string> FileChangeTracker::ScanSourceFiles(const std::string& directory, bool recursive) {
    std::vector<std::string> sourceFiles;
    
    try {
        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file() && ShouldTrackFile(entry.path().string())) {
                    sourceFiles.push_back(entry.path().string());
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (entry.is_regular_file() && ShouldTrackFile(entry.path().string())) {
                    sourceFiles.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::Log::Error("[FileChangeTracker] Error scanning directory {}: {}", directory, e.what());
    }
    
    return sourceFiles;
}

FileChangeTracker::ContentAnalysis FileChangeTracker::AnalyzeContent(const std::string& content) {
    ContentAnalysis analysis;
    
    // Basic regex patterns for C# constructs
    std::regex usingRegex(R"(^\s*using\s+([^;]+);)", std::regex_constants::ECMAScript);
    std::regex namespaceRegex(R"(namespace\s+([^\s{]+))");
    std::regex classRegex(R"((?:public\s+)?(?:partial\s+)?class\s+(\w+))");
    std::regex publicMethodRegex(R"(public\s+(?:\w+\s+)*(\w+)\s*\([^)]*\))");
    
    std::smatch match;
    std::string::const_iterator searchStart(content.cbegin());
    
    // Extract using statements
    while (std::regex_search(searchStart, content.cend(), match, usingRegex)) {
        analysis.UsingStatements.insert(match[1].str());
        searchStart = match.suffix().first;
    }
    
    // Extract namespaces
    searchStart = content.cbegin();
    while (std::regex_search(searchStart, content.cend(), match, namespaceRegex)) {
        analysis.Namespaces.insert(match[1].str());
        searchStart = match.suffix().first;
    }
    
    // Extract class names
    searchStart = content.cbegin();
    while (std::regex_search(searchStart, content.cend(), match, classRegex)) {
        analysis.ClassNames.insert(match[1].str());
        searchStart = match.suffix().first;
    }
    
    // Extract public methods
    searchStart = content.cbegin();
    while (std::regex_search(searchStart, content.cend(), match, publicMethodRegex)) {
        analysis.PublicMethods.insert(match[1].str());
        searchStart = match.suffix().first;
    }
    
    analysis.HasStructuralElements = !analysis.Namespaces.empty() || !analysis.ClassNames.empty();
    
    return analysis;
}

FileChangeTracker::ChangeType FileChangeTracker::CompareContentAnalysis(
    const ContentAnalysis& oldAnalysis, const ContentAnalysis& newAnalysis) {
    
    // Check for structural changes
    if (oldAnalysis.UsingStatements != newAnalysis.UsingStatements ||
        oldAnalysis.Namespaces != newAnalysis.Namespaces ||
        oldAnalysis.ClassNames != newAnalysis.ClassNames) {
        return ChangeType::Structural;
    }
    
    // Check for interface changes
    if (oldAnalysis.PublicMethods != newAnalysis.PublicMethods ||
        oldAnalysis.PublicProperties != newAnalysis.PublicProperties) {
        return ChangeType::Interface;
    }
    
    // If we get here, assume local changes
    return ChangeType::Local;
}

bool FileChangeTracker::ShouldUseIncrementalCompilation(const ChangeDetectionResult& result) {
    // Don't use incremental compilation if there are structural changes
    if (result.HasStructuralChanges) {
        return false;
    }
    
    // Don't use incremental compilation if too many files changed
    if (result.ChangedFiles.size() > 10) {
        return false;
    }
    
    // Don't use incremental compilation if there are new or deleted files
    if (!result.NewFiles.empty() || !result.DeletedFiles.empty()) {
        return false;
    }
    
    return true;
}

std::string FileChangeTracker::GetChangeTypeDescription(ChangeType changeType) {
    switch (changeType) {
        case ChangeType::None: return "None";
        case ChangeType::Trivial: return "Trivial";
        case ChangeType::Local: return "Local";
        case ChangeType::Interface: return "Interface";
        case ChangeType::Structural: return "Structural";
        default: return "Unknown";
    }
}

bool FileChangeTracker::SaveCache() {
    // TODO: Implement JSON serialization
    Logger::Log::Debug("[FileChangeTracker] Cache save not yet implemented");
    return true;
}

bool FileChangeTracker::LoadCache() {
    // TODO: Implement JSON deserialization
    Logger::Log::Debug("[FileChangeTracker] Cache load not yet implemented");
    return true;
}

const FileChangeTracker::FileMetadata* FileChangeTracker::GetCachedMetadata(const std::string& filePath) const {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    auto it = m_FileCache.find(filePath);
    return (it != m_FileCache.end()) ? &it->second : nullptr;
}

} // namespace GameEngine
