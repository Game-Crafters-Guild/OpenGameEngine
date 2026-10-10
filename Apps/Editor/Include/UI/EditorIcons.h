#pragma once

// Commonly used editor icon image paths ("editor:" resolves in the editor
// asset mount). Add an entry here when an icon is shared across
// panels/inspectors instead of repeating the path per call site.
namespace GameEngine::EditorIcons
{
    // Clipboard and history.
    inline constexpr const char* kCopy = "editor:Icons/copy.png";
    inline constexpr const char* kPaste = "editor:Icons/paste.png";
    inline constexpr const char* kUndo = "editor:Icons/undo.png";
    // Destructive.
    inline constexpr const char* kTrash = "editor:Icons/trash.png";
    inline constexpr const char* kClose = "editor:Icons/xclose.png";
    // Restore-shaped actions.
    inline constexpr const char* kReset = "editor:Icons/ResetReload.png";
    // Creation.
    inline constexpr const char* kPlus = "editor:Icons/plus.png";
    // Files and navigation.
    inline constexpr const char* kSave = "editor:Icons/save.png";
    inline constexpr const char* kFolderOpen = "editor:Icons/folder_open.png";
    inline constexpr const char* kFolder = "editor:Icons/Folder@64px.png";
    inline constexpr const char* kSearch = "editor:Icons/search.png";
    inline constexpr const char* kBookmark = "editor:Icons/book.png";
    inline constexpr const char* kCloud = "editor:Icons/cloud.png";
    inline constexpr const char* kInfo = "editor:Icons/info.png";
    inline constexpr const char* kLink = "editor:Icons/link.png";
    inline constexpr const char* kStats = "editor:Icons/stats.png";
    // Settings-shaped.
    inline constexpr const char* kSettings = "editor:Icons/settings.png";
    inline constexpr const char* kBrush = "editor:Icons/brush.png";
    inline constexpr const char* kPencil = "editor:Icons/draw_curve.png";
    // Visibility and ordering.
    inline constexpr const char* kEye = "editor:Icons/eye.png";
    inline constexpr const char* kSortList = "editor:Icons/list.png";
    inline constexpr const char* kArrowUp = "editor:Icons/ArrowUp.png";
    inline constexpr const char* kArrowDown = "editor:Icons/ArrowDown.png";
    // Scene content.
    inline constexpr const char* kScene = "editor:Icons/scene.png";
    inline constexpr const char* kCamera = "editor:Icons/camera.png";
    inline constexpr const char* kLight = "editor:Icons/PointLight.png";
    inline constexpr const char* kMaterial = "editor:Icons/material.png";
    inline constexpr const char* kTerrain = "editor:Icons/terrain.png";
    inline constexpr const char* kOcean = "editor:Icons/ocean.svg";
    inline constexpr const char* kWind = "editor:Icons/wind.svg";
    inline constexpr const char* kProbe = "editor:Icons/probe.png";
    inline constexpr const char* kDDGIVolume = "editor:Icons/ddgivolume.svg";
    inline constexpr const char* kTree = "editor:Icons/SettingsTrees.png";
    inline constexpr const char* kPulse = "editor:Icons/pulse.png";
    inline constexpr const char* kMarkup = "editor:Icons/markup.svg";
    inline constexpr const char* kActivity = "editor:Icons/activity.svg";
    inline constexpr const char* kSpline = "editor:Icons/GizmoSpline.png";
    inline constexpr const char* kNavGrid = "editor:Icons/grid.png";
    inline constexpr const char* kNode = "editor:Icons/node.png";
    inline constexpr const char* kBox = "editor:Icons/box.png";
    inline constexpr const char* kCube = "editor:Icons/Cube.png";
    inline constexpr const char* kSphere = "editor:Icons/Sphere.png";
    inline constexpr const char* kPlane = "editor:Icons/Plane.png";
    inline constexpr const char* kCapsule = "editor:Icons/Capsule.png";
    inline constexpr const char* kScript = "editor:Icons/log.png";
    inline constexpr const char* kTag = "editor:Icons/SettingsTags.png";
    inline constexpr const char* kVcs = "editor:Icons/SettingsVersionControl.png";
    inline constexpr const char* kLockClosed = "editor:Icons/LockClosed.png";
    inline constexpr const char* kLockOpen = "editor:Icons/LockOpen.png";
    // Post-processing stack.
    inline constexpr const char* kPostFx = "editor:Icons/postfx.png";
    inline constexpr const char* kColorFilter = "editor:Icons/color-filter.svg";
    // Animation editing.
    inline constexpr const char* kFilm = "editor:Icons/film.png";
    inline constexpr const char* kPlay = "editor:Icons/play.png";
    inline constexpr const char* kAddKeyMarker = "editor:Icons/add_key_marker.png";
    inline constexpr const char* kAddMarker = "editor:Icons/marker.png";
    inline constexpr const char* kBreakTangents = "editor:Icons/break_tangent.png";
    inline constexpr const char* kUnifyTangents = "editor:Icons/unify_tangent.png";
    inline constexpr const char* kLinearCurve = "editor:Icons/linear_curve.png";
    inline constexpr const char* kEase = "editor:Icons/ease_ease.png";
    inline constexpr const char* kSplineCurve = "editor:Icons/spline_curve.png";
    inline constexpr const char* kSteppedCurve = "editor:Icons/stepped_curve.png";
    inline constexpr const char* kDrawCurve = kPencil; // curve-tool name for the same pencil glyph
    inline constexpr const char* kLoop = "editor:Icons/looptime.png";
    inline constexpr const char* kLattice = "editor:Icons/lattice.png";
    // Component / entity types.
    inline constexpr const char* kVideoCam = "editor:Icons/videocam.png";
    inline constexpr const char* kSkyEnvironment = "editor:Icons/skyenvironment.png";
    inline constexpr const char* kSparkles = "editor:Icons/sparkles.png";
    inline constexpr const char* kMusicNote = "editor:Icons/music-note-icon@32px.png";
    inline constexpr const char* kPhysics = "editor:Icons/PhysicsSettings.png";
    // Tools.
    inline constexpr const char* kMove = "editor:Icons/GiszmoTranslate.png";
    inline constexpr const char* kAlarm = "editor:Icons/alarm.png";
    inline constexpr const char* kPointer = "editor:Icons/arrowmouse.png";
    inline constexpr const char* kRuler = "editor:Icons/Ruler.png";
    inline constexpr const char* kColorPicker = "editor:Icons/colorpicker.png";
    inline constexpr const char* k2D = "editor:Icons/2d.png";
    inline constexpr const char* kDot = "editor:Icons/dot.png";
    inline constexpr const char* kMagnet = "editor:Icons/magnet.png";
    inline constexpr const char* kNavGizmo = "editor:Icons/navgizmo.png";
} // namespace GameEngine::EditorIcons
