#pragma once

namespace GameEngine
{
class HierarchyPanel;
class SceneViewPanel;

namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// Connects Scene View asset drops to hierarchy selection and the undo stack (Scene Drop command).
void WireSceneViewAssetDropUndo(SceneViewPanel* sceneViewPanel,
                                HierarchyPanel* hierarchyPanel,
                                UndoRedoService* undoRedo,
                                EditorChangeNotifications* notifications);

} // namespace Editor
} // namespace GameEngine
