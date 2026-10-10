#pragma once
// PhysicsABI.h - Subsystem-scoped C ABI for Physics-related interop
//
// This header defines a small, versioned interface table for Physics operations.
// It intentionally lives outside ScriptingABI.h so subsystem bindings can stay
// modular (e.g. ECS, Physics, Rendering, etc.) without growing the general ABI surface.
//
// Base ABI types/macros (GE_API, GE_CDECL, GE_Result, GE_Handle, etc.) are
// shared with ScriptingABI.h.

#include "Scripting/ScriptingABI.h"

#ifdef __cplusplus
extern "C" {
#endif

// -------- Physics ABI Versioning --------
#define GE_PHYSICS_ABI_VERSION_ENCODE(major, minor) ((((major) & 0xFFFFu) << 16) | ((minor) & 0xFFFFu))
#define GE_PHYSICS_ABI_VERSION_MAJOR(v)             (((v) >> 16) & 0xFFFFu)
#define GE_PHYSICS_ABI_VERSION_MINOR(v)             ((v) & 0xFFFFu)

// Current Physics ABI version exposed via GE_Physics_GetInterface
static const uint32_t GE_PHYSICS_ABI_VERSION_CURRENT = GE_PHYSICS_ABI_VERSION_ENCODE(1u, 0u);

// Opaque physics resource handle (matches GenerationalVector::Handle::HandleType).
// 0 is reserved to mean "none/invalid" at the ABI boundary.
typedef uint64_t GE_Physics_Handle;

typedef struct GE_Physics_Vector3
{
    float x;
    float y;
    float z;
} GE_Physics_Vector3;

// Function pointer types for the Physics interface table.
typedef GE_Result (GE_CDECL *GE_PhysicsABI_GetDefaultWorld_Fn)(GE_Handle* outWorld);
typedef GE_Result (GE_CDECL *GE_PhysicsABI_StepWorld_Fn)(GE_Handle world, float deltaTime, int32_t collisionSteps);

typedef struct GE_Physics_Interface_v1
{
    uint32_t sizeBytes;  // sizeof(GE_Physics_Interface_v1)
    uint32_t abiVersion; // GE_PHYSICS_ABI_VERSION_CURRENT

    GE_PhysicsABI_GetDefaultWorld_Fn GetDefaultWorld;
    GE_PhysicsABI_StepWorld_Fn       StepWorld;
} GE_Physics_Interface_v1;

// Bootstrap entry: returns a pointer to the requested Physics interface version.
GE_API GE_Result GE_CDECL GE_Physics_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes);

// Direct exports (optional convenience; still routed through GE_Physics_GetInterface for multi-binding).
GE_API GE_Result GE_CDECL GE_PhysicsABI_GetDefaultWorld(GE_Handle* outWorld);
GE_API GE_Result GE_CDECL GE_PhysicsABI_StepWorld(GE_Handle world, float deltaTime, int32_t collisionSteps);

#ifdef __cplusplus
} // extern "C"
#endif

