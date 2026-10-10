#pragma once

#include "Types/Types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine
{

/**
 * @brief The writes this process announced, so the file watcher's report of one is
 * recognisable as that write arriving rather than as a change nobody made.
 *
 * A writer announces a path before it touches the file, publishes the bytes, then
 * reports the write. On a host with a file watcher the same write is delivered a
 * second time — the watcher sees the safe-save rename land — and everything the
 * change drives would run twice for one save. A change that matches nothing here is
 * somebody else's edit and must always be delivered.
 *
 * Announce-then-write is the order that makes this work: the watcher's report of a
 * new file can reach the ledger before the writer's own, so the announcement has to
 * be in place before the file exists.
 *
 * Matching is by path, qualified by content: a reported write remembers the size and
 * write time it left on disk, and a change to that path only counts as its echo while
 * the file still has them. An edit that moved the file on is a change of its own and
 * is delivered even inside the echo window.
 *
 * The content check is only as fine as the filesystem's timestamp: a rewrite that keeps
 * the size and lands inside that resolution (100 ns on NTFS, 2 s on FAT32), or a tool
 * that restores the original write time, is indistinguishable from the echo inside the
 * window and is treated as one. Accepted: no host this engine authors on has a coarse
 * enough stamp for a human edit to hit it, and the alternative is hashing every write.
 *
 * An edit that lands between the report and the echo delivers twice rather than once —
 * the echo fails the content check and is delivered as the edit it now describes, then
 * the edit's own event delivers too. Never a lost change, which is the direction that
 * matters.
 *
 * **Threading**: the announcing and reporting thread is whichever thread saved (the
 * main thread for a scene save, a worker for a cook); the querying thread is the file
 * watcher's. Every member is safe to call from any of them. The lock covers the two
 * vectors only — the filesystem stat behind the content check is taken outside it.
 *
 * Both vectors are empty when nothing is being written, so a change that arrives while
 * this process is writing nothing costs one lock and an emptiness test — no path
 * normalization and no filesystem call.
 */
class ExpectedWriteLedger
{
  public:
    using Registration = uint64;

    /// Identifies no registration; retiring or reporting it does nothing.
    static constexpr Registration kNoRegistration = 0;

    /**
     * @param echoWindow How long after a write is reported a matching change is still
     *        that write arriving. It has to outlast the watcher's own latency, which
     *        is why the caller supplies it: the ledger does not know what is watching.
     */
    explicit ExpectedWriteLedger(std::chrono::milliseconds echoWindow);

    /**
     * @brief Announce a write that is about to happen.
     *
     * Supersedes whatever was recorded for that path (or, with @p subtree, for anything
     * under it): those bytes are about to be replaced, so they are no longer evidence
     * about what a later change to the path means.
     *
     * @param path Target file, or with @p subtree the root of a tree about to be written.
     * @param subtree Whether every file under @p path is part of this write. Used where
     *        the files cannot be named in advance because copying the tree is what
     *        discovers them.
     * @return A registration to report against and retire. kNoRegistration for an empty path.
     */
    Registration Expect(const std::filesystem::path& path, bool subtree);

    /**
     * @brief Record that @p written is on disk, so its echo is recognisable by content.
     * Starts that path's echo window. A tree write calls this once per file it produced.
     */
    void Reported(const std::filesystem::path& written);

    /**
     * @brief Stop expecting the announced write.
     *
     * Ends the announcement only. Paths reported before it keep their own echo windows —
     * the write happened and its echo is still coming. A write that failed reported
     * nothing, so retiring it leaves the ledger with no reason to swallow a later change
     * to the path it did not produce.
     */
    void Retire(Registration id);

    /**
     * @brief Whether this change repeats a write this process has already driven.
     *
     * False for anything unannounced, and false for a path whose bytes have moved on
     * since it was reported.
     */
    bool IsEchoOfOurWrite(const std::filesystem::path& path);

  private:
    // An announced write, from before the file is touched until the writer retires it.
    struct Expectation
    {
        Registration Id = kNoRegistration;
        std::string Key;
        bool Subtree = false;
    };

    // A write that landed, with the bytes it left behind and the deadline past which
    // a change to that path is somebody else's.
    struct ReportedWrite
    {
        std::string Key;
        std::uintmax_t Size = 0;
        std::filesystem::file_time_type Modified{};
        std::chrono::steady_clock::time_point Deadline{};
    };

    /// Absolute + registry-canonical, so the writer's spelling and the watcher's
    /// on-disk casing produce the same key.
    static std::string MakeKey(const std::filesystem::path& path);

    /// Drops reported writes whose echo window has passed. Caller holds m_Mutex.
    void PruneExpiredLocked(std::chrono::steady_clock::time_point now);

    const std::chrono::milliseconds m_EchoWindow;

    mutable std::mutex m_Mutex;
    Registration m_NextRegistration = kNoRegistration + 1;
    std::vector<Expectation> m_Expected;
    std::vector<ReportedWrite> m_Reported;
};

} // namespace GameEngine
