#pragma once

#include "Types/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>

namespace GameEngine {

/**
 * @brief Analyzes C# source files to build dependency graphs for incremental compilation
 * 
 * This class parses C# source files to understand dependencies between files,
 * enabling smart incremental compilation by determining which files need
 * recompilation when other files change.
 */
class DependencyGraphAnalyzer {
public:
    /**
     * @brief Types of dependencies between files
     */
    enum class DependencyType {
        UsingStatement,     // using SomeNamespace;
        Inheritance,        // class A : B
        FieldType,          // public SomeType field;
        MethodParameter,    // void Method(SomeType param)
        MethodReturnType,   // SomeType Method()
        GenericConstraint,  // where T : SomeType
        AttributeUsage,     // [SomeAttribute]
        NestedType         // class Outer { class Inner {} }
    };

    /**
     * @brief Information about a dependency between two files
     */
    struct DependencyInfo {
        std::string SourceFile;      // File that depends on something
        std::string TargetFile;      // File that is depended upon
        std::string SourceSymbol;    // Symbol in source file (e.g., class name)
        std::string TargetSymbol;    // Symbol in target file (e.g., base class)
        DependencyType Type;         // Type of dependency
        int LineNumber;              // Line number where dependency occurs

        DependencyInfo() : Type(DependencyType::UsingStatement), LineNumber(0) {}
    };

    /**
     * @brief Information about a file in the dependency graph
     */
    struct FileNode {
        std::string FilePath;
        std::set<std::string> Dependencies;     // Files this file depends on
        std::set<std::string> Dependents;       // Files that depend on this file
        std::vector<DependencyInfo> OutgoingDeps; // Detailed dependency information
        std::unordered_set<std::string> ExportedSymbols; // Symbols exported by this file
        std::unordered_set<std::string> ImportedSymbols; // Symbols imported by this file
        std::time_t LastAnalyzed;               // When this file was last analyzed

        FileNode() : LastAnalyzed(0) {}
    };

    /**
     * @brief Result of dependency analysis
     */
    struct AnalysisResult {
        std::vector<std::string> AffectedFiles;
        std::vector<std::string> CompilationOrder;
        std::chrono::milliseconds AnalysisTime;
        bool HasCircularDependencies;
        std::vector<std::vector<std::string>> CircularDependencyChains;

        AnalysisResult() : AnalysisTime(0), HasCircularDependencies(false) {}
    };

public:
    /**
     * @brief Constructor
     */
    DependencyGraphAnalyzer();

    /**
     * @brief Destructor
     */
    ~DependencyGraphAnalyzer();

    /**
     * @brief Build dependency graph for a set of source files
     * @param sourceFiles List of C# source files to analyze
     * @return True if analysis was successful
     */
    bool BuildGraph(const std::vector<std::string>& sourceFiles);

    /**
     * @brief Get all files affected by changes to the specified files
     * @param changedFiles List of files that have changed
     * @return AnalysisResult containing affected files and compilation order
     */
    AnalysisResult GetAffectedFiles(const std::vector<std::string>& changedFiles);

    /**
     * @brief Get optimal compilation order for a set of files
     * @param files Files to compile
     * @return Ordered list of files for compilation (dependencies first)
     */
    std::vector<std::string> GetCompilationOrder(const std::set<std::string>& files);

    /**
     * @brief Check if there are circular dependencies
     * @return True if circular dependencies exist
     */
    bool HasCircularDependencies() const;

    /**
     * @brief Get circular dependency chains
     * @return List of circular dependency chains
     */
    std::vector<std::vector<std::string>> GetCircularDependencyChains() const;

    /**
     * @brief Update analysis for specific files (incremental update)
     * @param files Files to re-analyze
     * @return True if update was successful
     */
    bool UpdateAnalysis(const std::vector<std::string>& files);

    /**
     * @brief Clear all dependency information
     */
    void Clear();

    /**
     * @brief Get dependency information for a specific file
     * @param filePath Path to the file
     * @return Pointer to FileNode if found, nullptr otherwise
     */
    const FileNode* GetFileNode(const std::string& filePath) const;

    /**
     * @brief Get all files in the dependency graph
     * @return List of all tracked files
     */
    std::vector<std::string> GetAllFiles() const;

    /**
     * @brief Export dependency graph to DOT format for visualization
     * @param outputPath Path to save the DOT file
     * @return True if export was successful
     */
    bool ExportToDot(const std::string& outputPath) const;

private:
    std::unordered_map<std::string, FileNode> m_FileNodes;
    mutable std::mutex m_GraphMutex;
    bool m_HasCircularDependencies;
    std::vector<std::vector<std::string>> m_CircularDependencyChains;

    /**
     * @brief Analyze a single C# source file
     * @param filePath Path to the file to analyze
     * @return FileNode with dependency information
     */
    FileNode AnalyzeFile(const std::string& filePath);

    /**
     * @brief Parse C# source content to extract dependencies
     * @param content C# source code content
     * @param filePath Path to the source file
     * @return Vector of dependency information
     */
    std::vector<DependencyInfo> ParseDependencies(const std::string& content, const std::string& filePath);

    /**
     * @brief Extract using statements from C# content
     * @param content C# source code
     * @param filePath Source file path
     * @return Vector of using statement dependencies
     */
    std::vector<DependencyInfo> ExtractUsingStatements(const std::string& content, const std::string& filePath);

    /**
     * @brief Extract inheritance relationships from C# content
     * @param content C# source code
     * @param filePath Source file path
     * @return Vector of inheritance dependencies
     */
    std::vector<DependencyInfo> ExtractInheritanceRelationships(const std::string& content, const std::string& filePath);

    /**
     * @brief Extract type references from C# content
     * @param content C# source code
     * @param filePath Source file path
     * @return Vector of type reference dependencies
     */
    std::vector<DependencyInfo> ExtractTypeReferences(const std::string& content, const std::string& filePath);

    /**
     * @brief Extract exported symbols from C# content
     * @param content C# source code
     * @return Set of exported symbol names
     */
    std::unordered_set<std::string> ExtractExportedSymbols(const std::string& content);

    /**
     * @brief Resolve symbol to file mapping
     * @param symbol Symbol name to resolve
     * @return File path that exports the symbol, or empty string if not found
     */
    std::string ResolveSymbolToFile(const std::string& symbol) const;

    /**
     * @brief Perform topological sort on dependency graph
     * @param files Files to sort
     * @return Topologically sorted list of files
     */
    std::vector<std::string> TopologicalSort(const std::set<std::string>& files) const;

    /**
     * @brief Detect circular dependencies using DFS
     * @return True if circular dependencies are found
     */
    bool DetectCircularDependencies();

    /**
     * @brief DFS helper for circular dependency detection
     * @param node Current node being visited
     * @param visited Set of visited nodes
     * @param recursionStack Current recursion stack
     * @param path Current path for cycle detection
     * @return True if cycle is found
     */
    bool DFSCircularDetection(const std::string& node, 
                             std::unordered_set<std::string>& visited,
                             std::unordered_set<std::string>& recursionStack,
                             std::vector<std::string>& path);

    /**
     * @brief Update bidirectional dependencies
     */
    void UpdateBidirectionalDependencies();

    /**
     * @brief Get file content as string
     * @param filePath Path to the file
     * @return File content or empty string if error
     */
    std::string ReadFileContent(const std::string& filePath) const;
};

} // namespace GameEngine
