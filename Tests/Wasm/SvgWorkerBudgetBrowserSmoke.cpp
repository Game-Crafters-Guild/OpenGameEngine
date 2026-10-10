// Browser-only regression for SVG decoding while the editor's reserved
// pthread pool is occupied. Node can grow that pool during a blocking wait,
// so running the same code under Node does not exercise this failure.
// Serve the build's bin directory and open SvgWorkerBudgetBrowserSmoke.html.

#include "Assets/SvgRasterizer.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <emscripten/emscripten.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

int main()
{
    using namespace GameEngine;

    // The editor reserves four job workers and four other threads for the
    // logger, asset readers and persistent storage. Keep those other slots
    // occupied without requiring an OPFS mount in this focused gate.
    std::mutex mutex;
    std::condition_variable released;
    bool stop = false;
    std::array<std::thread, 4> reserved;
    for (auto& thread : reserved)
    {
        thread = std::thread([&]
        {
            std::unique_lock lock(mutex);
            released.wait(lock, [&] { return stop; });
        });
    }

    JobSystem::WorkStealingThreadPool pool(4);
    std::puts("SvgWorkerBudgetBrowserSmoke: 4 job workers + 4 reserved threads, pthread pool 8");
    std::fflush(stdout);

    constexpr size_t kImageCount = 32;
    std::array<std::promise<bool>, kImageCount> completions;
    std::array<std::future<bool>, kImageCount> pending;
    JobSystem::JobCounter joined;
    for (size_t i = 0; i < kImageCount; ++i)
    {
        pending[i] = completions[i].get_future();
        pool.Run([&, i]
        {
            SvgRasterizedImage image;
            const bool decoded = RasterizeSvgToRgbaAtSize(
                R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="#ff0000"/></svg>)",
                32.0f, image);
            const bool valid = decoded && image.Width == 32 && image.Height == 32
                && image.DataSize == 4096 && image.Data
                && image.Data[0] == 255 && image.Data[3] == 255;
            completions[i].set_value(valid);
        }, joined);
    }

    // Source ejection must wait for active decoders before releasing their
    // assets. The browser main thread cannot return to its event loop here
    // to finish starting an unexpected auxiliary pool.
    bool allValid = true;
    for (auto& completion : pending)
    {
        if (completion.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        {
            std::puts("FAIL: SVG decoding exceeded 10 seconds with all reserved workers in use");
            std::fflush(stdout);
            // Tear down workers even when a decoder cannot complete. This
            // reports the regression without leaving a frozen test page.
            emscripten_force_exit(2);
        }
        allValid = completion.get() && allValid;
    }
    pool.Wait(joined);

    {
        std::lock_guard lock(mutex);
        stop = true;
    }
    released.notify_all();
    for (auto& thread : reserved)
    {
        thread.join();
    }

    if (!allValid)
    {
        std::puts("FAIL: SVG decoding produced invalid dimensions or RGBA pixels");
        return 1;
    }
    std::printf("PASS: %zu concurrent SVG rasterizations completed with valid RGBA pixels\n",
                kImageCount);
    return 0;
}
