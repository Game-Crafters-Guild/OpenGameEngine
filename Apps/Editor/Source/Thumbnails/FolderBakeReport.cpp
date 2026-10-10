#include "Thumbnails/FolderBakeReport.h"

#include <format>

namespace GameEngine
{
namespace
{

// Up to this many files are named in the line; more are left to the log.
constexpr size_t kMaxFilesNamedInline = 2;

// "a.fbx" or "a.fbx and b.fbx"; empty when there are more than the line names.
std::string NameFiles(const std::vector<std::filesystem::path>& files)
{
    if (files.empty() || files.size() > kMaxFilesNamedInline)
        return {};
    std::string names = files[0].filename().string();
    if (files.size() == 2)
        names += " and " + files[1].filename().string();
    return names;
}

} // namespace

std::string DescribeFolderBakeReport(const FolderBakeReport& report)
{
    if (report.Queued == 0 && report.MissingFromDisk.empty())
        return {};
    if (report.Running)
        return std::format("Generating thumbnails: {} of {}", report.Finished(), report.Queued);
    std::string text = std::format("Thumbnails: {} generated, {} already current", report.Generated,
                                   report.AlreadyCurrent);
    if (const std::string named = NameFiles(report.Failed); !named.empty())
        text += std::format(". Could not generate {}; the log says why", named);
    else if (!report.Failed.empty())
        text += std::format(". {} could not be generated; the log names them and says why", report.Failed.size());
    if (const std::string named = NameFiles(report.MissingFromDisk); !named.empty())
        text += std::format(". {} {} not on disk: refresh the folder", named,
                            report.MissingFromDisk.size() == 1 ? "is" : "are");
    else if (!report.MissingFromDisk.empty())
        text += std::format(". {} listed files are not on disk: refresh the folder", report.MissingFromDisk.size());
    return text;
}

} // namespace GameEngine
