#include "Jobs/IncrementalCompilationTask.h"
#include <fstream>

#include "Logger/Logger.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PathResolver.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "Jobs/CompileServerClient.h"
#include "Jobs/PipeTransport.h"
#include "Jobs/WorkspaceId.h"


namespace GameEngine {

IncrementalCompilationTask::IncrementalCompilationTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                                     CoreCLRHost& clrHost,
                                                     const CompilationConfig& config)
    : Task(jobSystem)
    , m_Config(config)
    , m_ClrHost(clrHost)
    , m_LastFullCompilation(std::chrono::steady_clock::now()) {

    Logger::Log::Debug("[IncrementalCompilationTask] Initializing with project: {}", config.ProjectPath);

    // Initialize file change tracker
    std::filesystem::path cachePath = std::filesystem::path(config.ProjectPath).parent_path() / "file_change_cache.json";
    std::string cacheFile = cachePath.string();
    m_FileChangeTracker = std::make_unique<FileChangeTracker>(cacheFile);

    // Initialize dependency analyzer
    m_DependencyAnalyzer = std::make_unique<DependencyGraphAnalyzer>();

    Logger::Log::Info("[IncrementalCompilationTask] Initialized for project: {}",
                std::filesystem::path(config.ProjectPath).filename().string());
}

IncrementalCompilationTask::~IncrementalCompilationTask() = default;

void IncrementalCompilationTask::Execute() {
    auto startTime = std::chrono::high_resolution_clock::now();

    Logger::Log::Info("[IncrementalCompilationTask] Starting incremental compilation analysis");

    try {
        // Reset result
        m_Result = CompilationResult{};

        // Step 1: Detect file changes
        auto changeStart = std::chrono::high_resolution_clock::now();
        auto changes = m_FileChangeTracker->DetectChanges(m_Config.SourceDirectory);
        auto changeEnd = std::chrono::high_resolution_clock::now();
        m_Result.ChangeDetectionTime = std::chrono::duration_cast<std::chrono::milliseconds>(changeEnd - changeStart);

        Logger::Log::Debug("[IncrementalCompilationTask] Change detection: {}ms, {} changed files",
                     m_Result.ChangeDetectionTime.count(), changes.ChangedFiles.size());

        // Step 2: Analyze dependencies if enabled
        DependencyGraphAnalyzer::AnalysisResult dependencies;
        if (m_Config.EnableDependencyAnalysis && !changes.ChangedFiles.empty()) {
            auto depStart = std::chrono::high_resolution_clock::now();

            // Get all source files for dependency analysis
            auto allSourceFiles = GetAllSourceFiles();

            // Build dependency graph if not already built or if files changed significantly
            if (changes.NewFiles.size() > 0 || changes.DeletedFiles.size() > 0 ||
                changes.HasStructuralChanges) {
                Logger::Log::Debug("[IncrementalCompilationTask] Rebuilding dependency graph");
                m_DependencyAnalyzer->BuildGraph(allSourceFiles);
            }

            // Get affected files
            std::vector<std::string> changedFilePaths;
            for (const auto& file : changes.ChangedFiles) {
                changedFilePaths.push_back(file.FilePath);
            }
            dependencies = m_DependencyAnalyzer->GetAffectedFiles(changedFilePaths);

            auto depEnd = std::chrono::high_resolution_clock::now();
            m_Result.DependencyAnalysisTime = std::chrono::duration_cast<std::chrono::milliseconds>(depEnd - depStart);

            Logger::Log::Debug("[IncrementalCompilationTask] Dependency analysis: {}ms, {} affected files",
                         m_Result.DependencyAnalysisTime.count(), dependencies.AffectedFiles.size());
        }

        // Step 3: Determine compilation strategy
        auto strategy = DetermineStrategy(changes, dependencies);
        m_Result.StrategyUsed = strategy;

        Logger::Log::Info("[IncrementalCompilationTask] Using {} compilation strategy",
                    strategy == CompilationStrategy::Incremental ? "INCREMENTAL" : "FULL");

        // Step 4: Perform compilation
        auto compileStart = std::chrono::high_resolution_clock::now();
        bool success = false;

        if (strategy == CompilationStrategy::Incremental) {
            std::vector<std::string> changedFilePaths;
            for (const auto& file : changes.ChangedFiles) {
                changedFilePaths.push_back(file.FilePath);
            }
            success = PerformIncrementalCompilation(changedFilePaths, dependencies.AffectedFiles);
        } else {
            success = PerformFullCompilation();
        }

        auto compileEnd = std::chrono::high_resolution_clock::now();
        m_Result.RoslynCompilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(compileEnd - compileStart);

        m_Result.Success = success;

        // Step 5: Update cache if successful
        if (success) {
            m_FileChangeTracker->UpdateCache(changes.ChangedFiles);
            if (strategy == CompilationStrategy::Full) {
                m_LastFullCompilation = std::chrono::steady_clock::now();
            }
        }

        // Calculate total time
        auto endTime = std::chrono::high_resolution_clock::now();
        m_Result.CompilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Log performance metrics
        LogPerformanceMetrics();

    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Exception during compilation: {}", e.what());
        m_Result.Success = false;
        m_Result.Errors.push_back(std::string("Compilation exception: ") + e.what());
    }
}

IncrementalCompilationTask::CompilationStrategy IncrementalCompilationTask::DetermineStrategy(
    const FileChangeTracker::ChangeDetectionResult& changes,
    const DependencyGraphAnalyzer::AnalysisResult& dependencies) {

    // OPTIMIZATION: Fast path for common cases
    // Check configuration strategy first (most common case)
    if (m_Config.Strategy == CompilationStrategy::Incremental) {
        // Fast path: Always prefer incremental unless there are major issues
        if (!dependencies.HasCircularDependencies &&
            changes.ChangedFiles.size() <= static_cast<size_t>(m_Config.MaxIncrementalFiles * 2)) {
            return CompilationStrategy::Incremental;
        }
    }

    // Check if forced full compilation
    if (m_ForceFullCompilation) {
        Logger::Log::Debug("[IncrementalCompilationTask] Full compilation forced");
        m_ForceFullCompilation = false;
        return CompilationStrategy::Full;
    }

    if (m_Config.Strategy == CompilationStrategy::Full) {
        Logger::Log::Debug("[IncrementalCompilationTask] Full compilation forced by config");
        return CompilationStrategy::Full;
    }

    // Auto strategy - make intelligent decision (OPTIMIZED for performance)

    // OPTIMIZATION: Prefer incremental compilation for better performance
    // No changes = no compilation needed (use incremental for consistency)
    if (changes.ChangedFiles.empty()) {
        Logger::Log::Debug("[IncrementalCompilationTask] No changes detected, using incremental compilation");
        return CompilationStrategy::Incremental;
    }

    // OPTIMIZATION: Increased threshold for incremental compilation
    // Too many changed files = full compilation (increased threshold)
    if (changes.ChangedFiles.size() > static_cast<size_t>(m_Config.MaxIncrementalFiles * 2)) {
        Logger::Log::Debug("[IncrementalCompilationTask] Too many changed files ({}), using full compilation",
                     changes.ChangedFiles.size());
        return CompilationStrategy::Full;
    }

    // OPTIMIZATION: Only major structural changes require full compilation
    // Minor structural changes can use incremental
    if (changes.HasStructuralChanges && changes.ChangedFiles.size() > 5) {
        Logger::Log::Debug("[IncrementalCompilationTask] Major structural changes detected, using full compilation");
        return CompilationStrategy::Full;
    }

    // OPTIMIZATION: New/deleted files can often use incremental if count is low
    if ((!changes.NewFiles.empty() || !changes.DeletedFiles.empty()) &&
        (changes.NewFiles.size() + changes.DeletedFiles.size()) > 3) {
        Logger::Log::Debug("[IncrementalCompilationTask] Many new/deleted files detected, using full compilation");
        return CompilationStrategy::Full;
    }

    // OPTIMIZATION: Extend cache validity for better performance
    // Cache too old = full compilation (extended validity)
    if (IsCacheTooOld() && changes.ChangedFiles.size() > 10) {
        Logger::Log::Debug("[IncrementalCompilationTask] Cache too old with many changes, using full compilation");
        return CompilationStrategy::Full;
    }

    // Circular dependencies = full compilation (keep this check)
    if (dependencies.HasCircularDependencies) {
        Logger::Log::Debug("[IncrementalCompilationTask] Circular dependencies detected, using full compilation");
        return CompilationStrategy::Full;
    }

    // All checks passed - use incremental compilation
    Logger::Log::Debug("[IncrementalCompilationTask] All checks passed, using incremental compilation");
    return CompilationStrategy::Incremental;
}

bool IncrementalCompilationTask::PerformIncrementalCompilation(
    const std::vector<std::string>& changedFiles,
    const std::vector<std::string>& affectedFiles) {

    Logger::Log::Info("[IncrementalCompilationTask] Performing incremental compilation: {} changed, {} affected",
                changedFiles.size(), affectedFiles.size());

    try {
        // Get all source files
        auto allSourceFiles = GetAllSourceFiles();

        // Create compilation request
        std::string requestJson = CreateCompilationRequest(changedFiles, affectedFiles, allSourceFiles, true);

        // Call managed compiler
        bool success = CallManagedCompiler(requestJson, true);

        if (success) {
            m_Result.FilesCompiled = static_cast<int>(changedFiles.size());
            m_Result.CacheStatus = "Incremental update";
            Logger::Log::Info("[IncrementalCompilationTask] ✅ Incremental compilation successful");
        } else {
            Logger::Log::Warning("[IncrementalCompilationTask] ❌ Incremental compilation failed, falling back to full compilation");
            // Fallback to full compilation
            return PerformFullCompilation();
        }

        return success;

    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Exception in incremental compilation: {}", e.what());
        return false;
    }
}

bool IncrementalCompilationTask::PerformFullCompilation() {
    Logger::Log::Info("[IncrementalCompilationTask] Performing full compilation");

    try {
        // Get all source files
        auto allSourceFiles = GetAllSourceFiles();

        // Create compilation request
        std::string requestJson = CreateCompilationRequest({}, {}, allSourceFiles, false);

        // Call managed compiler
        bool success = CallManagedCompiler(requestJson, false);

        if (success) {
            m_Result.FilesCompiled = static_cast<int>(allSourceFiles.size());
            m_Result.CacheStatus = "Full rebuild";
            Logger::Log::Info("[IncrementalCompilationTask] ✅ Full compilation successful");
        } else {
            Logger::Log::Error("[IncrementalCompilationTask] ❌ Full compilation failed");
        }

        return success;

    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Exception in full compilation: {}", e.what());
        return false;
    }
}

bool IncrementalCompilationTask::CallManagedCompiler(const std::string& requestJson, bool /*isIncremental*/) {
    try {
        // Default: use CompileServer IPC via PipeTransport with per-workspace pipe naming
        std::string projectRoot = std::filesystem::path(m_Config.ProjectPath).parent_path().string();
        std::string wsId = WorkspaceId::Compute(projectRoot);
        std::string pipeName = std::string("GE_CompileServer_") + wsId;
        CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
        CompileServerResponse resp;
        if (client.Compile(requestJson, resp)) {
            // Map diagnostics
            m_Result.Warnings.clear();
            for (const auto& w : resp.Warnings) {
                m_Result.Warnings.push_back(w.FileUtf8 + ":" + std::to_string(w.Line) + "," + std::to_string(w.Column) + " " + w.Code + ": " + w.MessageUtf8);
            }
            m_Result.Errors.clear();
            for (const auto& e : resp.Errors) {
                m_Result.Errors.push_back(e.FileUtf8 + ":" + std::to_string(e.Line) + "," + std::to_string(e.Column) + " " + e.Code + ": " + e.MessageUtf8);
            }
            if (resp.Success) {
                Logger::Log::Debug("[IncrementalCompilationTask] CompileServer IPC reported success");
                m_Result.Success = true;
                m_Result.AssemblyBytes = std::move(resp.AssemblyBytes);
                m_Result.PdbBytes = std::move(resp.PdbBytes);
                // Optional: validate PE header quickly
                ValidateCompiledAssembly(m_Result.AssemblyBytes);

                // If configured, write to disk and request CoreCLRHost to load
                if (m_Config.EnableAssemblyLoadAfterCompile) {
                    try {
                        std::filesystem::path outDir = std::filesystem::path(m_Config.ProjectPath).parent_path() / "bin" / "GE_Temp";
                        std::error_code fec; std::filesystem::create_directories(outDir, fec);
                        std::filesystem::path dllPath = outDir / ScriptingPaths::kScriptsAssemblyFileName;
                        std::filesystem::path pdbPath = dllPath;
                        pdbPath.replace_extension(".pdb");
                        {
                            std::ofstream df(dllPath, std::ios::binary);
                            df.write(reinterpret_cast<const char*>(m_Result.AssemblyBytes.data()), (std::streamsize)m_Result.AssemblyBytes.size());
                        }
                        if (!m_Result.PdbBytes.empty()) {
                            std::ofstream pf(pdbPath, std::ios::binary);
                            pf.write(reinterpret_cast<const char*>(m_Result.PdbBytes.data()), (std::streamsize)m_Result.PdbBytes.size());
                        }
                        if (!m_ClrHost.PreloadAndSwapFromPath(dllPath)) {
                            m_Result.Errors.push_back("Failed to load compiled assembly via CoreCLRHost");
                            m_Result.Success = false;
                            return false;
                        }
                    } catch (const std::exception& ex) {
                        m_Result.Errors.push_back(std::string("Exception writing/loading compiled assembly: ") + ex.what());
                        m_Result.Success = false;
                        return false;
                    }
                }
                return true;
            }
            Logger::Log::Warning("[IncrementalCompilationTask] Managed compiler returned errors: {}", m_Result.Errors.size());
            m_Result.Success = false;
            return false;
        }
        Logger::Log::Warning("[IncrementalCompilationTask] CompileServer IPC failed; no legacy fallback enabled");
        m_Result.Success = false;
        return false;
    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Exception calling CompileServer: {}", e.what());
        return false;
    }
}

std::string IncrementalCompilationTask::CreateCompilationRequest(
    const std::vector<std::string>& changedFiles,
    const std::vector<std::string>& affectedFiles,
    const std::vector<std::string>& allSourceFiles,
    bool isIncremental) {

    nlohmann::json j;
    std::string projectRoot = std::filesystem::path(m_Config.ProjectPath).parent_path().string();
    j["ProjectRoot"] = projectRoot;
    j["ChangedFiles"] = changedFiles;
    j["AffectedFiles"] = affectedFiles;
    j["AllFiles"] = allSourceFiles;
    j["PreferredStrategy"] = isIncremental ? "Incremental" : "Full";
    j["ForceFull"] = !isIncremental;
    return j.dump();
}

std::vector<std::string> IncrementalCompilationTask::GetAllSourceFiles() {
    std::vector<std::string> sourceFiles;

    try {
        std::filesystem::path projectDir = std::filesystem::path(m_Config.ProjectPath).parent_path();

        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cs") {
                // Skip bin and obj directories
                std::string pathStr = entry.path().string();
                if (pathStr.find("\\bin\\") == std::string::npos &&
                    pathStr.find("\\obj\\") == std::string::npos &&
                    pathStr.find("/bin/") == std::string::npos &&
                    pathStr.find("/obj/") == std::string::npos) {
                    sourceFiles.push_back(entry.path().string());
                }
            }
        }

        Logger::Log::Debug("[IncrementalCompilationTask] Found {} source files", sourceFiles.size());

    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Error scanning source files: {}", e.what());
    }

    return sourceFiles;
}

void IncrementalCompilationTask::LogPerformanceMetrics() {
    Logger::Log::Info("[IncrementalCompilationTask] 📊 Performance Metrics:");
    Logger::Log::Info("  Total Time: {}ms", m_Result.CompilationTime.count());
    Logger::Log::Info("  Change Detection: {}ms", m_Result.ChangeDetectionTime.count());
    Logger::Log::Info("  Dependency Analysis: {}ms", m_Result.DependencyAnalysisTime.count());
    Logger::Log::Info("  Roslyn Compilation: {}ms", m_Result.RoslynCompilationTime.count());
    Logger::Log::Info("  Strategy Used: {}",
                m_Result.StrategyUsed == CompilationStrategy::Incremental ? "Incremental" : "Full");
    Logger::Log::Info("  Files Compiled: {}", m_Result.FilesCompiled);
    Logger::Log::Info("  Cache Status: {}", m_Result.CacheStatus);
    Logger::Log::Info("  Assembly Size: {} bytes", m_Result.AssemblyBytes.size());

    // Performance target analysis
    if (m_Result.StrategyUsed == CompilationStrategy::Incremental) {
        if (m_Result.CompilationTime.count() <= 15) {
            Logger::Log::Info("  🎯 PERFORMANCE TARGET MET: {}ms ≤ 15ms target", m_Result.CompilationTime.count());
        } else {
            Logger::Log::Warning("  ⚠️ PERFORMANCE TARGET MISSED: {}ms > 15ms target", m_Result.CompilationTime.count());
        }
    }
}

bool IncrementalCompilationTask::IsCacheTooOld() const {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::minutes>(now - m_LastFullCompilation);
    return age > m_Config.MaxCacheAge;
}

bool IncrementalCompilationTask::ValidateCompiledAssembly(const std::vector<uint8_t>& assemblyBytes) {
    if (assemblyBytes.empty()) {
        Logger::Log::Warning("[IncrementalCompilationTask] Assembly bytes are empty");
        return false;
    }

    if (assemblyBytes.size() < 64) {
        Logger::Log::Warning("[IncrementalCompilationTask] Assembly too small: {} bytes", assemblyBytes.size());
        return false;
    }

    // Check PE header
    if (assemblyBytes[0] != 0x4D || assemblyBytes[1] != 0x5A) { // "MZ"
        Logger::Log::Warning("[IncrementalCompilationTask] Invalid PE header");
        return false;
    }

    Logger::Log::Debug("[IncrementalCompilationTask] Assembly validation passed: {} bytes", assemblyBytes.size());
    return true;
}

void IncrementalCompilationTask::ClearCache() {
    if (m_FileChangeTracker) {
        m_FileChangeTracker->ClearCache();
    }

    // Clear compiler cache via managed code (legacy string command removed)
    try {
        GE_ClearCompilerCache();
        Logger::Log::Info("[IncrementalCompilationTask] Cache cleared");
    } catch (const std::exception& e) {
        Logger::Log::Error("[IncrementalCompilationTask] Error clearing cache: {}", e.what());
    }
}

std::string IncrementalCompilationTask::GetCacheStats() const {
    try {
        // Try advanced stats first
        GE_Result result = GE_GetCompilerStats();
        if (result == GE_Result_Ok) {
            return "Compiler stats requested (check logs)";
        }
        return "Compiler stats not available (GetCompilerStats failed)";
    } catch (const std::exception& e) {
        return std::string("Error getting cache stats: ") + e.what();
    }
}

void IncrementalCompilationTask::UpdateConfig(const CompilationConfig& config) {
    m_Config = config;
    Logger::Log::Debug("[IncrementalCompilationTask] Configuration updated");
}

std::unique_ptr<IncrementalCompilationTask> CreateIncrementalCompilationTask(
    JobSystem::WorkStealingThreadPool* jobSystem,
    CoreCLRHost& clrHost,
    const IncrementalCompilationTask::CompilationConfig& config) {
    return std::make_unique<IncrementalCompilationTask>(jobSystem, clrHost, config);
}

} // namespace GameEngine
