#include <gtest/gtest.h>
#include "Jobs/IncrementalCompilationTask.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/CoreCLRHost.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace GameEngine;

class IncrementalCompilationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize logger
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
        
        // Create test project directory
        testProjectDir = std::filesystem::temp_directory_path() / "IncrementalCompilationTest";
        std::filesystem::create_directories(testProjectDir);
        
        // Create test project file
        createTestProject();
        
        // Create test source files
        createTestSourceFiles();
        
        // Initialize thread pool
        threadPool = std::make_unique<JobSystem::WorkStealingThreadPool>(4);
        
        // Initialize CoreCLR host
        clrHost = std::make_unique<CoreCLRHost>();
        
        // Create cancellation token
        // TODO: Initialize cancellation token when available
        // cancellationToken = std::make_shared<CancellationToken>();
    }
    
    void TearDown() override {
        // Clean up test directory
        std::error_code ec;
        std::filesystem::remove_all(testProjectDir, ec);
        
        // Shutdown thread pool
        if (threadPool) {
            threadPool->Shutdown();
        }
    }
    
    void createTestProject() {
        std::filesystem::path projectFile = testProjectDir / "TestProject.csproj";
        std::ofstream file(projectFile);
        file << R"(
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <OutputType>Library</OutputType>
    <EnableDynamicLoading>true</EnableDynamicLoading>
  </PropertyGroup>
</Project>
)";
        file.close();
        
        projectPath = projectFile.string();
    }
    
    void createTestSourceFiles() {
        // Create multiple source files to test incremental compilation
        for (int i = 1; i <= 5; ++i) {
            std::filesystem::path sourceFile = testProjectDir / ("TestClass" + std::to_string(i) + ".cs");
            std::ofstream file(sourceFile);
            file << "using System;\n";
            file << "namespace TestProject {\n";
            file << "    public class TestClass" << i << " {\n";
            file << "        public static int GetValue() { return " << i << "; }\n";
            file << "        public static void DoWork() {\n";
            file << "            Console.WriteLine(\"Working in TestClass" << i << "\");\n";
            file << "        }\n";
            file << "    }\n";
            file << "}\n";
            file.close();
        }
    }
    
    void modifySourceFile(int fileNumber) {
        std::filesystem::path sourceFile = testProjectDir / ("TestClass" + std::to_string(fileNumber) + ".cs");
        std::ofstream file(sourceFile);
        file << "using System;\n";
        file << "namespace TestProject {\n";
        file << "    public class TestClass" << fileNumber << " {\n";
        file << "        public static int GetValue() { return " << (fileNumber * 10) << "; }\n";
        file << "        public static void DoWork() {\n";
        file << "            Console.WriteLine(\"Modified work in TestClass" << fileNumber << "\");\n";
        file << "            Console.WriteLine(\"Additional line for change detection\");\n";
        file << "        }\n";
        file << "    }\n";
        file << "}\n";
        file.close();
    }

protected:
    std::filesystem::path testProjectDir;
    std::string projectPath;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> threadPool;
    std::unique_ptr<CoreCLRHost> clrHost;
    // TODO: Add CancellationToken when implemented
    // std::shared_ptr<CancellationToken> cancellationToken;
};

TEST_F(IncrementalCompilationTest, BasicIncrementalCompilation) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    // Create compilation configuration
    IncrementalCompilationTask::CompilationConfig config;
    config.ProjectPath = projectPath;
    config.SourceDirectory = testProjectDir.string();
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Auto;
    config.EnableCaching = true;
    config.EnableDependencyAnalysis = true;
    config.MaxIncrementalFiles = 3;
    
    // Create incremental compilation task
    auto task = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    ASSERT_NE(task, nullptr);
    
    // Execute initial full compilation
    Logger::Log::Info("=== Performing Initial Full Compilation ===");
    auto startTime = std::chrono::high_resolution_clock::now();
    
    task->Execute();
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto fullCompilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Check that initial compilation succeeded
    EXPECT_TRUE(task->IsSuccessful()) << "Initial compilation should succeed";
    EXPECT_EQ(task->GetStrategyUsed(), IncrementalCompilationTask::CompilationStrategy::Full)
        << "Initial compilation should use full strategy";
    
    Logger::Log::Info("Full compilation time: {}ms", fullCompilationTime.count());
    
    // Modify one source file
    Logger::Log::Info("=== Modifying Source File ===");
    modifySourceFile(1);
    
    // Wait a bit to ensure file timestamp changes
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Create new task for incremental compilation
    auto incrementalTask = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    // Execute incremental compilation
    Logger::Log::Info("=== Performing Incremental Compilation ===");
    startTime = std::chrono::high_resolution_clock::now();
    
    incrementalTask->Execute();
    
    endTime = std::chrono::high_resolution_clock::now();
    auto incrementalCompilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Check that incremental compilation succeeded
    EXPECT_TRUE(incrementalTask->IsSuccessful()) << "Incremental compilation should succeed";
    
    Logger::Log::Info("Incremental compilation time: {}ms", incrementalCompilationTime.count());
    Logger::Log::Info("Strategy used: {}", 
                incrementalTask->GetStrategyUsed() == IncrementalCompilationTask::CompilationStrategy::Incremental ?
                "Incremental" : "Full");
    
    // Performance validation
    if (incrementalTask->GetStrategyUsed() == IncrementalCompilationTask::CompilationStrategy::Incremental) {
        // Incremental compilation should be significantly faster
        EXPECT_LT(incrementalCompilationTime.count(), 50) 
            << "Incremental compilation should be under 50ms (target: <15ms)";
        
        // Should be at least 10x faster than full compilation
        if (fullCompilationTime.count() > 0) {
            double speedupRatio = static_cast<double>(fullCompilationTime.count()) / incrementalCompilationTime.count();
            EXPECT_GT(speedupRatio, 5.0) 
                << "Incremental compilation should be at least 5x faster than full compilation";
            
            Logger::Log::Info("Performance improvement: {:.1f}x faster", speedupRatio);
        }
    }
    
    // Validate compilation result
    const auto& result = incrementalTask->GetResult();
    EXPECT_TRUE(result.Success) << "Compilation should succeed";
    EXPECT_GT(result.AssemblyBytes.size(), 0) << "Should produce assembly bytes";
    EXPECT_EQ(result.Errors.size(), 0) << "Should have no compilation errors";
    
    Logger::Log::Info("=== Performance Metrics ===");
    Logger::Log::Info("Change Detection: {}ms", result.ChangeDetectionTime.count());
    Logger::Log::Info("Dependency Analysis: {}ms", result.DependencyAnalysisTime.count());
    Logger::Log::Info("Roslyn Compilation: {}ms", result.RoslynCompilationTime.count());
    Logger::Log::Info("Files Compiled: {}", result.FilesCompiled);
    Logger::Log::Info("Assembly Size: {} bytes", result.AssemblyBytes.size());
}

TEST_F(IncrementalCompilationTest, FallbackToFullCompilation) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    // Create configuration that forces full compilation
    IncrementalCompilationTask::CompilationConfig config;
    config.ProjectPath = projectPath;
    config.SourceDirectory = testProjectDir.string();
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Auto;
    config.EnableCaching = true;
    config.EnableDependencyAnalysis = true;
    config.MaxIncrementalFiles = 1; // Very low threshold to force full compilation
    
    // Create task
    auto task = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    // Modify multiple files to exceed threshold
    modifySourceFile(1);
    modifySourceFile(2);
    modifySourceFile(3);
    
    // Execute compilation
    task->Execute();
    
    // Should fall back to full compilation due to too many changed files
    EXPECT_TRUE(task->IsSuccessful()) << "Compilation should succeed";
    EXPECT_EQ(task->GetStrategyUsed(), IncrementalCompilationTask::CompilationStrategy::Full)
        << "Should fall back to full compilation when too many files changed";
}

TEST_F(IncrementalCompilationTest, CacheManagement) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    // Create configuration
    IncrementalCompilationTask::CompilationConfig config;
    config.ProjectPath = projectPath;
    config.SourceDirectory = testProjectDir.string();
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Auto;
    config.EnableCaching = true;
    
    // Create task
    auto task = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    // Test cache clearing
    task->ClearCache();

    // Test cache stats
    std::string stats = task->GetCacheStats();
    EXPECT_FALSE(stats.empty()) << "Cache stats should return some information";
    
    Logger::Log::Info("Cache stats: {}", stats);
}
