#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

/// Replaces `output` with the completed build in `staging`.
///
/// The previous output is moved aside to `<output>.previous` and deleted only
/// once the new output is complete; no copy of it is kept. What cannot be
/// deleted then is left in `<output>.discard` and cleared by the next
/// promotion. A `<output>.previous` left by an interrupted promotion blocks
/// the next one until the user resolves it.
///
/// The move of `staging` is retried briefly (a scanner or indexer can hold a
/// freshly written file without delete sharing), then falls back to a copy.
/// `shouldCancel` is polled before each rename attempt and between copied
/// entries.
///
/// Returns false with a message in `errors` when the new output could not be
/// put in place. Cleanup that fails after a successful promotion (the backup,
/// or staging after a copy) is reported in `warnings`.
bool PromoteBuildOutput(const std::filesystem::path& staging,
                        const std::filesystem::path& output,
                        std::vector<std::string>& errors,
                        std::vector<std::string>& warnings,
                        const std::function<bool()>& shouldCancel = {});

} // namespace GameEngine
