#include <gtest/gtest.h>
#include "Jobs/IncrementalCompilationTask.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/CoreCLRHost.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>

using namespace GameEngine;

class HybridCompilationBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);

        testProjectDir = std::filesystem::temp_directory_path() / "HybridCompilationBenchmark";
        std::filesystem::create_directories(testProjectDir);

        createBenchmarkProject();
        createLargeCodebase();

        threadPool = std::make_unique<JobSystem::WorkStealingThreadPool>(8); // More threads for benchmark
        clrHost = std::make_unique<GameEngine::CoreCLRHost>();
        // TODO: cancellationToken = std::make_shared<GameEngine::CancellationToken>(); // Not implemented yet
        
        Logger::Log::Info("=== Hybrid Compilation Benchmark Setup Complete ===");
        Logger::Log::Info("Created {} source files for benchmarking", sourceFileCount);
    }
    
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testProjectDir, ec);
        
        if (threadPool) {
            threadPool->Shutdown();
        }
    }
    
    void createBenchmarkProject() {
        std::filesystem::path projectFile = testProjectDir / "BenchmarkProject.csproj";
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
    
    void createLargeCodebase() {
        sourceFileCount = 0;
        
        // Create base interfaces and abstract classes
        createComplexInterface("IGameObject", {"Initialize", "Update", "Render", "Cleanup"});
        createComplexInterface("IComponent", {"Attach", "Detach", "Process"});
        createComplexInterface("ISystem", {"Start", "Stop", "Tick"});
        
        // Create abstract base classes
        createAbstractClass("GameObject", "IGameObject", 5);
        createAbstractClass("Component", "IComponent", 3);
        createAbstractClass("System", "ISystem", 4);
        
        // Create concrete implementations
        for (int i = 1; i <= 25; ++i) {
            createConcreteClass("GameObject" + std::to_string(i), "GameObject", i);
            createConcreteClass("Component" + std::to_string(i), "Component", i);
            createConcreteClass("System" + std::to_string(i), "System", i);
        }
        
        // Create manager classes with complex dependencies
        for (int i = 1; i <= 10; ++i) {
            createManagerClass("Manager" + std::to_string(i), i);
        }
        
        // Create utility classes
        for (int i = 1; i <= 15; ++i) {
            createUtilityClass("Utility" + std::to_string(i), i);
        }
        
        // Create main application class
        createApplicationClass();
        
        Logger::Log::Info("Created large codebase with {} source files", sourceFileCount);
    }
    
    void createComplexInterface(const std::string& name, const std::vector<std::string>& methods) {
        std::string content = "using System;\nnamespace BenchmarkProject {\n";
        content += "    public interface " + name + " {\n";
        
        for (const auto& method : methods) {
            content += "        void " + method + "();\n";
            content += "        void " + method + "Async();\n";
        }
        
        content += "        bool IsActive { get; set; }\n";
        content += "        string Name { get; }\n";
        content += "    }\n}\n";
        
        createSourceFile(name + ".cs", content);
    }
    
    void createAbstractClass(const std::string& name, const std::string& interface, int methodCount) {
        std::string content = "using System;\nusing System.Threading.Tasks;\n";
        content += "namespace BenchmarkProject {\n";
        content += "    public abstract class " + name + " : " + interface + " {\n";
        content += "        protected bool isActive = false;\n";
        content += "        protected string name = \"" + name + "\";\n";
        content += "        \n";
        content += "        public virtual bool IsActive { get => isActive; set => isActive = value; }\n";
        content += "        public virtual string Name => name;\n";
        content += "        \n";
        
        // Add abstract methods
        for (int i = 0; i < methodCount; ++i) {
            content += "        public abstract void Method" + std::to_string(i) + "();\n";
        }
        
        // Add virtual methods
        content += "        public virtual void Initialize() { isActive = true; }\n";
        content += "        public virtual void InitializeAsync() { Task.Run(() => Initialize()); }\n";
        content += "        public virtual void Cleanup() { isActive = false; }\n";
        content += "        public virtual void CleanupAsync() { Task.Run(() => Cleanup()); }\n";
        
        content += "    }\n}\n";
        
        createSourceFile(name + ".cs", content);
    }
    
    void createConcreteClass(const std::string& name, const std::string& baseClass, int id) {
        std::string content = "using System;\nusing System.Collections.Generic;\n";
        content += "namespace BenchmarkProject {\n";
        content += "    public class " + name + " : " + baseClass + " {\n";
        content += "        private int id = " + std::to_string(id) + ";\n";
        content += "        private List<string> data = new List<string>();\n";
        content += "        private Dictionary<string, object> properties = new Dictionary<string, object>();\n";
        content += "        \n";
        content += "        public " + name + "() {\n";
        content += "            name = \"" + name + "_\" + id;\n";
        content += "            InitializeData();\n";
        content += "        }\n";
        content += "        \n";
        content += "        private void InitializeData() {\n";
        content += "            for (int i = 0; i < 10; i++) {\n";
        content += "                data.Add($\"Data_{id}_{i}\");\n";
        content += "                properties[$\"Property_{i}\"] = i * id;\n";
        content += "            }\n";
        content += "        }\n";
        content += "        \n";
        
        // Implement abstract methods
        for (int i = 0; i < 5; ++i) {
            content += "        public override void Method" + std::to_string(i) + "() {\n";
            content += "            Console.WriteLine($\"" + name + ".Method" + std::to_string(i) + " called\");\n";
            content += "            ProcessData();\n";
            content += "        }\n";
        }
        
        content += "        \n";
        content += "        private void ProcessData() {\n";
        content += "            foreach (var item in data) {\n";
        content += "                properties[item] = DateTime.Now.Ticks;\n";
        content += "            }\n";
        content += "        }\n";
        content += "        \n";
        content += "        public int Id => id;\n";
        content += "        public int DataCount => data.Count;\n";
        content += "    }\n}\n";
        
        createSourceFile(name + ".cs", content);
    }
    
    void createManagerClass(const std::string& name, int /*id*/) {
        std::string content = "using System;\nusing System.Collections.Generic;\nusing System.Linq;\n";
        content += "namespace BenchmarkProject {\n";
        content += "    public class " + name + " {\n";
        content += "        private List<IGameObject> gameObjects = new List<IGameObject>();\n";
        content += "        private List<IComponent> components = new List<IComponent>();\n";
        content += "        private List<ISystem> systems = new List<ISystem>();\n";
        content += "        private bool isRunning = false;\n";
        content += "        \n";
        content += "        public void Initialize() {\n";
        content += "            // Create game objects\n";
        content += "            for (int i = 1; i <= 5; i++) {\n";
        content += "                var goType = Type.GetType($\"BenchmarkProject.GameObject{i}\");\n";
        content += "                if (goType != null) {\n";
        content += "                    var go = Activator.CreateInstance(goType) as IGameObject;\n";
        content += "                    if (go != null) gameObjects.Add(go);\n";
        content += "                }\n";
        content += "            }\n";
        content += "            \n";
        content += "            // Initialize all objects\n";
        content += "            foreach (var go in gameObjects) go.Initialize();\n";
        content += "            foreach (var comp in components) comp.Attach();\n";
        content += "            foreach (var sys in systems) sys.Start();\n";
        content += "            \n";
        content += "            isRunning = true;\n";
        content += "        }\n";
        content += "        \n";
        content += "        public void Update() {\n";
        content += "            if (!isRunning) return;\n";
        content += "            \n";
        content += "            foreach (var go in gameObjects) go.Update();\n";
        content += "            foreach (var comp in components) comp.Process();\n";
        content += "            foreach (var sys in systems) sys.Tick();\n";
        content += "        }\n";
        content += "        \n";
        content += "        public void Shutdown() {\n";
        content += "            isRunning = false;\n";
        content += "            \n";
        content += "            foreach (var sys in systems) sys.Stop();\n";
        content += "            foreach (var comp in components) comp.Detach();\n";
        content += "            foreach (var go in gameObjects) go.Cleanup();\n";
        content += "        }\n";
        content += "        \n";
        content += "        public int ObjectCount => gameObjects.Count;\n";
        content += "        public bool IsRunning => isRunning;\n";
        content += "    }\n}\n";
        
        createSourceFile(name + ".cs", content);
    }
    
    void createUtilityClass(const std::string& name, int /*id*/) {
        std::string content = "using System;\nusing System.Collections.Generic;\nusing System.Linq;\n";
        content += "namespace BenchmarkProject {\n";
        content += "    public static class " + name + " {\n";
        content += "        private static Dictionary<string, object> cache = new Dictionary<string, object>();\n";
        content += "        \n";
        content += "        public static T GetOrCreate<T>(string key, Func<T> factory) {\n";
        content += "            if (cache.TryGetValue(key, out var cached)) {\n";
        content += "                return (T)cached;\n";
        content += "            }\n";
        content += "            \n";
        content += "            var value = factory();\n";
        content += "            cache[key] = value;\n";
        content += "            return value;\n";
        content += "        }\n";
        content += "        \n";
        content += "        public static void ClearCache() => cache.Clear();\n";
        content += "        \n";
        content += "        public static int CacheSize => cache.Count;\n";
        content += "        \n";
        content += "        public static string ProcessData(string input) {\n";
        content += "            return input?.ToUpper().Replace(\" \", \"_\") ?? \"\";\n";
        content += "        }\n";
        content += "        \n";
        content += "        public static List<T> FilterAndSort<T>(IEnumerable<T> items, Func<T, bool> filter, Func<T, object> keySelector) {\n";
        content += "            return items.Where(filter).OrderBy(keySelector).ToList();\n";
        content += "        }\n";
        content += "    }\n}\n";
        
        createSourceFile(name + ".cs", content);
    }
    
    void createApplicationClass() {
        std::string content = R"(
using System;
using System.Collections.Generic;
using System.Linq;

namespace BenchmarkProject {
    public class Application {
        private List<Manager1> managers = new List<Manager1>();
        private bool isInitialized = false;
        private DateTime startTime;
        
        public void Initialize() {
            if (isInitialized) return;
            
            startTime = DateTime.Now;
            
            // Create and initialize managers
            for (int i = 1; i <= 5; i++) {
                var managerType = Type.GetType($"BenchmarkProject.Manager{i}");
                if (managerType != null) {
                    var manager = Activator.CreateInstance(managerType) as Manager1;
                    if (manager != null) {
                        managers.Add(manager);
                        manager.Initialize();
                    }
                }
            }
            
            isInitialized = true;
            Console.WriteLine($"Application initialized with {managers.Count} managers");
        }
        
        public void Run() {
            if (!isInitialized) Initialize();
            
            var frameCount = 0;
            var lastTime = DateTime.Now;
            
            while (frameCount < 100) {
                var currentTime = DateTime.Now;
                var deltaTime = (currentTime - lastTime).TotalSeconds;
                
                foreach (var manager in managers) {
                    manager.Update();
                }
                
                frameCount++;
                lastTime = currentTime;
                
                if (frameCount % 10 == 0) {
                    Console.WriteLine($"Frame {frameCount}, Delta: {deltaTime:F4}s");
                }
            }
        }
        
        public void Shutdown() {
            if (!isInitialized) return;
            
            foreach (var manager in managers) {
                manager.Shutdown();
            }
            
            managers.Clear();
            isInitialized = false;
            
            var totalTime = DateTime.Now - startTime;
            Console.WriteLine($"Application shutdown after {totalTime.TotalSeconds:F2} seconds");
        }
        
        public int ManagerCount => managers.Count;
        public bool IsInitialized => isInitialized;
        public TimeSpan RunTime => DateTime.Now - startTime;
    }
}
)";
        
        createSourceFile("Application.cs", content);
    }
    
    void createSourceFile(const std::string& filename, const std::string& content) {
        std::filesystem::path filePath = testProjectDir / filename;
        std::ofstream file(filePath);
        file << content;
        file.close();
        sourceFileCount++;
    }
    
    std::vector<std::string> getRandomSourceFiles(int count) {
        std::vector<std::string> allFiles;
        for (const auto& entry : std::filesystem::directory_iterator(testProjectDir)) {
            if (entry.path().extension() == ".cs") {
                allFiles.push_back(entry.path().filename().string());
            }
        }
        
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(allFiles.begin(), allFiles.end(), g);
        
        count = std::min(count, static_cast<int>(allFiles.size()));
        return std::vector<std::string>(allFiles.begin(), allFiles.begin() + count);
    }
    
    void modifyRandomFiles(int count) {
        auto files = getRandomSourceFiles(count);
        for (const auto& file : files) {
            std::filesystem::path filePath = testProjectDir / file;
            std::ofstream appendFile(filePath, std::ios::app);
            appendFile << "\n// Benchmark modification: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
            appendFile.close();
        }
        
        // Ensure file timestamps change
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

protected:
    std::filesystem::path testProjectDir;
    std::string projectPath;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> threadPool;
    std::unique_ptr<GameEngine::CoreCLRHost> clrHost;
    // TODO: std::shared_ptr<GameEngine::CancellationToken> cancellationToken; // Not implemented yet
    int sourceFileCount = 0;
};

TEST_F(HybridCompilationBenchmark, LargeCodebasePerformance) {
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for benchmarking";
    }
    
    Logger::Log::Info("=== Large Codebase Performance Benchmark ===");
    Logger::Log::Info("Testing with {} source files", sourceFileCount);
    
    IncrementalCompilationTask::CompilationConfig config;
    config.ProjectPath = projectPath;
    config.SourceDirectory = testProjectDir.string();
    config.EnableCaching = true;
    config.EnableDependencyAnalysis = true;
    config.MaxIncrementalFiles = 10;
    
    // Benchmark 1: Initial full compilation
    Logger::Log::Info("Benchmark 1: Initial full compilation");
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Full;
    
    auto task = GameEngine::CreateIncrementalCompilationTask(threadPool.get(), *clrHost, config);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    task->Execute();
    auto endTime = std::chrono::high_resolution_clock::now();
    
    auto fullCompilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    EXPECT_TRUE(task->IsSuccessful()) << "Full compilation should succeed";
    Logger::Log::Info("Full compilation: {}ms", fullCompilationTime.count());
    
    // Benchmark 2: Single file incremental compilation
    Logger::Log::Info("Benchmark 2: Single file incremental compilation");
    modifyRandomFiles(1);
    
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Auto;
    auto incrementalTask = GameEngine::CreateIncrementalCompilationTask(threadPool.get(), *clrHost, config);
    
    startTime = std::chrono::high_resolution_clock::now();
    incrementalTask->Execute();
    endTime = std::chrono::high_resolution_clock::now();
    
    auto singleFileTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    EXPECT_TRUE(incrementalTask->IsSuccessful()) << "Single file incremental compilation should succeed";
    Logger::Log::Info("Single file incremental: {}ms", singleFileTime.count());
    
    // Benchmark 3: Multiple file incremental compilation
    Logger::Log::Info("Benchmark 3: Multiple file incremental compilation");
    modifyRandomFiles(5);
    
    auto multiFileTask = GameEngine::CreateIncrementalCompilationTask(threadPool.get(), *clrHost, config);
    
    startTime = std::chrono::high_resolution_clock::now();
    multiFileTask->Execute();
    endTime = std::chrono::high_resolution_clock::now();
    
    auto multiFileTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    EXPECT_TRUE(multiFileTask->IsSuccessful()) << "Multi-file incremental compilation should succeed";
    Logger::Log::Info("Multi-file incremental: {}ms", multiFileTime.count());
    
    // Performance analysis
    Logger::Log::Info("=== Performance Analysis ===");
    
    if (fullCompilationTime.count() > 0) {
        double singleFileSpeedup = static_cast<double>(fullCompilationTime.count()) / singleFileTime.count();
        double multiFileSpeedup = static_cast<double>(fullCompilationTime.count()) / multiFileTime.count();
        
        Logger::Log::Info("Single file speedup: {:.1f}x", singleFileSpeedup);
        Logger::Log::Info("Multi-file speedup: {:.1f}x", multiFileSpeedup);
        
        // Performance expectations for large codebase
        EXPECT_GT(singleFileSpeedup, 10.0) << "Single file incremental should be at least 10x faster";
        EXPECT_GT(multiFileSpeedup, 5.0) << "Multi-file incremental should be at least 5x faster";
        
        // Absolute performance targets
        EXPECT_LT(singleFileTime.count(), 200) << "Single file incremental should be under 200ms";
        EXPECT_LT(multiFileTime.count(), 500) << "Multi-file incremental should be under 500ms";
    }
    
    Logger::Log::Info("Benchmark completed successfully");
}

TEST_F(HybridCompilationBenchmark, ScalabilityTest) {
    if (!clrHost->IsInitialized()) {
        GTEST_SKIP() << "CoreCLR not available for benchmarking";
    }
    
    Logger::Log::Info("=== Scalability Test ===");
    
    IncrementalCompilationTask::CompilationConfig config;
    config.ProjectPath = projectPath;
    config.SourceDirectory = testProjectDir.string();
    config.Strategy = IncrementalCompilationTask::CompilationStrategy::Auto;
    config.EnableCaching = true;
    config.EnableDependencyAnalysis = true;
    
    // Test with increasing numbers of modified files
    std::vector<int> fileCounts = {1, 3, 5, 10, 15, 20};
    std::vector<std::chrono::milliseconds> compilationTimes;
    
    for (int fileCount : fileCounts) {
        Logger::Log::Info("Testing with {} modified files", fileCount);
        
        modifyRandomFiles(fileCount);
        
        auto task = GameEngine::CreateIncrementalCompilationTask(threadPool.get(), *clrHost, config);
        
        auto startTime = std::chrono::high_resolution_clock::now();
        task->Execute();
        auto endTime = std::chrono::high_resolution_clock::now();
        
        auto compilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        compilationTimes.push_back(compilationTime);
        
        EXPECT_TRUE(task->IsSuccessful()) << "Compilation with " << fileCount << " files should succeed";
        Logger::Log::Info("  Compilation time: {}ms", compilationTime.count());
    }
    
    // Analyze scalability
    Logger::Log::Info("=== Scalability Analysis ===");
    
    for (size_t i = 0; i < fileCounts.size(); ++i) {
        Logger::Log::Info("Files: {}, Time: {}ms", fileCounts[i], compilationTimes[i].count());
    }
    
    // Check that compilation time doesn't grow exponentially
    if (compilationTimes.size() >= 2) {
        auto firstTime = compilationTimes[0].count();
        auto lastTime = compilationTimes.back().count();
        auto firstFileCount = fileCounts[0];
        auto lastFileCount = fileCounts.back();
        
        if (firstTime > 0) {
            double timeRatio = static_cast<double>(lastTime) / firstTime;
            double fileRatio = static_cast<double>(lastFileCount) / firstFileCount;
            
            Logger::Log::Info("Time scaling factor: {:.2f} (files scaled by {:.2f})", timeRatio, fileRatio);
            
            // Time should scale sub-linearly with file count
            EXPECT_LT(timeRatio, fileRatio * 1.5) << "Compilation time should scale sub-linearly with file count";
        }
    }
}
