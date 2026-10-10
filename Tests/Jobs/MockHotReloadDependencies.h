#pragma once

#include "Jobs/HotReloadTasks.h"
#include <chrono>
#include <thread>

namespace GameEngine::Tests {

/**
 * @brief Mock compiler for testing that simulates successful compilation
 */
class MockCompiler : public Jobs::ICompiler {
public:
    explicit MockCompiler(bool shouldSucceed = true, int simulatedDelayMs = 10)
        : m_ShouldSucceed(shouldSucceed), m_SimulatedDelayMs(simulatedDelayMs) {}

    CompilationResult compile(const String& assemblyPath, const std::atomic<bool>* /*cancelRequested*/) override {
        // Simulate compilation time
        std::this_thread::sleep_for(std::chrono::milliseconds(m_SimulatedDelayMs));

        CompilationResult result;
        result.success = m_ShouldSucceed;
        
        if (m_ShouldSucceed) {
            result.output = "Mock compilation successful for " + assemblyPath;
            result.warnings.push_back("Mock warning: Unused variable");
        } else {
            result.output = "Mock compilation failed for " + assemblyPath;
            result.errors.push_back("Mock error: Syntax error on line 42");
        }
        
        return result;
    }
    
    void setShouldSucceed(bool shouldSucceed) { m_ShouldSucceed = shouldSucceed; }
    void setSimulatedDelay(int delayMs) { m_SimulatedDelayMs = delayMs; }

private:
    bool m_ShouldSucceed;
    int m_SimulatedDelayMs;
};

/**
 * @brief Mock assembly loader for testing
 */
class MockAssemblyLoader : public Jobs::IAssemblyLoader {
public:
    std::vector<uint8_t> loadAssemblyBytes(const String& /*assemblyPath*/) override {
        // Return mock assembly bytes
        std::vector<uint8_t> mockBytes;
        
        // Add PE header signature
        mockBytes.push_back('M');
        mockBytes.push_back('Z');
        
        // Add some dummy data
        for (int i = 0; i < 1024; ++i) {
            mockBytes.push_back(static_cast<uint8_t>(i % 256));
        }
        
        return mockBytes;
    }
};

/**
 * @brief Mock assembly swapper for testing
 */
class MockAssemblySwapper : public Jobs::IAssemblySwapper {
public:
    explicit MockAssemblySwapper(bool shouldSucceed = true, int simulatedDelayMs = 5)
        : m_ShouldSucceed(shouldSucceed), m_SimulatedDelayMs(simulatedDelayMs) {}

    SwapResult swapAssembly(const std::vector<uint8_t>& /*assemblyBytes*/, const String& assemblyPath) override {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Simulate swap time
        std::this_thread::sleep_for(std::chrono::milliseconds(m_SimulatedDelayMs));
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        SwapResult result;
        result.success = m_ShouldSucceed;
        result.duration = duration;
        
        if (!m_ShouldSucceed) {
            result.errorMessage = "Mock swap failed for " + assemblyPath;
        }
        
        return result;
    }
    
    void setShouldSucceed(bool shouldSucceed) { m_ShouldSucceed = shouldSucceed; }
    void setSimulatedDelay(int delayMs) { m_SimulatedDelayMs = delayMs; }

private:
    bool m_ShouldSucceed;
    int m_SimulatedDelayMs;
};

} // namespace GameEngine::Tests
