// The allocation counter's hook for a production image (Engine, Editor, Player,
// GameEngine.Native, EditorSDK). cmake/AllocationHook.cmake compiles this file into
// each of them in every configuration; it carries the replacements only where
// GE_DEBUG_INSTRUMENTATION is 1 (Debug, DebugFast) and is empty otherwise, so no
// Release production image replaces operator new. The switch is in the source, not in
// a per-configuration source list, because the Xcode generator rejects sources that
// vary by configuration.

#if GE_DEBUG_INSTRUMENTATION
#include "Memory/AllocationHook.inl"
#endif
