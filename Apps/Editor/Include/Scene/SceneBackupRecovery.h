#pragma once

#include <filesystem>
#include <vector>

namespace GameEngine::Editor
{

// Crash-recovery staging for scene autosave (Option B, terrain arc).
//
// The autosave timer never touches the files an explicit Save owns. It writes
// the scene snapshot to `<stem>.backup<ext>` beside the scene, and stages dirty
// terrain zone payloads to `<live>.tzone.backup` siblings. On a clean close all
// of that staging is deleted; only a crash leaves it behind. The next open of
// that scene detects the leftovers and offers to restore or discard them.
//
// Everything here is pure filesystem logic — no UI, no engine singletons — so
// the detection/promotion/decision paths are unit-testable headless.

// `<stem>.backup<ext>` beside the scene (matches SceneDocumentManager::SaveBackupCopy).
std::filesystem::path SceneBackupPathFor(const std::filesystem::path& scenePath);

// The scene's companion payload folder `<stem>_Zones` (matches TerrainZonePayloadSaver).
std::filesystem::path SceneZonesFolderFor(const std::filesystem::path& scenePath);

// Staged autosave sibling for a live .tzone payload: `<livePath>.backup`.
std::filesystem::path StagedZonePayloadPathFor(const std::filesystem::path& livePayloadPath);

// What a relaunch (or scene open) found on disk for `scenePath`.
struct SceneRecoveryScan
{
    std::filesystem::path ScenePath;
    std::filesystem::path BackupScenePath; // where the backup would live; may not exist

    // The backup scene exists and is newer than the scene file — i.e. it captures
    // work the saved scene does not. An older leftover (pre-dating the last
    // explicit Save) is stale and never offered.
    bool BackupSceneUsable = false;

    // Staged `*.tzone.backup` files under the scene's companion zones folder.
    // Staging is deleted on clean close, so any survivor implies a crash.
    std::vector<std::filesystem::path> StagedPayloads;

    bool RecoveryPending() const { return BackupSceneUsable || !StagedPayloads.empty(); }
};

// Detect leftover autosave staging for `scenePath`. Pure read — changes nothing.
SceneRecoveryScan ScanForSceneRecovery(const std::filesystem::path& scenePath);

// ACCEPT: atomically rename each staged payload over its live .tzone (strip the
// `.backup` suffix). Returns false if any promotion failed; successfully promoted
// files stay promoted (per-file rename is the atomic unit).
bool PromoteStagedZonePayloads(const std::vector<std::filesystem::path>& stagedPayloads);

// DECLINE / clean close: delete the staged payloads, the backup scene, and any
// leftover atomic-write temps. Live files are never touched.
void DiscardSceneBackups(const SceneRecoveryScan& scan);

// The user's answer to the "Restore unsaved changes?" prompt, resolved against
// the filesystem. Restore promotes the staged payloads and points the open at
// the backup scene content (when usable); decline discards all staging and
// points the open at the last saved state. Factored out of the modal so the
// decision path is drivable from tests.
struct SceneRecoveryResolution
{
    // File whose CONTENT the editor should load.
    std::filesystem::path ContentPathToOpen;
    // The document identity (always the real scene path).
    std::filesystem::path DocumentPath;
    // True when ContentPathToOpen is the backup scene (document must open dirty).
    bool OpenedFromBackup = false;
};
SceneRecoveryResolution ResolveRecoveryDecision(const SceneRecoveryScan& scan, bool restore);

} // namespace GameEngine::Editor
