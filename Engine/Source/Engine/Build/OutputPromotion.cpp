#include "Engine/Build/OutputPromotion.h"

#include "Engine/Build/DirectoryCopy.h"
#include "FileSystem/RenameWithRetry.h"

#include <cctype>

namespace fs = std::filesystem;

namespace GameEngine {

namespace {

constexpr const char* kCancelledKeepingPrevious = "Build cancelled; the previous output was kept";
constexpr const char* kCancelled = "Build cancelled";

bool IsCancelled(const std::function<bool()>& shouldCancel)
{
    return shouldCancel && shouldCancel();
}

// The system message without its trailing period, for use inside a sentence.
std::string ErrorText(const std::error_code& ec)
{
    std::string text = ec.message();
    while (!text.empty() && (text.back() == '.' || std::isspace(static_cast<unsigned char>(text.back()))))
        text.pop_back();
    return text;
}

void RestoreBackup(const fs::path& backup, const fs::path& output, std::vector<std::string>& errors)
{
    std::error_code ec;
    fs::rename(backup, output, ec);
    if (ec)
        errors.push_back("Previous output remains recoverable at: " + backup.string());
}

// Deletes the replaced output after a successful promotion. It is renamed to
// `discard` first: a file another process has mapped (a loaded DLL) can
// survive the delete, and what survives must not look like an interrupted
// promotion's backup, which blocks the next build.
void DeleteReplacedOutput(const fs::path& backup, const fs::path& discard, std::vector<std::string>& warnings)
{
    std::error_code ec;
    fs::rename(backup, discard, ec);
    const fs::path& replaced = ec ? backup : discard;
    fs::remove_all(replaced, ec);
    if (ec)
        warnings.push_back("Could not delete all of the replaced output at " + replaced.string() +
                           "; a program may still be using a file in it");
}

} // namespace

// Invariant: when this returns false, `output` holds what it held before the
// call and `staging` still holds the candidate. The one exception is a backup
// that cannot be moved back, which is reported with its path. `.previous`
// exists only while a promotion is in progress; `.discard` is a replaced
// output that may be deleted at any time.
bool PromoteBuildOutput(const fs::path& staging,
                        const fs::path& output,
                        std::vector<std::string>& errors,
                        std::vector<std::string>& warnings,
                        const std::function<bool()>& shouldCancel)
{
    const fs::path backup = fs::path(output.string() + ".previous");
    const fs::path discard = fs::path(output.string() + ".discard");
    std::error_code ec;
    fs::remove_all(discard, ec);
    if (fs::exists(backup, ec))
    {
        errors.push_back("A build stopped while replacing " + output.string() +
                         ", so that folder may be incomplete. The game it was replacing is in " +
                         backup.string() + ". Move that folder back or delete it, then build again.");
        return false;
    }
    if (!fs::is_directory(staging, ec))
    {
        errors.push_back("Cannot promote completed output: " + staging.string() + " is missing");
        return false;
    }

    const bool hadOutput = fs::exists(output, ec);
    if (hadOutput && !FileSystem::RenameWithRetry(output, backup, shouldCancel, ec))
    {
        errors.push_back(IsCancelled(shouldCancel)
                             ? std::string(kCancelledKeepingPrevious)
                             : "Cannot replace " + output.string() + ": a file in it is in use (" + ErrorText(ec) +
                                   "). Close the running game or any program using files in that folder, then "
                                   "build again.");
        return false;
    }

    if (!FileSystem::RenameWithRetry(staging, output, shouldCancel, ec))
    {
        if (IsCancelled(shouldCancel))
        {
            errors.push_back(hadOutput ? kCancelledKeepingPrevious : kCancelled);
            if (hadOutput)
                RestoreBackup(backup, output, errors);
            return false;
        }
        const std::string renameError = ErrorText(ec);
        std::error_code copyEc;
        if (!CopyDirectoryKeepingLinks(staging, output, copyEc, shouldCancel))
        {
            if (copyEc)
                errors.push_back("Cannot promote completed output: " + renameError + "; copy failed: " +
                                 ErrorText(copyEc));
            else
                errors.push_back(hadOutput ? kCancelledKeepingPrevious : kCancelled);
            std::error_code cleanupEc;
            fs::remove_all(output, cleanupEc);
            if (hadOutput)
                RestoreBackup(backup, output, errors);
            return false;
        }
        fs::remove_all(staging, ec);
        if (ec)
            warnings.push_back("Output was copied; could not remove the staging directory: " + staging.string());
    }

    if (hadOutput)
        DeleteReplacedOutput(backup, discard, warnings);
    return true;
}

} // namespace GameEngine
