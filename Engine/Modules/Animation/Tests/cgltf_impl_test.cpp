// Test-local cgltf implementation. The Animation library only includes
// cgltf.h declarations (the actual symbols are provided by Engine's own
// cgltf_impl.cpp at runtime). When Animation tests link Animation.lib
// without Engine, this translation unit supplies the missing symbols.
//
// Defining CGLTF_IMPLEMENTATION exactly once per executable is the
// canonical cgltf integration pattern.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
