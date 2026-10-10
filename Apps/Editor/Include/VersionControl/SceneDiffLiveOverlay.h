#pragma once

#include "ECS/ECS.h"
#include "VersionControl/SceneDiff.h"

namespace GameEngine::Editor
{

// Re-bases a saved-file-vs-baseline object diff onto the entity as it exists in
// the world right now.
//
// BuildSceneDiff compares the scene file on disk against the VCS base revision,
// so it is blind to edits made since the last save. Any decoration or revert
// that claims to describe "this entity versus its baseline" must overlay the
// live component values first, otherwise unsaved edits are silently omitted:
// they neither light up as changed nor get restored by a revert.
SceneObjectDiff OverlaySceneDiffWithLiveValues(SceneObjectDiff object,
                                               const ECS::World& world,
                                               ECS::EntityHandle entity);

} // namespace GameEngine::Editor
