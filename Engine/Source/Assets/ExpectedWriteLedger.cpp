#include "Assets/ExpectedWriteLedger.h"

#include "AssetCore/PathNormalization.h"

#include <algorithm>
#include <system_error>

namespace GameEngine
{

ExpectedWriteLedger::ExpectedWriteLedger(std::chrono::milliseconds echoWindow)
    : m_EchoWindow(echoWindow)
{
}

std::string ExpectedWriteLedger::MakeKey(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    // Watcher events and writers name the same file from different starting points,
    // so absolutize before folding: NormalizeForRegistryKey canonicalizes case,
    // Unicode form and separators but leaves a relative path relative.
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    std::string key = AssetPaths::NormalizeForRegistryKey(ec ? path : absolute);

    // A trailing separator would make the subtree prefix test miss its own root.
    while (key.size() > 1 && key.back() == '/')
        key.pop_back();
    return key;
}

namespace
{
bool KeyIsWithin(const std::string& key, const std::string& root)
{
    if (key.size() < root.size() || key.compare(0, root.size(), root) != 0)
        return false;
    return key.size() == root.size() || key[root.size()] == '/';
}
} // namespace

ExpectedWriteLedger::Registration ExpectedWriteLedger::Expect(const std::filesystem::path& path,
                                                              bool subtree)
{
    std::string key = MakeKey(path);
    if (key.empty())
        return kNoRegistration;

    std::lock_guard<std::mutex> lock(m_Mutex);

    // Announcing a write makes whatever was recorded for that path obsolete: those bytes
    // are about to be replaced, so they cannot be what decides whether this write's own
    // echo is an echo. Left in place, the previous write's identity outvotes this
    // announcement for every event that arrives before this write reports, and the save
    // drives twice.
    m_Reported.erase(std::remove_if(m_Reported.begin(), m_Reported.end(),
                                    [&key, subtree](const ReportedWrite& r)
                                    {
                                        return subtree ? KeyIsWithin(r.Key, key) : r.Key == key;
                                    }),
                     m_Reported.end());

    const Registration id = m_NextRegistration++;
    m_Expected.push_back(Expectation{id, std::move(key), subtree});
    return id;
}

void ExpectedWriteLedger::Reported(const std::filesystem::path& written)
{
    std::string key = MakeKey(written);
    if (key.empty())
        return;

    std::error_code sizeEc;
    std::error_code timeEc;
    const std::uintmax_t size = std::filesystem::file_size(written, sizeEc);
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(written, timeEc);

    // A file that cannot be stat'ed has no identity to match a later change against,
    // and recording a blank one would swallow the next real change to the path.
    if (sizeEc || timeEc)
        return;

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_Mutex);
    PruneExpiredLocked(now);

    const auto existing = std::find_if(m_Reported.begin(), m_Reported.end(),
                                       [&key](const ReportedWrite& r) { return r.Key == key; });
    if (existing != m_Reported.end())
    {
        existing->Size = size;
        existing->Modified = modified;
        existing->Deadline = now + m_EchoWindow;
        return;
    }

    m_Reported.push_back(ReportedWrite{std::move(key), size, modified, now + m_EchoWindow});
}

void ExpectedWriteLedger::Retire(Registration id)
{
    if (id == kNoRegistration)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = std::find_if(m_Expected.begin(), m_Expected.end(),
                                 [id](const Expectation& e) { return e.Id == id; });
    if (it != m_Expected.end())
        m_Expected.erase(it);
}

bool ExpectedWriteLedger::IsEchoOfOurWrite(const std::filesystem::path& path)
{
    // Nothing is being written, which is the state on every change the watcher reports
    // for an edit made outside this process. Checked before the key is built: the fold
    // absolutizes and case-folds through utf8proc, and paying that per watcher event to
    // search two empty vectors is the one cost this lookup can avoid outright.
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Expected.empty() && m_Reported.empty())
            return false;
    }

    const std::string key = MakeKey(path);
    if (key.empty())
        return false;

    const auto now = std::chrono::steady_clock::now();

    const auto coveredByExpectation = [this, &key]()
    {
        return std::any_of(m_Expected.begin(), m_Expected.end(),
                           [&key](const Expectation& e)
                           { return e.Subtree ? KeyIsWithin(key, e.Key) : e.Key == key; });
    };
    const auto findReported = [this, &key]()
    {
        return std::find_if(m_Reported.begin(), m_Reported.end(),
                            [&key](const ReportedWrite& r) { return r.Key == key; });
    };

    std::uintmax_t expectedSize = 0;
    std::filesystem::file_time_type expectedModified{};
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        PruneExpiredLocked(now);

        const auto reported = findReported();
        if (reported == m_Reported.end())
        {
            // Nothing reported for this path: either the write is still in flight and
            // its own report will drive it, or this change is somebody else's.
            return coveredByExpectation();
        }

        expectedSize = reported->Size;
        expectedModified = reported->Modified;
    }

    // Stat outside the lock: it can block on a slow or remote volume, and a thread
    // announcing its next write must not queue behind that.
    std::error_code sizeEc;
    std::error_code timeEc;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeEc);
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, timeEc);
    const bool sameBytes = !sizeEc && !timeEc && size == expectedSize && modified == expectedModified;

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto reported = findReported();
    if (reported == m_Reported.end())
        return coveredByExpectation();
    if (sameBytes)
        return true;

    // The file moved on since this process wrote it, so the change is an edit of its
    // own and the record it outlived goes with it.
    m_Reported.erase(reported);
    return false;
}

void ExpectedWriteLedger::PruneExpiredLocked(std::chrono::steady_clock::time_point now)
{
    m_Reported.erase(std::remove_if(m_Reported.begin(), m_Reported.end(),
                                    [now](const ReportedWrite& r) { return now > r.Deadline; }),
                     m_Reported.end());
}

} // namespace GameEngine
