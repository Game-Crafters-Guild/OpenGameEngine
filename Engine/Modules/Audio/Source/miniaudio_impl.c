#if defined(_MSC_VER)
#pragma warning(push)
// miniaudio is third-party; silence known warnings in the implementation TU.
// C4456: declaration hides previous local declaration (miniaudio internal scopes)
// C4245: signed/unsigned mismatch in return conversions (miniaudio internal APIs)
// C4098: void function returning a value (miniaudio ARM64 atomic macro bug)
#pragma warning(disable : 4456 4245 4098)
#endif

// macOS: Use static linking for CoreAudio frameworks instead of runtime linking
// This avoids notarization issues and ensures proper framework access
#if defined(__APPLE__)
#define MA_NO_RUNTIME_LINKING
#endif

#define MINIAUDIO_IMPLEMENTATION

// We intentionally build miniaudio as part of the Audio static library so consumers
// do not need to manage a separate dependency. The vcpkg port provides the header.
#include <miniaudio.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
