#pragma once

#include "AssetCore/Types.h"

#include <filesystem>

namespace GameEngine
{

/// Read-side file access for watched/project asset files.
///
/// On Windows every open here grants FILE_SHARE_READ | FILE_SHARE_WRITE |
/// FILE_SHARE_DELETE, so an external safe-save (write a temp file, then
/// MoveFileEx(REPLACE_EXISTING) over the target — Photoshop, VS Code, most
/// DCC tools) never fails against a handle the engine holds: the replace
/// succeeds immediately and an in-flight read keeps consuming the displaced
/// old bytes as a consistent snapshot; the file watcher then delivers the
/// new content as its own event. CRT-backed streams cannot express this
/// (fopen/std::ifstream cap at read+write sharing, never delete), so any
/// engine-side READ of a file an external application may replace must use
/// this instead of std::ifstream/fopen. Write paths are unaffected — they
/// keep their existing locking semantics on purpose.
class SharedFileReader
{
public:
    SharedFileReader() = default;
    explicit SharedFileReader(const std::filesystem::path& path);
    ~SharedFileReader();

    SharedFileReader(SharedFileReader&& other) noexcept;
    SharedFileReader& operator=(SharedFileReader&& other) noexcept;
    DISALLOW_COPY_AND_ASSIGN(SharedFileReader);

    bool Open(const std::filesystem::path& path);
    void Close();
    bool IsOpen() const;

    /// Current file size; -1 when unavailable.
    int64 Size() const;

    /// Absolute seek from the start of the file.
    bool SeekTo(uint64 offset);

    /// Returns bytes actually read (0 at EOF); -1 on error.
    int64 Read(void* dst, uint64 bytes);

private:
#ifdef _WIN32
    void* m_Handle = nullptr; // HANDLE; nullptr when closed.
#else
    int m_Fd = -1;
#endif
};

/// Whole-file convenience wrappers over SharedFileReader. Return false when
/// the file cannot be opened or a read fails; an empty file yields success
/// with empty output.
bool ReadFileBytesShared(const std::filesystem::path& path, Vector<uint8>& outBytes);
bool ReadFileTextShared(const std::filesystem::path& path, String& outText);

} // namespace GameEngine
