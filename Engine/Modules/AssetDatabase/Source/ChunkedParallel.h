#pragma once

#include <algorithm>
#include <cstddef>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

namespace GameEngine::AssetDatabase
{

/// Worker bounds for LaunchChunkedParallel: half the hardware threads, clamped.
inline constexpr size_t kMinChunkWorkers = 2;
inline constexpr size_t kMaxChunkWorkers = 8;

/// Split [0, count) into contiguous chunks and run chunkFn(chunkBegin, chunkEnd)
/// for each on its own std::async thread.
///
/// Returns one future per chunk, in chunk order, so a caller can merge results
/// in index order while later chunks still run. Every future must be drained
/// before anything chunkFn references goes out of scope.
///
/// The threads exit on completion: call this only where
/// Platform::SupportsTransientThreads() holds. AssetDatabase does not link
/// JobSystem, which is why this does not use a pool.
template <typename ChunkFn>
std::vector<std::future<std::invoke_result_t<ChunkFn&, size_t, size_t>>> LaunchChunkedParallel(size_t count,
                                                                                               ChunkFn chunkFn)
{
    using Result = std::invoke_result_t<ChunkFn&, size_t, size_t>;

    const size_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t workers = std::min(std::max(hardwareThreads / 2, kMinChunkWorkers), kMaxChunkWorkers);
    const size_t chunkSize = (count + workers - 1) / workers;

    std::vector<std::future<Result>> futures;
    futures.reserve(workers);
    for (size_t chunkBegin = 0; chunkBegin < count; chunkBegin += chunkSize)
    {
        const size_t chunkEnd = std::min(chunkBegin + chunkSize, count);
        futures.push_back(std::async(std::launch::async, chunkFn, chunkBegin, chunkEnd));
    }
    return futures;
}

} // namespace GameEngine::AssetDatabase
