#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Editor
{

enum class SceneDiffState
{
    Unchanged,
    Added,
    Removed,
    Modified,
};

// Runtime tint for a diff marker, in ARGB. Mirrors the --ui_color_vcs_*
// tokens in theme/tokens.css, which style the same markers wherever CSS can
// reach them; keep the two in step.
uint32_t SceneDiffStateColorArgb(SceneDiffState state);

// Human-readable state name used in marker tooltips.
const char* SceneDiffStateLabel(SceneDiffState state);

// Scene schemas compare property names lowercase — SceneIO folds every key
// with ToLowerAscii before dispatch, and schemas keep their comparisons
// lowercase to match. Diff keys preserve the file's authored casing for
// display, so anything that feeds a key back into ISceneComponentSchema
// Apply* must fold the field through this first.
std::string SceneSchemaFieldName(std::string_view field);

struct ScenePropertyDiff
{
    std::string Key;
    std::string OriginalValue;
    std::string CurrentValue;
    SceneDiffState State = SceneDiffState::Unchanged;
};

struct SceneObjectDiff
{
    std::string StableKey;
    std::string OriginalLabel;
    std::string CurrentLabel;
    // Scene parenting is a section-header attribute, not a reflected component
    // field — Components::Parent is explicitly [DoNotSerialize] so SceneIO can
    // own the link. A reparent therefore reaches neither Properties nor the
    // label, and has to be compared separately.
    std::string OriginalParent;
    std::string CurrentParent;
    bool IsEntity = false;
    SceneDiffState State = SceneDiffState::Unchanged;
    std::vector<ScenePropertyDiff> Properties;

    // Empty on either side means "root"; moving to or from the root counts.
    bool WasReparented() const { return OriginalParent != CurrentParent; }
};

// Parses the stable, human-readable scene schema and aligns sections and
// properties by identity rather than by line number. This keeps a reordered
// entity from turning the rest of a scene into a noisy text diff.
std::vector<SceneObjectDiff> BuildSceneDiff(std::string_view originalContent,
                                            std::string_view currentContent);

// Builds a scene diff against the active VCS provider's base revision. Returns
// an empty vector when no provider/base primitive is available or the file is
// ignored. Intended for in-place editor decorations as well as the Diff panel.
//
// Reading the base revision spawns a provider process (git show), so the result
// is memoized on the scene file's size and write time. Editor decoration paths
// may therefore call this per selection change and per property commit; only an
// actual save re-reads the baseline.
std::vector<SceneObjectDiff> LoadVersionControlledSceneDiff(const std::filesystem::path& scenePath);

// Drops the memoized baselines. Required whenever the base revision itself can
// have moved under an unchanged working file — a commit, checkout, or any other
// provider status change.
void InvalidateVersionControlledSceneDiffCache();

// Drops only the memos whose file status no longer matches what they were built
// under, and reports whether any went. Provider status notifications fire on a
// timer regardless of whether anything changed, so they must go through this
// rather than through the unconditional invalidate — otherwise every poll costs
// a fresh baseline read on the UI thread.
bool InvalidateVersionControlledSceneDiffsWhoseStatusChanged();

} // namespace GameEngine::Editor
