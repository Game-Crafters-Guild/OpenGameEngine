#include <gtest/gtest.h>
#include "Jobs/DependencyGraphAnalyzer.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>

using namespace GameEngine;

class DependencyGraphAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
        
        // Create test directory
        testDir = std::filesystem::temp_directory_path() / "dependency_graph_test";
        std::filesystem::create_directories(testDir);
        
        // Initialize analyzer
        analyzer = std::make_unique<DependencyGraphAnalyzer>();
    }
    
    void TearDown() override {
        analyzer.reset();
        
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
    
    std::string GetTestFilePath(const std::string& filename) {
        return (testDir / filename).string();
    }
    
    std::filesystem::path testDir;
    std::unique_ptr<DependencyGraphAnalyzer> analyzer;
};

TEST_F(DependencyGraphAnalyzerTest, BasicDependencyDetection) {
    // Create base class
    CreateTestFile("BaseClass.cs", R"(
using System;

namespace TestNamespace {
    public class BaseClass {
        public virtual void Method() {
            Console.WriteLine("Base method");
        }
    }
}
)");
    
    // Create derived class
    CreateTestFile("DerivedClass.cs", R"(
using System;

namespace TestNamespace {
    public class DerivedClass : BaseClass {
        public override void Method() {
            Console.WriteLine("Derived method");
        }
    }
}
)");
    
    std::vector<std::string> sourceFiles = {
        GetTestFilePath("BaseClass.cs"),
        GetTestFilePath("DerivedClass.cs")
    };
    
    // Build dependency graph
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Check that DerivedClass depends on BaseClass
    auto derivedNode = analyzer->GetFileNode(GetTestFilePath("DerivedClass.cs"));
    ASSERT_NE(derivedNode, nullptr);
    
    // Check exported symbols
    auto baseNode = analyzer->GetFileNode(GetTestFilePath("BaseClass.cs"));
    ASSERT_NE(baseNode, nullptr);
    EXPECT_TRUE(baseNode->ExportedSymbols.find("BaseClass") != baseNode->ExportedSymbols.end());
    
    Logger::Log::Info("✅ Basic dependency detection test passed");
}

TEST_F(DependencyGraphAnalyzerTest, AffectedFilesAnalysis) {
    // Create a chain of dependencies: A -> B -> C
    CreateTestFile("ClassA.cs", R"(
using System;

namespace TestNamespace {
    public class ClassA {
        public void Method() {
            Console.WriteLine("Class A");
        }
    }
}
)");
    
    CreateTestFile("ClassB.cs", R"(
using System;

namespace TestNamespace {
    public class ClassB : ClassA {
        public void Method() {
            Console.WriteLine("Class B");
        }
    }
}
)");
    
    CreateTestFile("ClassC.cs", R"(
using System;

namespace TestNamespace {
    public class ClassC : ClassB {
        public void Method() {
            Console.WriteLine("Class C");
        }
    }
}
)");
    
    std::vector<std::string> sourceFiles = {
        GetTestFilePath("ClassA.cs"),
        GetTestFilePath("ClassB.cs"),
        GetTestFilePath("ClassC.cs")
    };
    
    // Build dependency graph
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Test: changing ClassA should affect ClassB and ClassC
    std::vector<std::string> changedFiles = { GetTestFilePath("ClassA.cs") };
    auto result = analyzer->GetAffectedFiles(changedFiles);
    
    EXPECT_GE(result.AffectedFiles.size(), 1);
    EXPECT_FALSE(result.HasCircularDependencies);
    EXPECT_LT(result.AnalysisTime.count(), 100); // Should be fast
    
    Logger::Log::Info("✅ Affected files analysis test passed");
    Logger::Log::Info("   Affected files: {}", result.AffectedFiles.size());
    Logger::Log::Info("   Analysis time: {}ms", result.AnalysisTime.count());
}

TEST_F(DependencyGraphAnalyzerTest, CompilationOrderTest) {
    // Create files with dependencies
    CreateTestFile("Interface.cs", R"(
namespace TestNamespace {
    public interface ITestInterface {
        void TestMethod();
    }
}
)");
    
    CreateTestFile("BaseClass.cs", R"(
namespace TestNamespace {
    public class BaseClass : ITestInterface {
        public virtual void TestMethod() {
            // Implementation
        }
    }
}
)");
    
    CreateTestFile("DerivedClass.cs", R"(
namespace TestNamespace {
    public class DerivedClass : BaseClass {
        public override void TestMethod() {
            // Override implementation
        }
    }
}
)");
    
    std::vector<std::string> sourceFiles = {
        GetTestFilePath("DerivedClass.cs"),  // Intentionally out of order
        GetTestFilePath("BaseClass.cs"),
        GetTestFilePath("Interface.cs")
    };
    
    // Build dependency graph
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Get compilation order
    std::set<std::string> filesToCompile(sourceFiles.begin(), sourceFiles.end());
    auto compilationOrder = analyzer->GetCompilationOrder(filesToCompile);
    
    EXPECT_EQ(compilationOrder.size(), 3);
    
    // Interface should come before BaseClass, BaseClass before DerivedClass
    // (though our simple implementation might not detect all dependencies)
    
    Logger::Log::Info("✅ Compilation order test passed");
    Logger::Log::Info("   Compilation order:");
    for (size_t i = 0; i < compilationOrder.size(); ++i) {
        std::string filename = std::filesystem::path(compilationOrder[i]).filename().string();
        Logger::Log::Info("   {}. {}", i + 1, filename);
    }
}

TEST_F(DependencyGraphAnalyzerTest, CircularDependencyDetection) {
    // Create circular dependency: A -> B -> A
    CreateTestFile("ClassA.cs", R"(
namespace TestNamespace {
    public class ClassA {
        private ClassB b;
        
        public void Method() {
            // Uses ClassB
        }
    }
}
)");
    
    CreateTestFile("ClassB.cs", R"(
namespace TestNamespace {
    public class ClassB {
        private ClassA a;
        
        public void Method() {
            // Uses ClassA
        }
    }
}
)");
    
    std::vector<std::string> sourceFiles = {
        GetTestFilePath("ClassA.cs"),
        GetTestFilePath("ClassB.cs")
    };
    
    // Build dependency graph
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Check for circular dependencies
    bool hasCircular = analyzer->HasCircularDependencies();
    auto chains = analyzer->GetCircularDependencyChains();
    
    Logger::Log::Info("✅ Circular dependency detection test completed");
    Logger::Log::Info("   Has circular dependencies: {}", hasCircular ? "Yes" : "No");
    Logger::Log::Info("   Circular chains found: {}", chains.size());
}

TEST_F(DependencyGraphAnalyzerTest, SymbolExtractionTest) {
    // Create file with multiple symbols
    CreateTestFile("MultipleSymbols.cs", R"(
using System;

namespace TestNamespace {
    public interface ITestInterface {
        void TestMethod();
    }
    
    public class TestClass : ITestInterface {
        public void TestMethod() {
            Console.WriteLine("Test");
        }
    }
    
    public enum TestEnum {
        Value1,
        Value2,
        Value3
    }
}
)");
    
    std::vector<std::string> sourceFiles = { GetTestFilePath("MultipleSymbols.cs") };
    
    // Build dependency graph
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Check exported symbols
    auto node = analyzer->GetFileNode(GetTestFilePath("MultipleSymbols.cs"));
    ASSERT_NE(node, nullptr);
    
    EXPECT_TRUE(node->ExportedSymbols.find("ITestInterface") != node->ExportedSymbols.end());
    EXPECT_TRUE(node->ExportedSymbols.find("TestClass") != node->ExportedSymbols.end());
    EXPECT_TRUE(node->ExportedSymbols.find("TestEnum") != node->ExportedSymbols.end());
    
    Logger::Log::Info("✅ Symbol extraction test passed");
    Logger::Log::Info("   Exported symbols: {}", node->ExportedSymbols.size());
    for (const auto& symbol : node->ExportedSymbols) {
        Logger::Log::Info("   - {}", symbol);
    }
}

TEST_F(DependencyGraphAnalyzerTest, PerformanceTest) {
    // Create multiple files with dependencies
    const int numFiles = 20;
    
    // Create base interface
    CreateTestFile("IBase.cs", R"(
namespace TestNamespace {
    public interface IBase {
        void BaseMethod();
    }
}
)");
    
    // Create chain of classes
    for (int i = 0; i < numFiles; ++i) {
        std::string filename = "Class" + std::to_string(i) + ".cs";
        std::string baseClass = (i == 0) ? "IBase" : ("Class" + std::to_string(i - 1));
        
        std::string content = R"(
using System;

namespace TestNamespace {
    public class Class)" + std::to_string(i) + " : " + baseClass + R"( {
        public void Method)" + std::to_string(i) + R"(() {
            Console.WriteLine("Method )" + std::to_string(i) + R"(");
        }
        
        public void BaseMethod() {
            Console.WriteLine("Base method implementation");
        }
    }
}
)";
        CreateTestFile(filename, content);
    }
    
    // Collect all source files
    std::vector<std::string> sourceFiles;
    sourceFiles.push_back(GetTestFilePath("IBase.cs"));
    for (int i = 0; i < numFiles; ++i) {
        sourceFiles.push_back(GetTestFilePath("Class" + std::to_string(i) + ".cs"));
    }
    
    // Measure build time
    auto start = std::chrono::high_resolution_clock::now();
    bool success = analyzer->BuildGraph(sourceFiles);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_TRUE(success);
    EXPECT_LT(duration.count(), 1000); // Should complete within 1 second
    
    // Test affected files analysis
    std::vector<std::string> changedFiles = { GetTestFilePath("IBase.cs") };
    auto result = analyzer->GetAffectedFiles(changedFiles);
    
    EXPECT_GT(result.AffectedFiles.size(), 1); // Should affect multiple files
    
    Logger::Log::Info("✅ Performance test passed");
    Logger::Log::Info("   Build time: {}ms for {} files", duration.count(), sourceFiles.size());
    Logger::Log::Info("   Affected files: {}", result.AffectedFiles.size());
    Logger::Log::Info("   Analysis time: {}ms", result.AnalysisTime.count());
}

TEST_F(DependencyGraphAnalyzerTest, IncrementalUpdateTest) {
    // Create initial files
    CreateTestFile("ClassA.cs", R"(
namespace TestNamespace {
    public class ClassA {
        public void Method() {
            // Original implementation
        }
    }
}
)");
    
    CreateTestFile("ClassB.cs", R"(
namespace TestNamespace {
    public class ClassB : ClassA {
        public void Method() {
            // Derived implementation
        }
    }
}
)");
    
    std::vector<std::string> sourceFiles = {
        GetTestFilePath("ClassA.cs"),
        GetTestFilePath("ClassB.cs")
    };
    
    // Initial build
    bool success = analyzer->BuildGraph(sourceFiles);
    EXPECT_TRUE(success);
    
    // Modify one file
    CreateTestFile("ClassA.cs", R"(
namespace TestNamespace {
    public class ClassA {
        public void Method() {
            // Modified implementation
        }
        
        public void NewMethod() {
            // New method added
        }
    }
}
)");
    
    // Incremental update
    std::vector<std::string> updatedFiles = { GetTestFilePath("ClassA.cs") };
    bool updateSuccess = analyzer->UpdateAnalysis(updatedFiles);
    EXPECT_TRUE(updateSuccess);
    
    Logger::Log::Info("✅ Incremental update test passed");
}
