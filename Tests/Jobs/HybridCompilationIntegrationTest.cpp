#include <gtest/gtest.h>
#include "Jobs/IncrementalCompilationTask.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/CoreCLRHost.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

using namespace GameEngine;

class HybridCompilationIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize logger
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
        
        // Create test project directory
        testProjectDir = std::filesystem::temp_directory_path() / "HybridCompilationTest";
        std::filesystem::create_directories(testProjectDir);
        
        // Create test project file
        createTestProject();
        
        // Create complex test source files
        createComplexTestSourceFiles();
        
        // Initialize thread pool
        threadPool = std::make_unique<JobSystem::WorkStealingThreadPool>(4);

        // Initialize CoreCLR host
        clrHost = std::make_unique<CoreCLRHost>();
        
        Logger::Log::Info("=== Hybrid Compilation Integration Test Setup Complete ===");
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
        std::filesystem::path projectFile = testProjectDir / "HybridTestProject.csproj";
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
    
    void createComplexTestSourceFiles() {
        // Create base interfaces and classes
        createSourceFile("IComponent.cs", R"(
using System;
namespace HybridTestProject {
    public interface IComponent {
        void Initialize();
        void Update(float deltaTime);
        void Cleanup();
    }
}
)");
        
        createSourceFile("BaseComponent.cs", R"(
using System;
namespace HybridTestProject {
    public abstract class BaseComponent : IComponent {
        protected bool isInitialized = false;
        
        public virtual void Initialize() {
            isInitialized = true;
            Console.WriteLine($"{GetType().Name} initialized");
        }
        
        public abstract void Update(float deltaTime);
        
        public virtual void Cleanup() {
            isInitialized = false;
            Console.WriteLine($"{GetType().Name} cleaned up");
        }
    }
}
)");
        
        // Create multiple component implementations
        for (int i = 1; i <= 10; ++i) {
            std::string className = "Component" + std::to_string(i);
            std::string content = "using System;\n";
            content += "namespace HybridTestProject {\n";
            content += "    public class " + className + " : BaseComponent {\n";
            content += "        private float value = " + std::to_string(i * 10.0f) + "f;\n";
            content += "        private int updateCount = 0;\n";
            content += "        \n";
            content += "        public override void Update(float deltaTime) {\n";
            content += "            if (!isInitialized) return;\n";
            content += "            value += deltaTime * " + std::to_string(i) + ";\n";
            content += "            updateCount++;\n";
            content += "            if (updateCount % 100 == 0) {\n";
            content += "                Console.WriteLine($\"" + className + " updated {updateCount} times, value: {value:F2}\");\n";
            content += "            }\n";
            content += "        }\n";
            content += "        \n";
            content += "        public float GetValue() => value;\n";
            content += "        public int GetUpdateCount() => updateCount;\n";
            content += "    }\n";
            content += "}\n";
            
            createSourceFile(className + ".cs", content);
        }
        
        // Create manager classes with dependencies
        for (int i = 1; i <= 3; ++i) {
            std::string className = "Manager" + std::to_string(i);
            std::string content = "using System;\n";
            content += "using System.Collections.Generic;\n";
            content += "using System.Linq;\n";
            content += "namespace HybridTestProject {\n";
            content += "    public class " + className + " {\n";
            content += "        private List<IComponent> components = new List<IComponent>();\n";
            content += "        private bool isRunning = false;\n";
            content += "        \n";
            content += "        public void AddComponent(IComponent component) {\n";
            content += "            components.Add(component);\n";
            content += "            if (isRunning) component.Initialize();\n";
            content += "        }\n";
            content += "        \n";
            content += "        public void Start() {\n";
            content += "            isRunning = true;\n";
            content += "            foreach (var component in components) {\n";
            content += "                component.Initialize();\n";
            content += "            }\n";
            content += "            Console.WriteLine($\"" + className + " started with {components.Count} components\");\n";
            content += "        }\n";
            content += "        \n";
            content += "        public void UpdateAll(float deltaTime) {\n";
            content += "            if (!isRunning) return;\n";
            content += "            foreach (var component in components) {\n";
            content += "                component.Update(deltaTime);\n";
            content += "            }\n";
            content += "        }\n";
            content += "        \n";
            content += "        public void Stop() {\n";
            content += "            isRunning = false;\n";
            content += "            foreach (var component in components) {\n";
            content += "                component.Cleanup();\n";
            content += "            }\n";
            content += "            Console.WriteLine($\"" + className + " stopped\");\n";
            content += "        }\n";
            content += "        \n";
            content += "        public int ComponentCount => components.Count;\n";
            content += "    }\n";
            content += "}\n";
            
            createSourceFile(className + ".cs", content);
        }
        
        // Create main game system
        createSourceFile("GameSystem.cs", R"(
using System;
using System.Collections.Generic;
namespace HybridTestProject {
    public static class GameSystem {
        private static List<Manager1> managers = new List<Manager1>();
        private static bool isInitialized = false;
        
        public static void Initialize() {
            if (isInitialized) return;
            
            // Create managers and components
            for (int i = 0; i < 3; i++) {
                var manager = new Manager1();
                
                // Add components to manager
                for (int j = 1; j <= 5; j++) {
                    var componentType = Type.GetType($"HybridTestProject.Component{j}");
                    if (componentType != null) {
                        var component = Activator.CreateInstance(componentType) as IComponent;
                        if (component != null) {
                            manager.AddComponent(component);
                        }
                    }
                }
                
                managers.Add(manager);
                manager.Start();
            }
            
            isInitialized = true;
            Console.WriteLine("GameSystem initialized with " + managers.Count + " managers");
        }
        
        public static void Update(float deltaTime) {
            if (!isInitialized) return;
            
            foreach (var manager in managers) {
                manager.UpdateAll(deltaTime);
            }
        }
        
        public static void Shutdown() {
            if (!isInitialized) return;
            
            foreach (var manager in managers) {
                manager.Stop();
            }
            
            managers.Clear();
            isInitialized = false;
            Console.WriteLine("GameSystem shutdown");
        }
        
        public static int ManagerCount => managers.Count;
    }
}
)");
        
        // Count files in directory
        int fileCount = 0;
        for (const auto& entry : std::filesystem::directory_iterator(testProjectDir)) {
            if (entry.is_regular_file()) {
                fileCount++;
            }
        }
        Logger::Log::Info("Created {} source files for hybrid compilation testing", fileCount - 1); // -1 for project file
    }
    
    void createSourceFile(const std::string& filename, const std::string& content) {
        std::filesystem::path filePath = testProjectDir / filename;
        std::ofstream file(filePath);
        file << content;
        file.close();
    }
    
    void modifySourceFile(const std::string& filename, const std::string& modification) {
        std::filesystem::path filePath = testProjectDir / filename;
        std::ifstream readFile(filePath);
        std::string content((std::istreambuf_iterator<char>(readFile)),
                           std::istreambuf_iterator<char>());
        readFile.close();
        
        // Add modification comment
        content += "\n// Modified: " + modification + "\n";
        
        std::ofstream writeFile(filePath);
        writeFile << content;
        writeFile.close();
        
        Logger::Log::Debug("Modified {}: {}", filename, modification);
    }
    
    IncrementalCompilationTask::CompilationConfig createConfig(
        IncrementalCompilationTask::CompilationStrategy strategy = IncrementalCompilationTask::CompilationStrategy::Auto) {
        
        IncrementalCompilationTask::CompilationConfig config;
        config.ProjectPath = projectPath;
        config.SourceDirectory = testProjectDir.string();
        config.Strategy = strategy;
        config.EnableCaching = true;
        config.EnableDependencyAnalysis = true;
        config.MaxIncrementalFiles = 5;
        config.MaxCacheAge = std::chrono::minutes(30);
        
        return config;
    }
    
    std::chrono::milliseconds measureCompilationTime(
        IncrementalCompilationTask::CompilationStrategy strategy) {
        
        auto config = createConfig(strategy);
        auto task = CreateIncrementalCompilationTask(
            threadPool.get(),
            *clrHost,
            config
        );
        
        auto startTime = std::chrono::high_resolution_clock::now();
        task->Execute();
        auto endTime = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        Logger::Log::Info("Compilation with {} strategy: {}ms, Success: {}", 
                    strategy == IncrementalCompilationTask::CompilationStrategy::Auto ? "Auto" :
                    strategy == IncrementalCompilationTask::CompilationStrategy::Incremental ? "Incremental" : "Full",
                    duration.count(),
                    task->IsSuccessful());
        
        return duration;
    }

protected:
    std::filesystem::path testProjectDir;
    std::string projectPath;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> threadPool;
    std::unique_ptr<CoreCLRHost> clrHost;
};

TEST_F(HybridCompilationIntegrationTest, FullWorkflowIntegration) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    Logger::Log::Info("=== Testing Full Hybrid Compilation Workflow ===");
    
    // Step 1: Initial full compilation
    Logger::Log::Info("Step 1: Initial full compilation");
    auto fullTime = measureCompilationTime(IncrementalCompilationTask::CompilationStrategy::Full);
    EXPECT_GT(fullTime.count(), 0) << "Full compilation should take some time";
    
    // Step 2: Small incremental change
    Logger::Log::Info("Step 2: Small incremental change");
    modifySourceFile("Component1.cs", "Added comment for incremental test");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Ensure timestamp change
    
    auto incrementalTime = measureCompilationTime(IncrementalCompilationTask::CompilationStrategy::Auto);
    EXPECT_GT(incrementalTime.count(), 0) << "Incremental compilation should take some time";
    
    // Performance validation
    if (fullTime.count() > 0 && incrementalTime.count() > 0) {
        double speedupRatio = static_cast<double>(fullTime.count()) / incrementalTime.count();
        Logger::Log::Info("Performance improvement: {:.1f}x faster ({} ms -> {} ms)", 
                    speedupRatio, fullTime.count(), incrementalTime.count());
        
        // Expect at least 2x improvement for incremental compilation
        EXPECT_GT(speedupRatio, 2.0) << "Incremental compilation should be significantly faster";
    }
    
    // Step 3: Multiple file changes
    Logger::Log::Info("Step 3: Multiple file changes");
    modifySourceFile("Component2.cs", "Multiple file change test 1");
    modifySourceFile("Component3.cs", "Multiple file change test 2");
    modifySourceFile("Manager1.cs", "Multiple file change test 3");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto multiFileTime = measureCompilationTime(IncrementalCompilationTask::CompilationStrategy::Auto);
    EXPECT_GT(multiFileTime.count(), 0) << "Multi-file compilation should take some time";
    
    Logger::Log::Info("Multi-file compilation time: {}ms", multiFileTime.count());
    
    // Step 4: Cache management test
    Logger::Log::Info("Step 4: Cache management test");
    auto config = createConfig();
    auto task = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    // Test cache operations
    std::string initialStats = task->GetCacheStats();
    EXPECT_FALSE(initialStats.empty()) << "Cache stats should return information";

    task->ClearCache();

    std::string clearedStats = task->GetCacheStats();
    EXPECT_FALSE(clearedStats.empty()) << "Cache stats should still return information after clear";
    
    Logger::Log::Info("Initial cache stats: {}", initialStats);
    Logger::Log::Info("Cleared cache stats: {}", clearedStats);
}

TEST_F(HybridCompilationIntegrationTest, PerformanceTargetValidation) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    Logger::Log::Info("=== Performance Target Validation ===");
    
    // Perform initial full compilation
    auto fullTime = measureCompilationTime(IncrementalCompilationTask::CompilationStrategy::Full);
    
    // Perform multiple incremental compilations and measure average
    std::vector<std::chrono::milliseconds> incrementalTimes;
    
    for (int i = 1; i <= 5; ++i) {
        modifySourceFile("Component" + std::to_string(i) + ".cs", 
                        "Performance test iteration " + std::to_string(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        auto time = measureCompilationTime(IncrementalCompilationTask::CompilationStrategy::Auto);
        incrementalTimes.push_back(time);
    }
    
    // Calculate average incremental time
    auto totalIncrementalTime = std::chrono::milliseconds(0);
    for (const auto& time : incrementalTimes) {
        totalIncrementalTime += time;
    }
    auto avgIncrementalTime = totalIncrementalTime / incrementalTimes.size();
    
    Logger::Log::Info("Performance Results:");
    Logger::Log::Info("  Full compilation: {}ms", fullTime.count());
    Logger::Log::Info("  Average incremental: {}ms", avgIncrementalTime.count());
    Logger::Log::Info("  Performance improvement: {:.1f}x", 
                static_cast<double>(fullTime.count()) / avgIncrementalTime.count());
    
    // Performance targets validation
    EXPECT_LT(avgIncrementalTime.count(), 100) 
        << "Average incremental compilation should be under 100ms (target: <15ms for production)";
    
    if (fullTime.count() > 0) {
        double speedupRatio = static_cast<double>(fullTime.count()) / avgIncrementalTime.count();
        EXPECT_GT(speedupRatio, 5.0) 
            << "Incremental compilation should be at least 5x faster than full compilation";
    }
    
    // Consistency check - incremental times should be relatively consistent
    auto minTime = *std::min_element(incrementalTimes.begin(), incrementalTimes.end());
    auto maxTime = *std::max_element(incrementalTimes.begin(), incrementalTimes.end());
    
    if (minTime.count() > 0) {
        double variationRatio = static_cast<double>(maxTime.count()) / minTime.count();
        EXPECT_LT(variationRatio, 3.0) 
            << "Incremental compilation times should be relatively consistent";
        
        Logger::Log::Info("  Time variation: {:.1f}x ({}ms - {}ms)", 
                    variationRatio, minTime.count(), maxTime.count());
    }
}

TEST_F(HybridCompilationIntegrationTest, CacheSystemValidation) {
    // Skip test if CoreCLR is not available
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for testing";
    }
    
    Logger::Log::Info("=== Cache System Validation ===");
    
    auto config = createConfig();
    auto task = CreateIncrementalCompilationTask(
        threadPool.get(),
        *clrHost,
        config
    );
    
    // Initial compilation to populate cache
    task->Execute();
    EXPECT_TRUE(task->IsSuccessful()) << "Initial compilation should succeed";
    
    std::string initialStats = task->GetCacheStats();
    Logger::Log::Info("Initial cache stats: {}", initialStats);
    
    // Modify a file and recompile
    modifySourceFile("Component1.cs", "Cache validation test");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto startTime = std::chrono::high_resolution_clock::now();
    task->Execute();
    auto endTime = std::chrono::high_resolution_clock::now();
    
    EXPECT_TRUE(task->IsSuccessful()) << "Incremental compilation should succeed";
    
    auto compilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    Logger::Log::Info("Incremental compilation with cache: {}ms", compilationTime.count());
    
    std::string updatedStats = task->GetCacheStats();
    Logger::Log::Info("Updated cache stats: {}", updatedStats);
    
    // Cache should improve performance
    EXPECT_LT(compilationTime.count(), 200) 
        << "Cached incremental compilation should be fast";
    
    // Test cache clearing
    task->ClearCache();
    std::string clearedStats2 = task->GetCacheStats();
    Logger::Log::Info("Cleared cache stats: {}", clearedStats2);
    
    // Compilation after cache clear should still work but might be slower
    modifySourceFile("Component2.cs", "Post-cache-clear test");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    startTime = std::chrono::high_resolution_clock::now();
    task->Execute();
    endTime = std::chrono::high_resolution_clock::now();
    
    EXPECT_TRUE(task->IsSuccessful()) << "Compilation after cache clear should succeed";
    
    auto postClearTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    Logger::Log::Info("Compilation after cache clear: {}ms", postClearTime.count());
}
