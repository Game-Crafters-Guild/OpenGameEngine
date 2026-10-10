// Package-module entry: registers the EZTree engine plugin when the eztree
// native module DLL loads (static init — the same single-TU pattern
// GE_REGISTER_COMPONENT uses, proven cross-DLL safe by the native-scripting
// smoke tests). The DLL links the Engine import lib, so this lands in the
// HOST's EnginePluginRegistry; when the rendering runtime is already live the
// registry's late-registration handler replays the full hook sequence and the
// extraction system is spliced into its declared Extraction-phase wave.

#include "EZTreeECS/EZTreePlugin.h"

namespace
{

struct EZTreeModuleRegistrar
{
    EZTreeModuleRegistrar()
    {
        GameEngine::EZTreeECS::RegisterEZTreeEnginePlugin();
    }
};

EZTreeModuleRegistrar s_Registrar;

} // namespace
