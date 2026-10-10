#pragma once

namespace GameEngine::Mathematics
{
struct Vector2;
struct Vector3;
} // namespace GameEngine::Mathematics

namespace GameEngine::Editor::SceneTools
{

struct ScenePointerEvent;

/// Projects a world-space point into Scene View pixels: x right and y down from
/// the view's top-left corner, over a view of `view.viewW` by `view.viewH` pixels.
///
/// The camera is the state `SceneViewController::PopulatePointerCameraState`
/// writes into `view`: the position, the right, up and forward basis, and
/// either `tanHalfFovY` (perspective, when positive) or `orthoHeight` (the
/// orthographic frustum's full vertical extent in meters, when `tanHalfFovY`
/// is zero). The pointer fields of `view` are not read.
///
/// Returns false and leaves `outPixel` unchanged when the view has no area,
/// when the orthographic height is not positive, or when the point lies less
/// than 1 cm in front of a perspective camera (behind it included). A point
/// outside the view frustum still projects, to a pixel outside the view.
bool ProjectWorldToView(const ScenePointerEvent& view,
                        const Mathematics::Vector3& world,
                        Mathematics::Vector2& outPixel);

} // namespace GameEngine::Editor::SceneTools
