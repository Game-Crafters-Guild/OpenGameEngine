// Advisory producer-cost benchmark, not a timing-based unit gate. Compare raw
// guarded pointers to the owned-call-site API under the same Logger build.
#include "Logger/Logger.h"
#include "Logger/CallbackSink.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

int main()
{
    using namespace Logger;
    using Clock = std::chrono::steady_clock;
    constexpr int iterations = 200000;
    const char* file = "Engine/Source/Engine/Rendering/ExampleModule/ExampleSource.cpp";
    const char* function = "ExampleFunction";
    const OwnedSourceLocation source(file, function);
    size_t consumed = 0;
    auto measure = [&](auto&& operation) {
        const auto begin = Clock::now();
        for (int i = 0; i < iterations; ++i) consumed += operation().size();
        return std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / iterations;
    };
    const double guarded = measure([&] { return CopySourceCString(file); });
    const double owned = measure([&] { return std::string(source.File()); });
    std::printf("source copy ns: guarded=%.1f owned=%.1f consumed=%zu\n", guarded, owned, consumed);
    Log::Config config;
    config.GlobalMinLevel = LogLevel::Info;
    Log::Initialize(config);
    Log::ClearSinks();
    std::atomic<int> delivered{0};
    auto sink = std::make_unique<CallbackSink>();
    sink->RegisterCallback([&](const LogMessage&) { ++delivered; });
    Log::AddSink(std::move(sink));
    for (int count : {100, 1000, 10000})
    {
        for (int arm = 0; arm != 2; ++arm)
        {
            Log::Flush();
            delivered = 0;
            const auto begin = Clock::now();
            for (int i = 0; i < count; ++i)
                if (arm == 0)
                    Log::LogWithSource(LogLevel::Info, file, 12, function, std::string_view("benchmark"));
                else
                    Log::LogWithSource(LogLevel::Info, source, 12, std::string_view("benchmark"));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            Log::Flush();
            if (delivered.load() != count)
            {
                std::fprintf(stderr, "Invalid benchmark: only %d of %d records delivered\n", delivered.load(), count);
                Log::Shutdown();
                return 1;
            }
            std::printf("enqueue %d records (%s): %.3f ms\n", count, arm == 0 ? "raw" : "owned", ms);
        }
    }
    Log::Shutdown();
}
