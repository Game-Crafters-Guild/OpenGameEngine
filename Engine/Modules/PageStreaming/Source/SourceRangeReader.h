#pragma once

#include "AssetCore/SharedFileRead.h"
#include "Types/Types.h"

#include <filesystem>
#include <future>
#include <optional>
#include <vector>

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::PageStreaming
{

/// Reads byte ranges of one source file with one range of read-ahead: Request starts the next
/// range, Take waits for it. With an AssetIOService the read runs on its reader threads, so the
/// caller keeps working while the disk does; without one it runs on the calling thread inside
/// Request. Blocking I/O never runs on a job-pool worker. Take blocks: call from a thread that is
/// not a pool worker. One thread at a time.
class SourceRangeReader
{
public:
    SourceRangeReader(const std::filesystem::path& file, AssetIOService* io);

    /// False when the file cannot be opened (for the synchronous form; the service reports its own).
    bool IsOpen() const;

    /// Starts reading `bytes` bytes at `offset`. At most one range is outstanding: Take it first.
    void Request(uint64 offset, uint64 bytes);

    /// Waits for the outstanding range and moves its bytes into `out`. False when the read failed
    /// or nothing was requested.
    bool Take(std::vector<uint8>& out);

private:
    std::filesystem::path m_File;
    AssetIOService* m_Io = nullptr;
    SharedFileReader m_Reader; // the synchronous form
    std::optional<std::future<std::optional<std::vector<uint8>>>> m_Pending;
};

} // namespace GameEngine::PageStreaming
