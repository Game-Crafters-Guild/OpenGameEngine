#pragma once

#include <cstdint>

#include "Scripting/ScriptingABI.h" // for GE_API export macro

namespace GameEngine {
#ifdef _DEBUG
// Test-only seam: force CoreBridge.Initialize to return a specific code
// Use INT32_MAX to clear the override and allow normal behavior.
void SetCoreBridgeInitResultOverrideForTests(int32_t code);

// Test-only seam: reset the native logger bridge used by GE_Log so tests can
// validate GE_LOGFILE behavior in isolation.
GE_API void ResetNativeLoggerBridgeForTests();
#endif
}

