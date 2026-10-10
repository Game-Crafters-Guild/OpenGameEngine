// Editor-module entry: registers the EZTree editor plugin + its component
// traits and pick provider when the eztree editor DLL loads (static init —
// the same single-TU pattern the runtime module's EZTreeModuleInit uses).
// The DLL links the EditorSDK import lib, so registrations land in the
// HOST's editor registries; EditorPluginRegistry replays the inspector pass
// for plugins that register after editor startup.

#include "EZTreeEditorPlugin.h"

namespace
{

struct EZTreeEditorModuleRegistrar
{
    EZTreeEditorModuleRegistrar()
    {
        GameEngine::EZTreeEditor::RegisterEZTreeEditorPlugin();
    }
};

EZTreeEditorModuleRegistrar s_Registrar;

} // namespace
