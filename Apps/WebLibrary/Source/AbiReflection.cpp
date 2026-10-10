// The live component registry as a page reads it: ge_reflection_json
// (Apps/WebLibrary/ts/src/abi.ts).

#include "AbiErrors.h"
#include "AbiLifecycle.h"
#include "PageReflection.h"

#include <emscripten/emscripten.h>

using namespace GameEngine::WebLibrary;

extern "C"
{

/// The reflected components as JSON (abi.ts's ReflectionJson), editor-only ones left out; null
/// on failure. The text stays valid until the next call that returns a string.
EMSCRIPTEN_KEEPALIVE const char* ge_reflection_json()
{
    const AbiCallScope scope("ge_reflection_json");
    if (scope.Refused() || RefuseAfterShutdown("ge_reflection_json"))
        return nullptr;
    return ReturnString(BuildReflectionJson());
}

} // extern "C"
