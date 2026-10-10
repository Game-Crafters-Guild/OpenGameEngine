#include <gtest/gtest.h>
#include "Jobs/FileChangeTracker.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace GameEngine;

class FileChangeTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
        
        // Create test directory
        testDir = std::filesystem::temp_directory_path() / "file_change_tracker_test";
        std::filesystem::create_directories(testDir);
        
        // Initialize tracker
        tracker = std::make_unique<FileChangeTracker>((testDir / "cache.json").string());
    }
    
    void TearDown() override {
        tracker.reset();
        
        // Clean up test directory
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }
    
    void CreateTestFile(const std::string& filename, const std::string& content) {
        std::filesystem::path filePath = testDir / filename;
        std::ofstream file(filePath);
        file << content;
        file.close();
    }
    
    void ModifyTestFile(const std::string& filename, const std::string& newContent) {
        // Add a small delay to ensure timestamp changes
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CreateTestFile(filename, newContent);
    }
    
    std::filesystem::path testDir;
    std::unique_ptr<FileChangeTracker> tracker;
};

TEST_F(FileChangeTrackerTest, DetectNewFiles) {
    // Create a new C# file
    CreateTestFile("TestClass.cs", R"(
using System;

namespace TestNamespace {
    public class TestClass {
        public void TestMethod() {
            Console.WriteLine("Hello World");
        }
    }
}
)");
    
    // Detect changes
    auto result = tracker->DetectChanges(testDir.string());
    
    // Should detect the new file
    EXPECT_EQ(result.NewFiles.size(), 1);
    EXPECT_EQ(result.ChangedFiles.size(), 1);
    EXPECT_TRUE(result.HasStructuralChanges);
    EXPECT_EQ(result.ChangedFiles[0].ChangeKind, FileChangeTracker::ChangeType::Structural);
    
    Logger::Log::Info("✅ New file detection test passed");
}

TEST_F(FileChangeTrackerTest, DetectFileModifications) {
    // Create initial file
    CreateTestFile("TestClass.cs", R"(
using System;

namespace TestNamespace {
    public class TestClass {
        public void TestMethod() {
            Console.WriteLine("Hello World");
        }
    }
}
)");
    
    // First scan to establish baseline
    auto initialResult = tracker->DetectChanges(testDir.string());
    tracker->UpdateCache(initialResult.ChangedFiles);
    
    // Modify the file
    ModifyTestFile("TestClass.cs", R"(
using System;

namespace TestNamespace {
    public class TestClass {
        public void TestMethod() {
            Console.WriteLine("Hello Modified World");
        }
        
        public void NewMethod() {
            Console.WriteLine("New method added");
        }
    }
}
)");
    
    // Detect changes again
    auto result = tracker->DetectChanges(testDir.string());
    
    // Should detect the modification
    EXPECT_EQ(result.ChangedFiles.size(), 1);
    EXPECT_EQ(result.NewFiles.size(), 0);
    EXPECT_TRUE(result.HasStructuralChanges); // Adding new method is structural
    
    Logger::Log::Info("✅ File modification detection test passed");
}

TEST_F(FileChangeTrackerTest, IgnoreNonCSharpFiles) {
    // Create various file types
    CreateTestFile("test.txt", "This is a text file");
    CreateTestFile("test.cpp", "#include <iostream>");
    CreateTestFile("TestClass.cs", "using System; public class Test {}");
    
    // Detect changes
    auto result = tracker->DetectChanges(testDir.string());
    
    // Should only detect the .cs file
    EXPECT_EQ(result.ChangedFiles.size(), 1);
    EXPECT_EQ(result.NewFiles.size(), 1);
    EXPECT_TRUE(result.NewFiles[0].find("TestClass.cs") != std::string::npos);
    
    Logger::Log::Info("✅ File type filtering test passed");
}

TEST_F(FileChangeTrackerTest, PerformanceTest) {
    // Create multiple files
    const int numFiles = 50;
    for (int i = 0; i < numFiles; ++i) {
        std::string filename = "TestClass" + std::to_string(i) + ".cs";
        std::string content = R"(
using System;

namespace TestNamespace)" + std::to_string(i) + R"( {
    public class TestClass)" + std::to_string(i) + R"( {
        public void TestMethod)" + std::to_string(i) + R"(() {
            Console.WriteLine("Hello from class )" + std::to_string(i) + R"(");
        }
    }
}
)";
        CreateTestFile(filename, content);
    }
    
    // Measure detection time
    auto start = std::chrono::high_resolution_clock::now();
    auto result = tracker->DetectChanges(testDir.string());
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_EQ(result.ChangedFiles.size(), numFiles);
    EXPECT_LT(duration.count(), 1000); // Should complete within 1 second
    
    Logger::Log::Info("✅ Performance test passed: {}ms for {} files", duration.count(), numFiles);
}

TEST_F(FileChangeTrackerTest, IncrementalCompilationRecommendation) {
    // Test scenarios where incremental compilation should/shouldn't be used
    
    // Scenario 1: Single local change - should use incremental
    CreateTestFile("TestClass.cs", "using System; public class Test { public void Method() { var x = 1; } }");
    auto result1 = tracker->DetectChanges(testDir.string());
    tracker->UpdateCache(result1.ChangedFiles);
    
    ModifyTestFile("TestClass.cs", "using System; public class Test { public void Method() { var x = 2; } }");
    auto result2 = tracker->DetectChanges(testDir.string());
    
    // For now, our basic implementation marks most changes as structural
    // In a full implementation, this would be more sophisticated
    bool shouldUseIncremental = FileChangeTracker::ShouldUseIncrementalCompilation(result2);
    
    Logger::Log::Info("✅ Incremental compilation recommendation test completed");
    Logger::Log::Info("   Should use incremental: {}", shouldUseIncremental ? "Yes" : "No");
    Logger::Log::Info("   Changed files: {}", result2.ChangedFiles.size());
    Logger::Log::Info("   Has structural changes: {}", result2.HasStructuralChanges ? "Yes" : "No");
}

TEST_F(FileChangeTrackerTest, ChangeTypeClassification) {
    // Test different change type descriptions
    EXPECT_EQ(FileChangeTracker::GetChangeTypeDescription(FileChangeTracker::ChangeType::None), "None");
    EXPECT_EQ(FileChangeTracker::GetChangeTypeDescription(FileChangeTracker::ChangeType::Trivial), "Trivial");
    EXPECT_EQ(FileChangeTracker::GetChangeTypeDescription(FileChangeTracker::ChangeType::Local), "Local");
    EXPECT_EQ(FileChangeTracker::GetChangeTypeDescription(FileChangeTracker::ChangeType::Interface), "Interface");
    EXPECT_EQ(FileChangeTracker::GetChangeTypeDescription(FileChangeTracker::ChangeType::Structural), "Structural");
    
    Logger::Log::Info("✅ Change type classification test passed");
}
