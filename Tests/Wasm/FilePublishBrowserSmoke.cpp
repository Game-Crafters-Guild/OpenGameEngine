// Browser regression for concurrent cache publication in nested WasmFS directories.
#include "FileSystem/FileSystem.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <emscripten/emscripten.h>

int main()
{
    namespace fs = std::filesystem;
    constexpr int kThreads = 4;
    constexpr int kIterations = 200;
    const fs::path dir = "/file-publish-smoke/cache/Tex";
    fs::create_directories(dir);
    std::atomic<int> verified{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    std::vector<std::future<void>> done;
    std::puts("FilePublishBrowserSmoke: 4 threads publishing 800 files in a nested directory");
    std::fflush(stdout);
    for (int t = 0; t < kThreads; ++t)
    {
        std::promise<void> finished;
        done.push_back(finished.get_future());
        threads.emplace_back([&, t, finished = std::move(finished)]() mutable
        {
            try
            {
                for (int i = 0; i < kIterations; ++i)
                {
                    const std::string payload = "payload-" + std::to_string(t) + "-" + std::to_string(i);
                    const fs::path target = dir / (payload + ".bin");
                    const fs::path temp = dir / (payload + ".tmp");
                    { std::ofstream out(temp, std::ios::binary); out << payload; }
                    if (!GameEngine::FileSystem::PublishFile(temp, target))
                    {
                        ++failures;
                        continue;
                    }
                    std::ifstream in(target, std::ios::binary);
                    const std::string actual((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
                    if (actual == payload && !fs::exists(temp))
                        ++verified;
                    else
                        ++failures;
                }
            }
            catch (...)
            {
                ++failures;
            }
            finished.set_value();
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (auto& future : done)
    {
        if (future.wait_until(deadline) != std::future_status::ready)
        {
            std::fprintf(stderr, "FAIL: file publication stalled after %d verified files\n", verified.load());
            std::fflush(stderr);
            emscripten_force_exit(2);
        }
    }
    for (auto& thread : threads)
        thread.join();
    if (verified != kThreads * kIterations || failures != 0)
    {
        std::fprintf(stderr, "FAIL: verified %d files, %d failures\n", verified.load(), failures.load());
        return 1;
    }
    fs::remove_all("/file-publish-smoke");
    std::puts("PASS: 800 concurrent file publications completed with exact payloads and no temporary files");
    return 0;
}
