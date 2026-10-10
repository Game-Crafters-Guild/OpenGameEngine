#pragma once
// ECSABI.h - Subsystem-scoped C ABI for ECS-related interop
//
// This header defines a small, versioned interface table for ECS operations.
// It intentionally lives outside ScriptingABI.h so subsystem bindings can stay
// modular (e.g. ECS, Rendering, etc.) without growing the general ABI surface.
//
// Base ABI types/macros (GE_API, GE_CDECL, GE_Result, GE_Handle, etc.) are
// shared with ScriptingABI.h.

#include "Scripting/ScriptingABI.h"

#ifdef __cplusplus
extern "C" {
#endif

// -------- ECS ABI Versioning --------
#define GE_ECS_ABI_VERSION_ENCODE(major, minor) ((((major) & 0xFFFFu) << 16) | ((minor) & 0xFFFFu))
#define GE_ECS_ABI_VERSION_MAJOR(v)             (((v) >> 16) & 0xFFFFu)
#define GE_ECS_ABI_VERSION_MINOR(v)             ((v) & 0xFFFFu)

// Current ECS ABI version exposed via GE_ECS_GetInterface.
// v2.0: legacy per-entity query family (QueryCreate/QueryDestroy/QueryNext)
// deleted — CachedQuery chunk iteration is the sole query surface — and
// component type ids widened to the full 64-bit ComponentTypeId (consteval
// Hash64 identity; the former uint32 fields truncated it and could not
// round-trip).
// v2.1 (minor, appended): RegisterBlobComponentWithSchema — a blob (C#)
// component registers a reflected field table alongside its name+size, making
// it first-class for scene save/load, the inspector, and hot-reload layout
// migration through the same ComponentFieldRegistry machinery native
// components use.
// v2.2 (minor, appended): GetComponentDisabledTypeId — the type id of the tag
// that switches a component off, so managed code enables and disables
// components through the deferred add/remove commands. The cached query
// follows the native enable model: it excludes disabled entities and every
// required component that is switched off, and ArchetypeHasComponent reports
// a switched-off optional component as absent.
// v2.3 (minor, appended): SetEntityEnabled — switches an entity on or off
// through World::SetEntityEnabledImmediate, so both activity tags move together
// and an entity switched back on under a parent that is on returns to queries
// at once; queued while structural changes are deferred.
static const uint32_t GE_ECS_ABI_VERSION_CURRENT = GE_ECS_ABI_VERSION_ENCODE(2u, 3u);

// Opaque entity identifier (packed index+version in engine ECS).
// 0 is reserved to mean "none/invalid" at the ABI boundary.
typedef uint32_t GE_ECS_Entity;

// Component type identity: the engine's 64-bit ComponentTypeId (FNV-1a Hash64
// of the normalized type name). 0 is reserved to mean "none/invalid".
typedef uint64_t GE_ECS_ComponentTypeId;

typedef enum GE_ECS_PrimitiveType
{
    GE_ECS_Primitive_Cube = 0,
    GE_ECS_Primitive_Sphere = 1,
    GE_ECS_Primitive_Capsule = 2,
    GE_ECS_Primitive_Plane = 3
} GE_ECS_PrimitiveType;

// Local-space transform matrix (column-major, matching Components::Transform.matrix).
typedef struct GE_ECS_Transform
{
    float matrix[16];
} GE_ECS_Transform;

// Function pointer types for the ECS interface table.
typedef GE_Result (GE_CDECL *GE_ECSABI_GetPrimaryWorld_Fn)(GE_Handle* outWorld);
typedef GE_Result (GE_CDECL *GE_ECSABI_GetEntityCount_Fn)(GE_Handle world, int32_t* outCount);
typedef GE_Result (GE_CDECL *GE_ECSABI_CreateEmptyEntity_Fn)(GE_Handle world,
                                                             const char* nameUtf8,
                                                             uint32_t nameLen,
                                                             GE_ECS_Entity parentEntity,
                                                             GE_ECS_Entity* outEntity);
typedef GE_Result (GE_CDECL *GE_ECSABI_CreatePrimitive_Fn)(GE_Handle world,
                                                           GE_ECS_PrimitiveType primitive,
                                                           const char* nameUtf8,
                                                           uint32_t nameLen,
                                                           GE_ECS_Entity parentEntity,
                                                           GE_ECS_Entity* outEntity);
typedef GE_Result (GE_CDECL *GE_ECSABI_IsEntityValid_Fn)(GE_Handle world, GE_ECS_Entity entity, int32_t* outValid);
typedef GE_Result (GE_CDECL *GE_ECSABI_DestroyEntity_Fn)(GE_Handle world, GE_ECS_Entity entity);
typedef GE_Result (GE_CDECL *GE_ECSABI_SetName_Fn)(GE_Handle world, GE_ECS_Entity entity, const char* nameUtf8, uint32_t nameLen);
typedef GE_Result (GE_CDECL *GE_ECSABI_GetName_Fn)(GE_Handle world, GE_ECS_Entity entity, char* outNameUtf8, uint32_t outCap, uint32_t* outLen);
typedef GE_Result (GE_CDECL *GE_ECSABI_GetTransform_Fn)(GE_Handle world, GE_ECS_Entity entity, GE_ECS_Transform* outTransform);
typedef GE_Result (GE_CDECL *GE_ECSABI_SetTransform_Fn)(GE_Handle world, GE_ECS_Entity entity, const GE_ECS_Transform* transform);

typedef GE_Result (GE_CDECL *GE_ECSABI_RegisterBlobComponent_Fn)(const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes, GE_ECS_ComponentTypeId* outTypeId);
typedef GE_Result (GE_CDECL *GE_ECSABI_GetComponentTypeIdByName_Fn)(const char* nameUtf8, uint32_t nameLen, GE_ECS_ComponentTypeId* outTypeId);
typedef GE_Result (GE_CDECL *GE_ECSABI_SetComponentBytes_Fn)(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, const uint8_t* data, uint32_t dataLen);
typedef GE_Result (GE_CDECL *GE_ECSABI_GetComponentBytes_Fn)(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, uint8_t* outData, uint32_t outCap, uint32_t* outLen);
typedef GE_Result (GE_CDECL *GE_ECSABI_RemoveComponent_Fn)(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId);

// v1.3: Cached query for chunk-based iteration (source-generated ECS systems)
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryCreate_Fn)(GE_Handle world,
                                                              const GE_ECS_ComponentTypeId* requiredTypeIds,
                                                              uint32_t requiredCount,
                                                              const GE_ECS_ComponentTypeId* excludedTypeIds,
                                                              uint32_t excludedCount,
                                                              GE_Handle* outQuery);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryDestroy_Fn)(GE_Handle query);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryReset_Fn)(GE_Handle query,
                                                             uint32_t* outArchetypeCount);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetArchetypeInfo_Fn)(GE_Handle query,
                                                                        uint32_t archetypeIndex,
                                                                        uint32_t* outEntityCount,
                                                                        uint32_t* outChunkCount);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetChunkData_Fn)(GE_Handle query,
                                                                    uint32_t archetypeIndex,
                                                                    uint32_t chunkIndex,
                                                                    GE_ECS_ComponentTypeId componentTypeId,
                                                                    void** outData,
                                                                    uint32_t* outCount,
                                                                    uint32_t* outStride);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetChunkEntityIds_Fn)(GE_Handle query,
                                                                         uint32_t archetypeIndex,
                                                                         uint32_t chunkIndex,
                                                                         uint32_t** outEntityIds,
                                                                         uint32_t* outCount);

// v1.3: Component presence check
typedef GE_Result (GE_CDECL *GE_ECSABI_HasComponent_Fn)(GE_Handle world,
                                                         GE_ECS_Entity entity,
                                                         GE_ECS_ComponentTypeId componentTypeId,
                                                         int32_t* outHas);

// v1.3: Structural change fencing for managed system ticks
typedef GE_Result (GE_CDECL *GE_ECSABI_SetDeferStructuralChanges_Fn)(GE_Handle world, int32_t defer);
typedef GE_Result (GE_CDECL *GE_ECSABI_FlushDeferredCommands_Fn)(GE_Handle world);

// v1.4: Per-archetype component presence check for optional component support
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryArchetypeHasComponent_Fn)(
    GE_Handle query,
    uint32_t archetypeIndex,
    GE_ECS_ComponentTypeId componentTypeId,
    int32_t* outHas);

// v1.5: Entity command buffer — raw entity creation and explicit deferred commands
typedef GE_Result (GE_CDECL *GE_ECSABI_CreateEntityRaw_Fn)(
    GE_Handle world, GE_ECS_Entity* outEntity);
typedef GE_Result (GE_CDECL *GE_ECSABI_DeferCommand_Fn)(
    GE_Handle world, uint32_t commandType, GE_ECS_Entity entity,
    GE_ECS_ComponentTypeId componentTypeId, const void* data, uint32_t dataLen);

// v2.1: One reflected field of a blob component, marshaled from the managed
// source generator. fieldType carries the ECS FieldTypeId underlying value
// (a stable append-only taxonomy — see ECS/Reflection.h); offset/size describe
// the field's byte span within the component and MUST match the blittable
// layout the managed side reads/writes through Set/GetComponentBytes and chunk
// spans (LayoutKind.Sequential managed layout — the same bytes both sides touch).
typedef struct GE_ECS_FieldDesc
{
    const char* nameUtf8;  // field name; not required to be null-terminated
    uint32_t    nameLen;
    uint32_t    offset;    // byte offset within the component
    uint32_t    size;      // total byte span (elementSize * count for arrays)
    uint16_t    fieldType; // ECS::FieldTypeId underlying value; 0 (Unknown) is invalid
    uint16_t    reserved;  // must be 0
} GE_ECS_FieldDesc;

// v2.1: Register a blob component together with its reflected field table.
// Subsumes RegisterBlobComponent for schema-carrying components:
//   * unknown name          -> registers the blob and its field table
//   * known name, same size -> replaces the field table (idempotent re-load)
//   * known name, new size or moved/renamed fields -> hot-reload relayout:
//     migrates placed instances in the primary world (same-named fields are
//     preserved, new fields default to zero) and updates the recorded size —
//     the case plain RegisterBlobComponent must keep rejecting.
// The field array and all name buffers may be temporaries; the engine copies.
typedef GE_Result (GE_CDECL *GE_ECSABI_RegisterBlobComponentWithSchema_Fn)(
    const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes,
    const GE_ECS_FieldDesc* fields, uint32_t fieldCount,
    GE_ECS_ComponentTypeId* outTypeId);

// v2.2: The ComponentDisabled tag of a registered component (the id the native
// Entity::SetEnabled<T> writes), registering the tag on first use. Adding the
// tag to an entity switches the component off; removing it switches it back on.
// A component declared NotToggleable (Transform, Name, the hierarchy links) has
// no on/off state: the call succeeds and writes 0, and such a component is
// always on. An unknown id and an enable-state tag itself give NotFound.
typedef GE_Result (GE_CDECL *GE_ECSABI_GetComponentDisabledTypeId_Fn)(
    GE_ECS_ComponentTypeId componentTypeId, GE_ECS_ComponentTypeId* outDisabledTypeId);

// v2.3: Switch an entity on or off — ECS::Disabled and the derived
// DisabledInHierarchy together, except that switching on keeps the derived tag
// while the entity's parent is still off (World::SetEntityEnabledImmediate).
// Queued while structural changes are deferred and applied at the flush;
// immediate otherwise.
typedef GE_Result (GE_CDECL *GE_ECSABI_SetEntityEnabled_Fn)(GE_Handle world, GE_ECS_Entity entity, int32_t enabled);

// v1.7: Change-signaling chunk enumeration (design M11/Q7).
// outChunkVersion (nullable) receives the requested component column's
// write-grant version for the chunk — a uint64 monotonic stamp to compare
// against a consumer gate sampled from World::GetGlobalSystemVersion().
// The V2 entry returns a MUTABLE span and records a write grant (stamps);
// the ReadOnly entry returns the const column and never stamps — managed
// READS must use it so they stop over-stamping.
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetChunkDataV2_Fn)(GE_Handle query,
                                                                      uint32_t archetypeIndex,
                                                                      uint32_t chunkIndex,
                                                                      GE_ECS_ComponentTypeId componentTypeId,
                                                                      void** outData,
                                                                      uint32_t* outCount,
                                                                      uint32_t* outStride,
                                                                      uint64_t* outChunkVersion);
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetChunkDataReadOnly_Fn)(GE_Handle query,
                                                                            uint32_t archetypeIndex,
                                                                            uint32_t chunkIndex,
                                                                            GE_ECS_ComponentTypeId componentTypeId,
                                                                            const void** outData,
                                                                            uint32_t* outCount,
                                                                            uint32_t* outStride,
                                                                            uint64_t* outChunkVersion);

// v1.6: Slice-based component data access (heterogeneous chunk support).
// Unlike GetChunkData which indexes by chunk, these index by entity offset
// within an archetype, returning the maximum contiguous slice from that offset.
// With colocated storage, all components share the same chunk boundaries,
// so the slice always covers a contiguous range within one chunk.
typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetComponentSlice_Fn)(
    GE_Handle query,
    uint32_t archetypeIndex,
    GE_ECS_ComponentTypeId componentTypeId,
    uint32_t entityOffset,
    void** outData,
    uint32_t* outCount,
    uint32_t* outStride);

typedef GE_Result (GE_CDECL *GE_ECSABI_CachedQueryGetEntityIdSlice_Fn)(
    GE_Handle query,
    uint32_t archetypeIndex,
    uint32_t entityOffset,
    uint32_t maxCount,
    uint32_t** outEntityIds,
    uint32_t* outCount);

typedef struct GE_ECS_Interface_v2
{
    uint32_t sizeBytes;  // sizeof(GE_ECS_Interface_v2)
    uint32_t abiVersion; // GE_ECS_ABI_VERSION_CURRENT

    GE_ECSABI_GetPrimaryWorld_Fn   GetPrimaryWorld;
    GE_ECSABI_GetEntityCount_Fn    GetEntityCount;
    GE_ECSABI_CreateEmptyEntity_Fn CreateEmptyEntity;
    GE_ECSABI_CreatePrimitive_Fn   CreatePrimitive;

    // Entity ops
    GE_ECSABI_IsEntityValid_Fn     IsEntityValid;
    GE_ECSABI_DestroyEntity_Fn     DestroyEntity;

    // Common engine components
    GE_ECSABI_SetName_Fn           SetName;
    GE_ECSABI_GetName_Fn           GetName;
    GE_ECSABI_GetTransform_Fn      GetTransform;
    GE_ECSABI_SetTransform_Fn      SetTransform;

    // Runtime-defined components (blob) and type-erased component access
    GE_ECSABI_RegisterBlobComponent_Fn    RegisterBlobComponent;
    GE_ECSABI_GetComponentTypeIdByName_Fn GetComponentTypeIdByName;
    GE_ECSABI_SetComponentBytes_Fn        SetComponentBytes;
    GE_ECSABI_GetComponentBytes_Fn        GetComponentBytes;
    GE_ECSABI_RemoveComponent_Fn          RemoveComponent;

    // v1.3: Cached chunk queries for high-performance iteration
    GE_ECSABI_CachedQueryCreate_Fn              CachedQueryCreate;
    GE_ECSABI_CachedQueryDestroy_Fn             CachedQueryDestroy;
    GE_ECSABI_CachedQueryReset_Fn               CachedQueryReset;
    GE_ECSABI_CachedQueryGetArchetypeInfo_Fn    CachedQueryGetArchetypeInfo;
    GE_ECSABI_CachedQueryGetChunkData_Fn        CachedQueryGetChunkData;
    GE_ECSABI_CachedQueryGetChunkEntityIds_Fn   CachedQueryGetChunkEntityIds;
    GE_ECSABI_HasComponent_Fn                   HasComponent;
    GE_ECSABI_SetDeferStructuralChanges_Fn      SetDeferStructuralChanges;
    GE_ECSABI_FlushDeferredCommands_Fn          FlushDeferredCommands;

    // v1.4: Per-archetype component presence check for optional component support
    GE_ECSABI_CachedQueryArchetypeHasComponent_Fn CachedQueryArchetypeHasComponent;

    // v1.5: Entity command buffer
    GE_ECSABI_CreateEntityRaw_Fn   CreateEntityRaw;
    GE_ECSABI_DeferCommand_Fn      DeferCommand;

    // v1.6: Slice-based iteration for heterogeneous chunk support
    GE_ECSABI_CachedQueryGetComponentSlice_Fn CachedQueryGetComponentSlice;
    GE_ECSABI_CachedQueryGetEntityIdSlice_Fn  CachedQueryGetEntityIdSlice;

    // v1.7: Change-signaling chunk enumeration (chunkVersion out param) and
    // the read-only span entry (managed reads stop over-stamping — Q7/M11)
    GE_ECSABI_CachedQueryGetChunkDataV2_Fn       CachedQueryGetChunkDataV2;
    GE_ECSABI_CachedQueryGetChunkDataReadOnly_Fn CachedQueryGetChunkDataReadOnly;

    // v2.1: blob component registration with a reflected field table
    GE_ECSABI_RegisterBlobComponentWithSchema_Fn RegisterBlobComponentWithSchema;

    // v2.2: the tag that switches a component off
    GE_ECSABI_GetComponentDisabledTypeId_Fn GetComponentDisabledTypeId;

    // v2.3: the entity switch (World::SetEntityEnabledImmediate)
    GE_ECSABI_SetEntityEnabled_Fn SetEntityEnabled;
} GE_ECS_Interface_v2;

// Bootstrap entry: returns a pointer to the requested ECS interface version.
GE_API GE_Result GE_CDECL GE_ECS_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes);

// -------- Host ECS state sharing --------
// When the host (EXE) and GameEngine.Native.dll both statically link Engine.lib,
// each gets its own copy of the ComponentRegistry maps. Call this once at
// startup to redirect the DLL's registry to the host's, ensuring shared
// handlers and shared name/blob component metadata.
//
// Note: ComponentTypeId is now a consteval Hash64 of the type name (Phase 1b),
// so there is no allocator to share — type IDs are compile-time constants
// identical across DLLs by construction. The redirect now only covers
// ComponentRegistry. Pass the results of ComponentRegistry::Get*Ptr() from the
// host module. Pass all NULL to revert to DLL-local state.
//
// Required initialization order (single-threaded, before any managed code runs):
//   1. GE_SetHostEcsState(...)           — share ComponentRegistry
//   2. GE_ScriptingInitialize(...)       — start managed runtime
GE_API GE_Result GE_CDECL GE_SetHostEcsState(void* registryComponents,
                                               void* registryNames,
                                               void* registryHandlers);

// Direct exports (optional convenience; still routed through GE_ECS_GetInterface for multi-binding).
// Use the ECSABI_ prefix to avoid colliding with the legacy ECS example exports
// already present in ScriptingABI.
GE_API GE_Result GE_CDECL GE_ECSABI_GetPrimaryWorld(GE_Handle* outWorld);
GE_API GE_Result GE_CDECL GE_ECSABI_GetEntityCount(GE_Handle world, int32_t* outCount);
GE_API GE_Result GE_CDECL GE_ECSABI_CreateEmptyEntity(GE_Handle world,
                                                      const char* nameUtf8,
                                                      uint32_t nameLen,
                                                      GE_ECS_Entity parentEntity,
                                                      GE_ECS_Entity* outEntity);
GE_API GE_Result GE_CDECL GE_ECSABI_CreatePrimitive(GE_Handle world,
                                                   GE_ECS_PrimitiveType primitive,
                                                   const char* nameUtf8,
                                                   uint32_t nameLen,
                                                   GE_ECS_Entity parentEntity,
                                                   GE_ECS_Entity* outEntity);

GE_API GE_Result GE_CDECL GE_ECSABI_IsEntityValid(GE_Handle world, GE_ECS_Entity entity, int32_t* outValid);
GE_API GE_Result GE_CDECL GE_ECSABI_DestroyEntity(GE_Handle world, GE_ECS_Entity entity);
GE_API GE_Result GE_CDECL GE_ECSABI_SetName(GE_Handle world, GE_ECS_Entity entity, const char* nameUtf8, uint32_t nameLen);
GE_API GE_Result GE_CDECL GE_ECSABI_GetName(GE_Handle world, GE_ECS_Entity entity, char* outNameUtf8, uint32_t outCap, uint32_t* outLen);
GE_API GE_Result GE_CDECL GE_ECSABI_GetTransform(GE_Handle world, GE_ECS_Entity entity, GE_ECS_Transform* outTransform);
GE_API GE_Result GE_CDECL GE_ECSABI_SetTransform(GE_Handle world, GE_ECS_Entity entity, const GE_ECS_Transform* transform);

GE_API GE_Result GE_CDECL GE_ECSABI_RegisterBlobComponent(const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes, GE_ECS_ComponentTypeId* outTypeId);
GE_API GE_Result GE_CDECL GE_ECSABI_RegisterBlobComponentWithSchema(
    const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes,
    const GE_ECS_FieldDesc* fields, uint32_t fieldCount,
    GE_ECS_ComponentTypeId* outTypeId);
GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentTypeIdByName(const char* nameUtf8, uint32_t nameLen, GE_ECS_ComponentTypeId* outTypeId);
GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentDisabledTypeId(GE_ECS_ComponentTypeId componentTypeId, GE_ECS_ComponentTypeId* outDisabledTypeId);
GE_API GE_Result GE_CDECL GE_ECSABI_SetEntityEnabled(GE_Handle world, GE_ECS_Entity entity, int32_t enabled);
GE_API GE_Result GE_CDECL GE_ECSABI_SetComponentBytes(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, const uint8_t* data, uint32_t dataLen);
GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentBytes(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, uint8_t* outData, uint32_t outCap, uint32_t* outLen);
GE_API GE_Result GE_CDECL GE_ECSABI_RemoveComponent(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId);

// v1.3: Cached chunk queries
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryCreate(GE_Handle world,
                                                       const GE_ECS_ComponentTypeId* requiredTypeIds,
                                                       uint32_t requiredCount,
                                                       const GE_ECS_ComponentTypeId* excludedTypeIds,
                                                       uint32_t excludedCount,
                                                       GE_Handle* outQuery);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryDestroy(GE_Handle query);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryReset(GE_Handle query,
                                                      uint32_t* outArchetypeCount);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetArchetypeInfo(GE_Handle query,
                                                                  uint32_t archetypeIndex,
                                                                  uint32_t* outEntityCount,
                                                                  uint32_t* outChunkCount);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkData(GE_Handle query,
                                                              uint32_t archetypeIndex,
                                                              uint32_t chunkIndex,
                                                              GE_ECS_ComponentTypeId componentTypeId,
                                                              void** outData,
                                                              uint32_t* outCount,
                                                              uint32_t* outStride);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkEntityIds(GE_Handle query,
                                                                   uint32_t archetypeIndex,
                                                                   uint32_t chunkIndex,
                                                                   uint32_t** outEntityIds,
                                                                   uint32_t* outCount);
GE_API GE_Result GE_CDECL GE_ECSABI_HasComponent(GE_Handle world,
                                                   GE_ECS_Entity entity,
                                                   GE_ECS_ComponentTypeId componentTypeId,
                                                   int32_t* outHas);
GE_API GE_Result GE_CDECL GE_ECSABI_SetDeferStructuralChanges(GE_Handle world, int32_t defer);
GE_API GE_Result GE_CDECL GE_ECSABI_FlushDeferredCommands(GE_Handle world);

// v1.4: Per-archetype component presence check
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryArchetypeHasComponent(GE_Handle query,
                                                                       uint32_t archetypeIndex,
                                                                       GE_ECS_ComponentTypeId componentTypeId,
                                                                       int32_t* outHas);

// v1.5: Entity command buffer
GE_API GE_Result GE_CDECL GE_ECSABI_CreateEntityRaw(GE_Handle world, GE_ECS_Entity* outEntity);
GE_API GE_Result GE_CDECL GE_ECSABI_DeferCommand(GE_Handle world, uint32_t commandType,
                                                   GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId,
                                                   const void* data, uint32_t dataLen);

// v1.6: Slice-based iteration
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetComponentSlice(
    GE_Handle query, uint32_t archetypeIndex, GE_ECS_ComponentTypeId componentTypeId,
    uint32_t entityOffset, void** outData, uint32_t* outCount, uint32_t* outStride);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetEntityIdSlice(
    GE_Handle query, uint32_t archetypeIndex, uint32_t entityOffset,
    uint32_t maxCount, uint32_t** outEntityIds, uint32_t* outCount);

// v1.7: Change-signaling chunk enumeration + read-only span (see typedefs)
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkDataV2(
    GE_Handle query, uint32_t archetypeIndex, uint32_t chunkIndex,
    GE_ECS_ComponentTypeId componentTypeId, void** outData, uint32_t* outCount,
    uint32_t* outStride, uint64_t* outChunkVersion);
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkDataReadOnly(
    GE_Handle query, uint32_t archetypeIndex, uint32_t chunkIndex,
    GE_ECS_ComponentTypeId componentTypeId, const void** outData, uint32_t* outCount,
    uint32_t* outStride, uint64_t* outChunkVersion);

#ifdef __cplusplus
} // extern "C"
#endif


