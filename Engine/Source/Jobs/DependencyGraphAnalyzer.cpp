#include "Jobs/DependencyGraphAnalyzer.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <queue>
#include <stack>
#include <filesystem>

namespace GameEngine {

DependencyGraphAnalyzer::DependencyGraphAnalyzer() 
    : m_HasCircularDependencies(false) {
}

DependencyGraphAnalyzer::~DependencyGraphAnalyzer() {
}

bool DependencyGraphAnalyzer::BuildGraph(const std::vector<std::string>& sourceFiles) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Logger::Log::Debug("[DependencyGraphAnalyzer] Building dependency graph for {} files", sourceFiles.size());
    
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    
    try {
        // Clear existing graph
        m_FileNodes.clear();
        m_HasCircularDependencies = false;
        m_CircularDependencyChains.clear();
        
        // Analyze each file
        for (const auto& filePath : sourceFiles) {
            auto fileNode = AnalyzeFile(filePath);
            m_FileNodes[filePath] = std::move(fileNode);
        }
        
        // Update bidirectional dependencies
        UpdateBidirectionalDependencies();
        
        // Detect circular dependencies
        m_HasCircularDependencies = DetectCircularDependencies();
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        Logger::Log::Info("[DependencyGraphAnalyzer] Built dependency graph in {}ms: {} files, {} circular deps",
                           duration.count(), sourceFiles.size(), m_HasCircularDependencies ? "has" : "no");
        
        return true;
        
    } catch (const std::exception& e) {
        Logger::Log::Error("[DependencyGraphAnalyzer] Error building dependency graph: {}", e.what());
        return false;
    }
}

DependencyGraphAnalyzer::AnalysisResult DependencyGraphAnalyzer::GetAffectedFiles(
    const std::vector<std::string>& changedFiles) {
    
    auto startTime = std::chrono::high_resolution_clock::now();
    AnalysisResult result;
    
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    
    try {
        std::set<std::string> affectedSet;
        std::queue<std::string> toProcess;
        
        // Start with changed files
        for (const auto& file : changedFiles) {
            if (m_FileNodes.find(file) != m_FileNodes.end()) {
                affectedSet.insert(file);
                toProcess.push(file);
            }
        }
        
        // BFS to find all affected files
        while (!toProcess.empty()) {
            std::string currentFile = toProcess.front();
            toProcess.pop();
            
            auto it = m_FileNodes.find(currentFile);
            if (it != m_FileNodes.end()) {
                // Add all dependents of this file
                for (const auto& dependent : it->second.Dependents) {
                    if (affectedSet.find(dependent) == affectedSet.end()) {
                        affectedSet.insert(dependent);
                        toProcess.push(dependent);
                    }
                }
            }
        }
        
        // Convert to vector and get compilation order
        result.AffectedFiles.assign(affectedSet.begin(), affectedSet.end());
        result.CompilationOrder = GetCompilationOrder(affectedSet);
        result.HasCircularDependencies = m_HasCircularDependencies;
        result.CircularDependencyChains = m_CircularDependencyChains;

        auto endTime = std::chrono::high_resolution_clock::now();
        result.AnalysisTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        Logger::Log::Debug("[DependencyGraphAnalyzer] Found {} affected files in {}ms",
                            result.AffectedFiles.size(), result.AnalysisTime.count());
        
    } catch (const std::exception& e) {
        Logger::Log::Error("[DependencyGraphAnalyzer] Error analyzing affected files: {}", e.what());
    }
    
    return result;
}

std::vector<std::string> DependencyGraphAnalyzer::GetCompilationOrder(const std::set<std::string>& files) {
    return TopologicalSort(files);
}

DependencyGraphAnalyzer::FileNode DependencyGraphAnalyzer::AnalyzeFile(const std::string& filePath) {
    FileNode node;
    node.FilePath = filePath;
    node.LastAnalyzed = std::time(nullptr);
    
    try {
        std::string content = ReadFileContent(filePath);
        if (content.empty()) {
            Logger::Log::Warning("[DependencyGraphAnalyzer] Could not read file: {}", filePath);
            return node;
        }
        
        // Parse dependencies
        auto dependencies = ParseDependencies(content, filePath);
        node.OutgoingDeps = dependencies;

        // Extract exported symbols
        node.ExportedSymbols = ExtractExportedSymbols(content);

        // Build dependency sets
        for (const auto& dep : dependencies) {
            if (!dep.TargetFile.empty()) {
                node.Dependencies.insert(dep.TargetFile);
            }
            if (!dep.TargetSymbol.empty()) {
                node.ImportedSymbols.insert(dep.TargetSymbol);
            }
        }

        Logger::Log::Debug("[DependencyGraphAnalyzer] Analyzed {}: {} deps, {} exports",
                     filePath, node.Dependencies.size(), node.ExportedSymbols.size());
        
    } catch (const std::exception& e) {
        Logger::Log::Error("[DependencyGraphAnalyzer] Error analyzing file {}: {}", filePath, e.what());
    }
    
    return node;
}

std::vector<DependencyGraphAnalyzer::DependencyInfo> DependencyGraphAnalyzer::ParseDependencies(
    const std::string& content, const std::string& filePath) {
    
    std::vector<DependencyInfo> dependencies;
    
    // Extract different types of dependencies
    auto usingDeps = ExtractUsingStatements(content, filePath);
    auto inheritanceDeps = ExtractInheritanceRelationships(content, filePath);
    auto typeDeps = ExtractTypeReferences(content, filePath);
    
    // Combine all dependencies
    dependencies.insert(dependencies.end(), usingDeps.begin(), usingDeps.end());
    dependencies.insert(dependencies.end(), inheritanceDeps.begin(), inheritanceDeps.end());
    dependencies.insert(dependencies.end(), typeDeps.begin(), typeDeps.end());
    
    return dependencies;
}

std::vector<DependencyGraphAnalyzer::DependencyInfo> DependencyGraphAnalyzer::ExtractUsingStatements(
    const std::string& content, const std::string& filePath) {
    
    std::vector<DependencyInfo> dependencies;
    
    // Regex to match using statements
    std::regex usingRegex(R"(^\s*using\s+([^;]+);)");
    std::sregex_iterator iter(content.begin(), content.end(), usingRegex);
    std::sregex_iterator end;
    
    int lineNumber = 1;
    for (auto it = iter; it != end; ++it) {
        DependencyInfo dep;
        dep.SourceFile = filePath;
        dep.TargetSymbol = it->str(1);
        dep.Type = DependencyType::UsingStatement;
        dep.LineNumber = lineNumber;

        // For now, we can't easily resolve using statements to specific files
        // This would require a more sophisticated symbol resolution system
        dep.TargetFile = ""; // Will be resolved later if needed
        
        dependencies.push_back(dep);
        lineNumber++;
    }
    
    return dependencies;
}

std::vector<DependencyGraphAnalyzer::DependencyInfo> DependencyGraphAnalyzer::ExtractInheritanceRelationships(
    const std::string& content, const std::string& filePath) {
    
    std::vector<DependencyInfo> dependencies;
    
    // Regex to match class inheritance
    std::regex inheritanceRegex(R"(class\s+(\w+)\s*:\s*([^{]+))");
    std::sregex_iterator iter(content.begin(), content.end(), inheritanceRegex);
    std::sregex_iterator end;
    
    for (auto it = iter; it != end; ++it) {
        std::string className = it->str(1);
        std::string baseClasses = it->str(2);
        
        // Split base classes by comma
        std::stringstream ss(baseClasses);
        std::string baseClass;
        
        while (std::getline(ss, baseClass, ',')) {
            // Trim whitespace
            baseClass.erase(0, baseClass.find_first_not_of(" \t"));
            baseClass.erase(baseClass.find_last_not_of(" \t") + 1);
            
            if (!baseClass.empty()) {
                DependencyInfo dep;
                dep.SourceFile = filePath;
                dep.SourceSymbol = className;
                dep.TargetSymbol = baseClass;
                dep.Type = DependencyType::Inheritance;
                dep.TargetFile = ResolveSymbolToFile(baseClass);
                
                dependencies.push_back(dep);
            }
        }
    }
    
    return dependencies;
}

std::vector<DependencyGraphAnalyzer::DependencyInfo> DependencyGraphAnalyzer::ExtractTypeReferences(
    const std::string& content, const std::string& filePath) {
    
    std::vector<DependencyInfo> dependencies;
    
    // This is a simplified implementation
    // A full implementation would use a proper C# parser
    
    // Regex to match field declarations
    std::regex fieldRegex(R"((?:public|private|protected|internal)\s+(\w+)\s+\w+\s*[;=])");
    std::sregex_iterator iter(content.begin(), content.end(), fieldRegex);
    std::sregex_iterator end;
    
    for (auto it = iter; it != end; ++it) {
        std::string typeName = it->str(1);
        
        // Skip primitive types
        if (typeName == "int" || typeName == "string" || typeName == "bool" || 
            typeName == "float" || typeName == "double" || typeName == "var") {
            continue;
        }
        
        DependencyInfo dep;
        dep.SourceFile = filePath;
        dep.TargetSymbol = typeName;
        dep.Type = DependencyType::FieldType;
        dep.TargetFile = ResolveSymbolToFile(typeName);
        
        dependencies.push_back(dep);
    }
    
    return dependencies;
}

std::unordered_set<std::string> DependencyGraphAnalyzer::ExtractExportedSymbols(const std::string& content) {
    std::unordered_set<std::string> symbols;
    
    // Extract class names
    std::regex classRegex(R"((?:public\s+)?(?:partial\s+)?class\s+(\w+))");
    std::sregex_iterator iter(content.begin(), content.end(), classRegex);
    std::sregex_iterator end;
    
    for (auto it = iter; it != end; ++it) {
        symbols.insert(it->str(1));
    }
    
    // Extract interface names
    std::regex interfaceRegex(R"((?:public\s+)?interface\s+(\w+))");
    iter = std::sregex_iterator(content.begin(), content.end(), interfaceRegex);
    
    for (auto it = iter; it != end; ++it) {
        symbols.insert(it->str(1));
    }
    
    // Extract enum names
    std::regex enumRegex(R"((?:public\s+)?enum\s+(\w+))");
    iter = std::sregex_iterator(content.begin(), content.end(), enumRegex);
    
    for (auto it = iter; it != end; ++it) {
        symbols.insert(it->str(1));
    }
    
    return symbols;
}

std::string DependencyGraphAnalyzer::ResolveSymbolToFile(const std::string& symbol) const {
    // Search through all file nodes to find which file exports this symbol
    for (const auto& [filePath, node] : m_FileNodes) {
        if (node.ExportedSymbols.find(symbol) != node.ExportedSymbols.end()) {
            return filePath;
        }
    }
    return ""; // Symbol not found
}

std::vector<std::string> DependencyGraphAnalyzer::TopologicalSort(const std::set<std::string>& files) const {
    std::vector<std::string> result;
    std::unordered_map<std::string, int> inDegree;
    std::queue<std::string> zeroInDegree;
    
    // Initialize in-degree count
    for (const auto& file : files) {
        inDegree[file] = 0;
    }
    
    // Calculate in-degrees
    for (const auto& file : files) {
        auto it = m_FileNodes.find(file);
        if (it != m_FileNodes.end()) {
            for (const auto& dep : it->second.Dependencies) {
                if (files.find(dep) != files.end()) {
                    inDegree[dep]++;
                }
            }
        }
    }
    
    // Find files with zero in-degree
    for (const auto& [file, degree] : inDegree) {
        if (degree == 0) {
            zeroInDegree.push(file);
        }
    }
    
    // Topological sort
    while (!zeroInDegree.empty()) {
        std::string current = zeroInDegree.front();
        zeroInDegree.pop();
        result.push_back(current);
        
        auto it = m_FileNodes.find(current);
        if (it != m_FileNodes.end()) {
            for (const auto& dependent : it->second.Dependents) {
                if (files.find(dependent) != files.end()) {
                    inDegree[dependent]--;
                    if (inDegree[dependent] == 0) {
                        zeroInDegree.push(dependent);
                    }
                }
            }
        }
    }
    
    return result;
}

void DependencyGraphAnalyzer::UpdateBidirectionalDependencies() {
    // Clear existing dependents
    for (auto& [filePath, node] : m_FileNodes) {
        node.Dependents.clear();
    }

    // Rebuild dependents based on dependencies
    for (const auto& [filePath, node] : m_FileNodes) {
        for (const auto& dependency : node.Dependencies) {
            auto it = m_FileNodes.find(dependency);
            if (it != m_FileNodes.end()) {
                it->second.Dependents.insert(filePath);
            }
        }
    }
}

bool DependencyGraphAnalyzer::DetectCircularDependencies() {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> recursionStack;
    std::vector<std::string> path;
    
    m_CircularDependencyChains.clear();
    
    for (const auto& [filePath, node] : m_FileNodes) {
        if (visited.find(filePath) == visited.end()) {
            if (DFSCircularDetection(filePath, visited, recursionStack, path)) {
                return true;
            }
        }
    }
    
    return false;
}

bool DependencyGraphAnalyzer::DFSCircularDetection(const std::string& node,
                                                  std::unordered_set<std::string>& visited,
                                                  std::unordered_set<std::string>& recursionStack,
                                                  std::vector<std::string>& path) {
    visited.insert(node);
    recursionStack.insert(node);
    path.push_back(node);
    
    auto it = m_FileNodes.find(node);
    if (it != m_FileNodes.end()) {
        for (const auto& dependency : it->second.Dependencies) {
            if (recursionStack.find(dependency) != recursionStack.end()) {
                // Found a cycle
                auto cycleStart = std::find(path.begin(), path.end(), dependency);
                std::vector<std::string> cycle(cycleStart, path.end());
                cycle.push_back(dependency); // Complete the cycle
                m_CircularDependencyChains.push_back(cycle);
                return true;
            }
            
            if (visited.find(dependency) == visited.end()) {
                if (DFSCircularDetection(dependency, visited, recursionStack, path)) {
                    return true;
                }
            }
        }
    }
    
    recursionStack.erase(node);
    path.pop_back();
    return false;
}

std::string DependencyGraphAnalyzer::ReadFileContent(const std::string& filePath) const {
    std::string content;
    if (!ReadFileTextShared(std::filesystem::path(filePath), content)) {
        return "";
    }
    return content;
}

bool DependencyGraphAnalyzer::HasCircularDependencies() const {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    return m_HasCircularDependencies;
}

std::vector<std::vector<std::string>> DependencyGraphAnalyzer::GetCircularDependencyChains() const {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    return m_CircularDependencyChains;
}

void DependencyGraphAnalyzer::Clear() {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    m_FileNodes.clear();
    m_HasCircularDependencies = false;
    m_CircularDependencyChains.clear();
}

const DependencyGraphAnalyzer::FileNode* DependencyGraphAnalyzer::GetFileNode(const std::string& filePath) const {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    auto it = m_FileNodes.find(filePath);
    return (it != m_FileNodes.end()) ? &it->second : nullptr;
}

std::vector<std::string> DependencyGraphAnalyzer::GetAllFiles() const {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    std::vector<std::string> files;
    for (const auto& [filePath, node] : m_FileNodes) {
        files.push_back(filePath);
    }
    return files;
}

bool DependencyGraphAnalyzer::UpdateAnalysis(const std::vector<std::string>& files) {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    
    try {
        // Re-analyze specified files
        for (const auto& filePath : files) {
            auto fileNode = AnalyzeFile(filePath);
            m_FileNodes[filePath] = std::move(fileNode);
        }
        
        // Update bidirectional dependencies
        UpdateBidirectionalDependencies();
        
        // Re-detect circular dependencies
        m_HasCircularDependencies = DetectCircularDependencies();
        
        return true;
    } catch (const std::exception& e) {
        Logger::Log::Error("[DependencyGraphAnalyzer] Error updating analysis: {}", e.what());
        return false;
    }
}

bool DependencyGraphAnalyzer::ExportToDot(const std::string& outputPath) const {
    std::lock_guard<std::mutex> lock(m_GraphMutex);
    
    std::ofstream file(outputPath);
    if (!file.is_open()) {
        Logger::Log::Error("[DependencyGraphAnalyzer] Could not open file for DOT export: {}", outputPath);
        return false;
    }
    
    file << "digraph DependencyGraph {\n";
    file << "  rankdir=TB;\n";
    file << "  node [shape=box];\n\n";
    
    // Write nodes
    for (const auto& [filePath, node] : m_FileNodes) {
        std::string nodeName = std::filesystem::path(filePath).filename().string();
        file << "  \"" << nodeName << "\";\n";
    }
    
    file << "\n";
    
    // Write edges
    for (const auto& [filePath, node] : m_FileNodes) {
        std::string sourceNode = std::filesystem::path(filePath).filename().string();
        for (const auto& dependency : node.Dependencies) {
            std::string targetNode = std::filesystem::path(dependency).filename().string();
            file << "  \"" << sourceNode << "\" -> \"" << targetNode << "\";\n";
        }
    }
    
    file << "}\n";
    file.close();
    
    Logger::Log::Info("[DependencyGraphAnalyzer] Exported dependency graph to: {}", outputPath);
    return true;
}

} // namespace GameEngine
