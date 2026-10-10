#pragma once

#include "Scene/SceneLoadFailure.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace GameEngine::Editor
{

// Builds the `sceneDegraded` object get_editor_state reports for a scene that loaded but could not
// have every assignment in the file applied.
//
// It takes BOTH records because they answer different questions and reporting only one made this
// surface contradict the window title. `atLoad` is what that load could not apply; the title reads
// (PARTIAL LOAD) for as long as it stands. `outstanding` is what a SAVE still has to answer for,
// and it shrinks as overrides retire — the user discarding a preserved value, the component being
// removed, the entity being destroyed. Keyed on `outstanding` alone, the object went null the
// moment the last override was discarded, so an agent read a healthy scene while a human read a
// partial one.
//
// So presence follows `atLoad`, exactly like the title, and `outstandingCount`/`skips` carry the
// save-facing answer. `skips` lists only outstanding rows: an override the user discarded is never
// reported as text a save will write back.
//
// Returns a null json when `atLoad` is empty — the scene loaded cleanly and there is nothing to say.
nlohmann::json BuildSceneDegradedReport(const std::optional<SceneLoadDegraded>& atLoad,
                                        const std::optional<SceneLoadDegraded>& outstanding);

} // namespace GameEngine::Editor
