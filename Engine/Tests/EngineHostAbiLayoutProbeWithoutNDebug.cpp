// The engine's Debug and DebugFast state: NDEBUG undefined, whatever this test
// binary's own configuration is.
#if defined(NDEBUG)
#undef NDEBUG
#endif

#define GE_ABI_LAYOUT_PROBE_FUNCTION MeasureEnginePublicTypeLayoutWithoutNDebug
#include "EngineHostAbiLayoutProbeBody.inl"
