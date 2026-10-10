// The editor module compiles the runtime module's option helpers directly
// (enum<->string mapping, defaults, sanitize/hash) — one source of truth
// inside one package, no cross-DLL linking between the package's two halves.
// Resolved through the sibling Runtime module's root, which the editor build
// wiring puts on this module's include path.
#include "EZTree/EZTreeOptions.cpp"
