#pragma once

#include "Markups/MarkupPresentation.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
struct SceneViewCameraPose;
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class MarkupEditorBridge;
class UndoRedoService;

// What the agent's mark-up methods act on: the edit world and the editor services
// around it. Every pointer but Undo and Notifications must be set; PlayMode refuses
// every method (mark-ups are edited in the edit world).
struct MarkupRequestContext
{
    ECS::World* World = nullptr;
    MarkupEditorBridge* Bridge = nullptr;
    UndoRedoService* Undo = nullptr;
    EditorChangeNotifications* Notifications = nullptr;
    bool PlayMode = false;
    // markup_frame fits a volume to this field of view (MarkupFrameDistance): the tangent of
    // the main Scene View's narrower half field of view.
    float FrameTanHalfFov = kMarkupDefaultTanHalfFov;
};

// The eight markup_* debug-server methods (DebugServer/MarkupDebugHandlers.cpp registers
// them; mcp/src/tools/markups.ts exposes them). Each returns its result, or a refusal
// built by RefuseRequest whose reason names the fix. The parameters and results are
// documented in docs/Editor/world-markups.html.

// Rows sorted by last update, newest first; filters: status, author, since (a scene
// revision, strictly after), changedBy, includeHidden.
nlohmann::json ListMarkups(const MarkupRequestContext& context, const nlohmann::json& params);
// One row with its description, its thread and its shape.
nlohmann::json GetMarkup(const MarkupRequestContext& context, const nlohmann::json& params);
// A volume or region mark-up, one undo step (a region's exclusions included); status Proposed
// unless given.
nlohmann::json CreateMarkup(const MarkupRequestContext& context, const nlohmann::json& params);
// Any of title, status, description, tags, color, shape, hidden; on a region its outline, type,
// extrudeHeight, members and exclusions; kind "region" converts a volume. One undo step, outside
// which hidden (a view state) stays.
nlohmann::json UpdateMarkup(const MarkupRequestContext& context, const nlohmann::json& params);
// Appends a comment to the thread.
nlohmann::json CommentOnMarkup(const MarkupRequestContext& context, const nlohmann::json& params);
// The Scene View pose that frames the mark-up's bounding sphere (ComputeLookAtPose, the
// look_at math); the caller applies it.
nlohmann::json FrameMarkup(const MarkupRequestContext& context, const nlohmann::json& params,
                           SceneViewCameraPose& outPose);
// Which of `points` (world x, z) lie inside the mark-up's area: a region's base outline with
// its members applied (MarkupECS::MarkupRegionArea), a volume's footprint on the ground.
nlohmann::json ContainsInMarkup(const MarkupRequestContext& context, const nlohmann::json& params);
// Shows or hides one mark-up or all: a view state, no scene edit, no undo step.
nlohmann::json SetMarkupsVisible(const MarkupRequestContext& context, const nlohmann::json& params);

} // namespace GameEngine::Editor
