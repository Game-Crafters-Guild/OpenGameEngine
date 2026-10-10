#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

// Filesystem operations whose portable std::filesystem spelling is wrong on
// at least one platform. On the browser build the project tree lives in OPFS
// behind WASMFS's proxy: copy_file fails (EPERM: no permission bits to
// preserve), and a recursive walk from the UI thread blocks the thread that
// services the proxy — a deadlock, not slowness. Everything std::filesystem
// does correctly everywhere stays std::filesystem at the call site.
namespace GameEngine::FileSystem
{

/// Copy one file, replacing an existing destination and creating its parent.
/// Uses native copying where supported; Web streams bytes for OPFS.
bool CopyFileContents(const std::filesystem::path& from, const std::filesystem::path& to);

/// Recursively copy a directory tree (or a single file), replacing existing files.
/// Native copying preserves file permissions; Web streams bytes for OPFS.
/// `to` is the destination path to create, not its parent.
bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to);

/// Name a temporary file beside `target`, distinct across concurrent native
/// processes and calls in this process. The caller creates the file; this does
/// not reserve it. Use PublishFile after writing the complete contents.
std::filesystem::path MakeTemporarySiblingPath(const std::filesystem::path& target);

/// Make the bytes already written to `temp` the contents of `target`, consuming
/// `temp` either way unless both paths already name the same file (a no-op).
/// Every caller that builds a file beside its destination
/// and then puts it in place goes through here.
///
/// Native: a rename, so a concurrent reader sees either the previous contents
/// or the complete new ones. A failed replacement preserves the destination;
/// stage `temp` on the destination volume. Brief permission/busy failures are
/// retried for up to five attempts, with 200 ms between attempts.
/// Persistent permission/busy failures block the calling thread for about 0.8 s.
///
/// Web: a stream copy plus a remove, because WASMFS's rename deadlocks.
/// `__syscall_renameat` holds the new parent's directory lock and then walks
/// that parent's ancestors, locking each one — child before parent — while
/// every path lookup locks parent before child (`Directory::Handle::cacheChild`
/// locks the child to set its parent). Two threads in one directory tree close
/// that cycle, and renameat holds a process-wide lock for its whole duration,
/// so the first deadlock stops every later rename in the process. A reader on
/// the web build can therefore observe a partially written `target`; callers
/// revalidate what they read and treat a short or malformed file as a miss.
bool PublishFile(const std::filesystem::path& temp, const std::filesystem::path& target);

/// Remove the temporary siblings (MakeTemporarySiblingPath) in `directory` whose target's file
/// name ends in `targetSuffix` and whose writing process is no longer running: what a writer that
/// died between its write and its publish leaves behind. The temporary files of a running process,
/// this one included, are kept, so a concurrent writer is never disturbed. A recycled process id
/// keeps an orphan until a later sweep. Returns the number removed.
std::size_t RemoveOrphanedTemporaryFiles(const std::filesystem::path& directory, std::string_view targetSuffix);

/// Direct subdirectories of `root`, one level only — the single read the UI
/// thread may make on every platform. Recursing from the UI thread deadlocks
/// on web; gate a recursive walk on Platform::SupportsSynchronousDirectoryWalk.
std::vector<std::filesystem::path> ListDirectories(const std::filesystem::path& root);

/// The one directory the platform keeps every user project in, or empty where
/// a project may live wherever the user chooses (desktop). On web it is the
/// persistent storage mount: the only tree that survives a reload, so it is
/// where projects are created and imported and the complete set of projects
/// the page has.
std::filesystem::path ProjectLibraryRoot();

/// Whether two paths that differ only in letter case name different files.
/// True on Linux and on the browser's MEMFS/OPFS; false on Windows and on the
/// default macOS volume. Registry keys are normalized only where this is false.
bool IsCaseSensitive();

} // namespace GameEngine::FileSystem
