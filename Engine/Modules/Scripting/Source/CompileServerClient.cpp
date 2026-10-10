// Wrapper to build the legacy Jobs-based CompileServerClient implementation
// into the Scripting module without duplicating code. This keeps the
// implementation in one place (Engine/Source/Jobs/CompileServerClient.cpp)
// while making Scripting the owning module for linkage purposes.
#include "../../../Source/Jobs/CompileServerClient.cpp"

