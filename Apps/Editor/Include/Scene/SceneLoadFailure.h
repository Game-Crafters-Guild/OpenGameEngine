#pragma once

#include "Scene/SceneIO.h" // Scene::SceneLoadDegradation

#include <cstddef>
#include <filesystem>
#include <string>

namespace GameEngine
{
namespace Editor
{

// A scene open that did not produce a loaded document. Recorded so the failure
// outlives the log line that announced it: the editor chrome, the debug server
// and any automation all need to see that what is in the world right now is not
// the scene anyone asked for.
struct SceneLoadFailure
{
    // The document the user tried to open — the .scene they picked, not the
    // backup file a crash-recovery open reads its content from.
    std::filesystem::path DocumentPath;

    std::string Message;             // e.g. "Invalid MeshRenderer.meshasset: ..."
    std::filesystem::path ErrorFile; // the file the error was raised in (may be an include)
    int ErrorLine = 0;               // 0 when unknown

    // The load had already cleared the world when it failed, so the entities
    // still in it are a partial instantiation of DocumentPath — neither the
    // previously open scene nor a loaded scene.
    bool WorldCleared = false;

    // Entities in the world at the moment the failure was recorded.
    std::size_t EntitiesInWorld = 0;
};

// A scene open that DID produce a loaded document, but not all of it. Distinct from
// SceneLoadFailure in the one way that matters to everything downstream: the world really is this
// scene, so the document keeps its path, its title and its autosave — it is simply missing the
// assignments this build could not read.
//
// It is recorded because the danger is silent and delayed: the user works in what looks like their
// scene and presses Ctrl+S, and the save writes back a file with their unreadable data replaced by
// whatever the fields defaulted to. Save interposes on this record.
struct SceneLoadDegraded
{
    // The document the user opened — the .scene they picked.
    std::filesystem::path DocumentPath;

    // Every assignment the load could not apply, straight from the loader.
    Scene::SceneLoadDegradation Census;

    std::size_t SkippedCount() const { return Census.skips.size(); }

    // The subset a save would actually LOSE. The rest had its authored text preserved and re-emits
    // verbatim, so warning about it as data loss would be crying wolf.
    std::size_t DroppedCount() const { return Census.DroppedCount(); }
};

// Whether a save is allowed to overwrite the source file of a scene that did not fully load.
// Defaulting to Refuse is the point: every existing caller keeps the safe behaviour, and consent has
// to be spelled out at the call site that obtained it.
enum class DegradedSavePolicy
{
    Refuse,
    SaveAnyway,
};

} // namespace Editor
} // namespace GameEngine
