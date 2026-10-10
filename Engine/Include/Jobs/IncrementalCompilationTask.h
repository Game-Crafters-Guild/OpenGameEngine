#pragma once

#include "JobSystem/Task.h"
#include "Jobs/FileChangeTracker.h"
#include "Jobs/DependencyGraphAnalyzer.h"
#include "Types/Types.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>

// Forward declaration
namespace GameEngine {
    class CoreCLRHost;
}

namespace GameEngine {

// Use fully qualified JobSystem types to avoid conflicts

/**
 * @brief Task for performing incremental compilation using Roslyn APIs
 *
 * This task integrates FileChangeTracker and DependencyGraphAnalyzer to perform
 * intelligent incremental compilation, achieving target performance of <15ms
 * compilation time vs 800-900ms for full compilation.
 */
class IncrementalCompilationTask : public JobSystem::Task {
public:
    /**
     * @brief Compilation strategy to use
     */
    enum class CompilationStrategy {
        Auto,           // Automatically choose best strategy
        Incremental,    // Force incremental compilation
        Full           // Force full compilation
    };

    /**
     * @brief Result of compilation operation
     */
    struct CompilationResult {
        bool Success = false;
        std::vector<uint8_t> AssemblyBytes;
        std::vector<uint8_t> PdbBytes;
        std::vector<std::string> Errors;
        std::vector<std::string> Warnings;
        std::chrono::milliseconds CompilationTime{0};
        CompilationStrategy StrategyUsed = CompilationStrategy::Auto;
        int FilesCompiled = 0;
        std::string CacheStatus;

        // Performance metrics
        std::chrono::milliseconds ChangeDetectionTime{0};
        std::chrono::milliseconds DependencyAnalysisTime{0};
        std::chrono::milliseconds RoslynCompilationTime{0};
    };

    /**
     * @brief Configuration for incremental compilation
     */
    struct CompilationConfig {
        std::string ProjectPath;
        std::string SourceDirectory;
        CompilationStrategy Strategy = CompilationStrategy::Auto;
        bool EnableCaching = true;
        bool EnableDependencyAnalysis = true;
        bool EnableAssemblyLoadAfterCompile = false; // write DLL/PDB and load via CoreCLRHost

        int MaxIncrementalFiles = 10;  // Max files for incremental compilation
        std::chrono::minutes MaxCacheAge{30};  // Max age before forcing full compilation
    };

public:
    /**
     * @brief Constructor
     * @param jobSystem Thread pool for task execution
     * @param token Cancellation token
     * @param clrHost CoreCLR host for managed compilation
     * @param config Compilation configuration
     */
    explicit IncrementalCompilationTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                       CoreCLRHost& clrHost,
                                       const CompilationConfig& config);

    /**
     * @brief Destructor
     */
    ~IncrementalCompilationTask() override;

    /**
     * @brief Execute the incremental compilation task
     */
    void Execute() override;

    /**
     * @brief Get the compilation result
     * @return CompilationResult with details about the compilation
     */
    const CompilationResult& GetResult() const { return m_Result; }

    /**
     * @brief Check if compilation was successful
     * @return True if compilation succeeded
     */
    bool IsSuccessful() const { return m_Result.Success; }

    /**
     * @brief Get compiled assembly bytes
     * @return Vector of assembly bytes
     */
    const std::vector<uint8_t>& GetAssemblyBytes() const { return m_Result.AssemblyBytes; }

    /**
     * @brief Get compilation time
     * @return Duration of compilation
     */
    std::chrono::milliseconds GetCompilationTime() const { return m_Result.CompilationTime; }

    /**
     * @brief Get strategy that was used
     * @return CompilationStrategy that was actually used
     */
    CompilationStrategy GetStrategyUsed() const { return m_Result.StrategyUsed; }

    /**
     * @brief Clear compilation cache
     */
    void ClearCache();

    /**
     * @brief Get cache statistics
     * @return String with cache statistics
     */
    std::string GetCacheStats() const;

    /**
     * @brief Force full compilation on next execution
     */
    void ForceFullCompilation() { m_ForceFullCompilation = true; }

    /**
     * @brief Update configuration
     * @param config New configuration
     */
    void UpdateConfig(const CompilationConfig& config);

private:
    CompilationConfig m_Config;
    CompilationResult m_Result;

    std::unique_ptr<FileChangeTracker> m_FileChangeTracker;
    std::unique_ptr<DependencyGraphAnalyzer> m_DependencyAnalyzer;
    CoreCLRHost& m_ClrHost;

    bool m_ForceFullCompilation = false;
    std::chrono::time_point<std::chrono::steady_clock> m_LastFullCompilation;

    /**
     * @brief Determine the best compilation strategy
     * @param changes File change detection result
     * @param dependencies Dependency analysis result
     * @return CompilationStrategy to use
     */
    CompilationStrategy DetermineStrategy(
        const FileChangeTracker::ChangeDetectionResult& changes,
        const DependencyGraphAnalyzer::AnalysisResult& dependencies);

    /**
     * @brief Perform incremental compilation
     * @param changedFiles List of changed files
     * @param affectedFiles List of affected files
     * @return True if compilation succeeded
     */
    bool PerformIncrementalCompilation(
        const std::vector<std::string>& changedFiles,
        const std::vector<std::string>& affectedFiles);

    /**
     * @brief Perform full compilation
     * @return True if compilation succeeded
     */
    bool PerformFullCompilation();

    /**
     * @brief Call managed incremental compiler
     * @param requestJson JSON request for compilation
     * @param isIncremental True for incremental, false for full compilation
     * @return True if compilation succeeded
     */
    bool CallManagedCompiler(const std::string& requestJson, bool isIncremental);

    /**
     * @brief Create JSON request for managed compiler
     * @param changedFiles List of changed files
     * @param affectedFiles List of affected files
     * @param allSourceFiles List of all source files
     * @param isIncremental True for incremental compilation
     * @return JSON string for compilation request
     */
    std::string CreateCompilationRequest(
        const std::vector<std::string>& changedFiles,
        const std::vector<std::string>& affectedFiles,
        const std::vector<std::string>& allSourceFiles,
        bool isIncremental);

    /**
     * @brief Get all source files in the project
     * @return List of source file paths
     */
    std::vector<std::string> GetAllSourceFiles();

    /**
     * @brief Log performance metrics
     */
    void LogPerformanceMetrics();

    /**
     * @brief Check if cache is too old
     * @return True if cache should be invalidated
     */
    bool IsCacheTooOld() const;

    /**
     * @brief Validate compilation result
     * @param assemblyBytes Compiled assembly bytes
     * @return True if assembly is valid
     */
    bool ValidateCompiledAssembly(const std::vector<uint8_t>& assemblyBytes);
};




/**
 * @brief Factory function to create incremental compilation task
 * @param jobSystem Thread pool for task execution
 * @param token Cancellation token
 * @param clrHost CoreCLR host for managed compilation
 * @param config Compilation configuration
 * @return Unique pointer to IncrementalCompilationTask
 */
std::unique_ptr<IncrementalCompilationTask> CreateIncrementalCompilationTask(
    JobSystem::WorkStealingThreadPool* jobSystem,
    CoreCLRHost& clrHost,
    const IncrementalCompilationTask::CompilationConfig& config);

} // namespace GameEngine
