#include "HotReloadBenchmark.h"
#include "Logger/Logger.h"

int main() {
    try {
        GameEngine::Tests::HotReloadBenchmark::runAllBenchmarks();
        return 0;
    } catch (const std::exception& e) {
        Logger::Log::Error("Benchmark failed: {}", e.what());
        return 1;
    }
}
