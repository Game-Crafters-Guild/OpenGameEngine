#include <iostream>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <vector>
#include <thread>

// Simplified test - just measure compilation time directly
// This bypasses the complex engine initialization

/**
 * @brief Simplified compilation performance test
 */
int main() {
    std::cout << "🎯 SIMPLIFIED COMPILATION PERFORMANCE TEST" << std::endl;
    std::cout << "Target: Compilation time measurement" << std::endl;

    try {
        // Test 1: Measure dotnet build performance directly
        std::cout << "\n📊 TEST 1: Direct dotnet build performance" << std::endl;

        std::string projectPath = "../Scripts/GameEngine.Scripts.csproj";
        std::string buildCommand = "dotnet build \"" + projectPath + "\" --configuration Debug --verbosity quiet";

        std::cout << "Command: " << buildCommand << std::endl;

        // Measure multiple compilation cycles
        const int numCycles = 3;
        std::vector<std::chrono::milliseconds> compilationTimes;

        for (int i = 0; i < numCycles; ++i) {
            std::cout << "\nCycle " << (i + 1) << "/" << numCycles << std::endl;

            auto startTime = std::chrono::high_resolution_clock::now();

            // Execute dotnet build
            int result = std::system(buildCommand.c_str());

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            compilationTimes.push_back(duration);

            std::cout << "  Compilation time: " << duration.count() << "ms" << std::endl;
            std::cout << "  Build result: " << (result == 0 ? "SUCCESS" : "FAILED") << std::endl;

            // Brief pause between cycles
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Calculate statistics
        auto avgTime = std::accumulate(compilationTimes.begin(), compilationTimes.end(), std::chrono::milliseconds(0)) / numCycles;
        auto minTime = *std::min_element(compilationTimes.begin(), compilationTimes.end());
        auto maxTime = *std::max_element(compilationTimes.begin(), compilationTimes.end());

        std::cout << "\n📊 COMPILATION PERFORMANCE RESULTS:" << std::endl;
        std::cout << "  Average compilation time: " << avgTime.count() << "ms" << std::endl;
        std::cout << "  Minimum compilation time: " << minTime.count() << "ms" << std::endl;
        std::cout << "  Maximum compilation time: " << maxTime.count() << "ms" << std::endl;

        // Performance analysis
        std::cout << "\n🎯 PERFORMANCE ANALYSIS:" << std::endl;
        if (avgTime.count() < 1000) {
            std::cout << "✅ EXCELLENT: Average compilation <1s" << std::endl;
        } else if (avgTime.count() < 2000) {
            std::cout << "✅ GOOD: Average compilation <2s" << std::endl;
        } else if (avgTime.count() < 5000) {
            std::cout << "⚠️ ACCEPTABLE: Average compilation <5s" << std::endl;
        } else {
            std::cout << "❌ SLOW: Average compilation >=5s" << std::endl;
        }

        std::cout << "\n💡 ASYNC PIPELINE BENEFIT:" << std::endl;
        std::cout << "  If compilation runs on background thread:" << std::endl;
        std::cout << "  - Main thread blocking: ~5-10ms (just task submission)" << std::endl;
        std::cout << "  - Background compilation: " << avgTime.count() << "ms" << std::endl;
        std::cout << "  - Performance improvement: " << (avgTime.count() / 10) << "x faster main thread" << std::endl;

        std::cout << "\n🎯 COMPILATION PERFORMANCE TEST COMPLETED" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
