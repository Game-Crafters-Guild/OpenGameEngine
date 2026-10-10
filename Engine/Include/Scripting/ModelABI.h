#pragma once

#include "Scripting/ScriptingABI.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The kinds of object in a model's source file whose extras the model keeps. The values are
// GameEngine::ModelObjectKind's.
typedef enum GE_ModelObjectKind
{
    GE_ModelObjectKind_Scene = 0,
    GE_ModelObjectKind_Node = 1,
    GE_ModelObjectKind_Mesh = 2,
    GE_ModelObjectKind_Material = 3,
    GE_ModelObjectKind_Animation = 4,
} GE_ModelObjectKind;

// Reads the extras JSON text the loaded model modelGuidBytes kept at import for its object of `kind`
// (a GE_ModelObjectKind) named nameUtf8 (nameLength bytes, no NUL terminator needed; an unnamed object
// is named by the empty string). The text is the file's own bytes for an extras object, array, number or
// literal; an extras value that is a JSON string reads without its quotes. Same buffer contract as
// GE_UIElement_GetValueText: copies min(*outLen, bufferLen) bytes into buffer, no NUL terminator, and
// always reports the text's full byte length in *outLen; buffer may be null only when bufferLen is 0.
// *outLen is 0 when the model kept no extras for the object: it has none, no object of that kind has the
// name, an earlier object of that kind has it, or the text was over the import's bounds (256 KiB per
// object; 1 MiB per model, counting each object's name with its extras).
//
// Main thread only: a hot reload replaces the model's extras there. A call from another thread is refused
// with GE_Result_Fail and logged once; a host that never marked a main thread (a standalone test host)
// accepts every thread. GE_Result_NotFound when no loaded model has the GUID; GE_Result_InvalidArg, checked
// before the model is looked up, for a kind outside GE_ModelObjectKind, a null GUID or outLen, a null name
// with a nonzero length, a negative bufferLen, or a null buffer with a nonzero bufferLen; GE_Result_Fail
// also for an internal error.
GE_API GE_Result GE_CDECL GE_Model_GetExtras(const uint8_t modelGuidBytes[16],
                                             uint32_t kind,
                                             const char* nameUtf8,
                                             uint32_t nameLength,
                                             char* buffer,
                                             int32_t bufferLen,
                                             int32_t* outLen);

#ifdef __cplusplus
} // extern "C"
#endif
