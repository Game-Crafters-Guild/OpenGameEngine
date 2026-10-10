#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// Progress and outcome of a "Generate Thumbnails" folder bake
// (IThumbnailProvider::GenerateFolderThumbnails). Every queued asset ends in
// exactly one of the three outcomes; Running stays true until all have.
struct FolderBakeReport
{
    size_t Queued = 0;
    size_t Generated = 0;      // rendered and written to the thumbnail cache
    size_t AlreadyCurrent = 0; // its cached thumbnail was up to date, or a visible tile drew it
    // The assets that could not be generated (the model does not load, renders
    // nothing, or timed out), in the order they failed.
    std::vector<std::filesystem::path> Failed;
    // Files the asset database lists under the folder that are not on disk:
    // never queued; refreshing the folder drops them from the database.
    std::vector<std::filesystem::path> MissingFromDisk;
    bool Running = false;

    size_t Finished() const { return Generated + AlreadyCurrent + Failed.size(); }
    bool operator==(const FolderBakeReport&) const = default;
};

// The line the asset browser shows for `report`: the progress while it runs,
// the outcome after, naming up to two failed or missing files and saying what
// to do about them; empty when the bake had nothing to report.
std::string DescribeFolderBakeReport(const FolderBakeReport& report);

} // namespace GameEngine
