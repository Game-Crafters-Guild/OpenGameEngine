// A shipping game's state: NDEBUG defined, whatever this test binary's own
// configuration is.
#if !defined(NDEBUG)
#define NDEBUG 1
#endif

#define GE_ABI_LAYOUT_PROBE_FUNCTION MeasureEnginePublicTypeLayoutWithNDebug
#include "EngineHostAbiLayoutProbeBody.inl"
