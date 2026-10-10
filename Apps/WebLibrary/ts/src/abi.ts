// The engine module's C ABI as the facade calls it: the contract the WebLibrary target's
// `extern "C"` exports and its JavaScript binding implement (design: workbench
// designs/web/web-api.md section 3.1).
//
// Shapes:
// - Entities are the engine's 32-bit EntityHandle ids; kInvalidEntity means none.
// - Component types are the 64-bit ComponentTypeId hashes ComponentFieldRegistry keys on,
//   passed as bigint. Fields are indices into that type's registered field table, in the
//   order ge_reflection_json lists them.
// - Strings cross as NUL-terminated UTF-8 in module memory: the facade writes arguments into
//   its scratch arena (allocated with _malloc); a returned string pointer stays valid until
//   the next ABI call.
// - Field values cross as the field's bytes in module memory, `size` bytes as the reflection
//   reports them, little-endian: Bool 1 byte; integers and Float/Double native; Vec2/3/4,
//   Quat (x, y, z, w), Color (r, g, b, a) and Mat4 (column-major) as float32 runs; AssetGuid
//   16 bytes; EntityHandle uint32; String a char array padded with NULs; a C array is its
//   elements back to back. The facade reads and writes them through typed-array views on
//   HEAPU8.buffer, re-read after every call because a heap growth replaces the buffer.
// - Every call that can fail returns an error code (kOk on success) or, for calls that
//   return an id or handle, the invalid value; ge_last_error then holds the message.
//
// The call contract under ASYNCIFY: ge_create and ge_shutdown suspend (they wait on the
// browser: the engine pack's fetch and the WebGPU device, the GPU's last work), and the binding
// runs both with ccall's { async: true }. No call may run while one is suspended: the facade
// never makes one (contract.ts), and the module refuses one with the message (AbiCallScope).
// A model load does not suspend: ge_load_asset starts the browser's fetch and returns at once,
// and the facade polls ge_asset_status until the load is Ready or Failed, while frames keep
// running. A load moves only inside ge_tick and ge_update_assets: the module hands a fetched
// file to the asset manager there and reads its outcome there (UrlAssetLoads::Advance), so a
// status read changes nothing. Before engine.run(), when no frame ticks, the facade calls
// ge_update_assets on its own poll instead. On the single-threaded build the asset manager reads
// and decodes inside that update, so one update carries the model's decode (about 70 ms for a
// 9 MB GLB); ge_instantiate_model decodes and uploads its textures on the main thread.

import type { TrackDownload } from './progress.js';

/** The engine's 32-bit entity id. */
export type EntityId = number;
/** The 64-bit component type hash. */
export type TypeId = bigint;
/** The index of a field in its component's reflected field table. */
export type FieldId = number;
/** A byte offset into the module's memory. */
export type Ptr = number;

/** ECS kInvalidEntity: no entity (ge_entity_parent's "make it a root"). */
export const kInvalidEntity: EntityId = 0xffffffff;
/** The success code of every call that returns an error code. */
export const kOk = 0;
/** ge_load_asset's failure handle. */
export const kInvalidAsset = 0;

/** What ge_asset_status returns. */
export const AssetStatus = {
    Loading: 0,
    Ready: 1,
    Failed: 2,
} as const;

/** The functions the WebLibrary module exports, as the facade calls them. */
export interface Abi {
    /** The module's memory. A heap growth replaces the buffer: never keep a view across a call. */
    readonly HEAPU8: Uint8Array;
    _malloc(size: number): Ptr;
    _free(ptr: Ptr): void;

    /**
     * Creates the runtime host on the canvas `canvasSelector` names (a CSS selector, UTF-8)
     * with an empty primary world, waits for the WebGPU device, and loads the default render
     * pipeline (Web.rendergraph). The host renders the canvas from the primary world's Camera
     * entity; the facade creates exactly one, right after this call, and the host takes the
     * first Camera the world holds when it has several. No flag is defined yet: pass 0.
     * Suspends, as ge_shutdown does.
     */
    ge_create(canvasSelector: Ptr, flags: number): Promise<number>;
    /** Runs one frame: input, the world's systems, the asset manager's update, the URL loads' step (where a load moves), the render graph. */
    ge_tick(): number;
    /**
     * Runs the asset manager's update and the URL loads' step alone, so loads move while no
     * frame ticks. The facade calls it only on its own load poll before engine.run() or after
     * engine.pause(), never in a frame that also calls ge_tick.
     */
    ge_update_assets(): number;
    /** The canvas's CSS size and the device pixel ratio; the engine sizes the drawing buffer to their product. */
    ge_resize(width: number, height: number, devicePixelRatio: number): number;
    /**
     * Stops the host and releases the device; every later call fails. Suspends while the GPU
     * finishes its last work: no other call may run until it resolves.
     */
    ge_shutdown(): Promise<number>;

    /**
     * Starts fetching and loading the asset at `url` (absolute, UTF-8) and returns its handle
     * at once, or kInvalidAsset when the load cannot start. Does not suspend. A URL loads once:
     * for a URL already loading or loaded it returns that load's handle, with no new fetch.
     */
    ge_load_asset(url: Ptr): number;
    /**
     * One of AssetStatus. A pure query: it changes nothing, and the status moves only inside
     * ge_tick or ge_update_assets. After Failed, ge_last_error holds the reason.
     */
    ge_asset_status(handle: number): number;
    /**
     * Instantiates a loaded model under `parent` (kInvalidEntity for a root); returns the
     * root entity, which carries a LocalBounds enclosing the whole model, or kInvalidEntity.
     */
    ge_instantiate_model(handle: number, parent: EntityId): EntityId;
    /**
     * Sets the emission of a loaded model's materials to `strength` (0 or more) times the
     * brightness its file gives them. The materials belong to the load, shared by every instance
     * of it, and each ge_instantiate_model of the load sets them back to the file's brightness.
     */
    ge_model_emissive_strength(handle: number, strength: number): number;

    /** Creates an entity with an identity Transform; returns its id or kInvalidEntity. */
    ge_entity_create(): EntityId;
    /**
     * Writes the entity's LocalBounds box to `out` as six float32: the center, then the half
     * extents, in meters in the entity's local space. Returns 1 when written, 0 when the
     * entity has no LocalBounds, negative on failure.
     */
    ge_entity_bounds(entity: EntityId, out: Ptr): number;
    /** Destroys the entity and its children. */
    ge_entity_destroy(entity: EntityId): number;
    /**
     * Writes the ids of the entity's direct children to `out` as uint32, at most `capacity` of
     * them, and returns how many it has (more than `capacity`: call again with a larger buffer);
     * negative on failure.
     */
    ge_entity_children(entity: EntityId, out: Ptr, capacity: number): number;
    /** Reparents the entity; kInvalidEntity makes it a root. */
    ge_entity_parent(entity: EntityId, parent: EntityId): number;
    /**
     * Gives the entity a built-in mesh ('plane', 'cube', 'sphere', 'capsule'; `meshName` is a
     * UTF-8 string) with the default material: a MeshRenderer and the mesh's LocalBounds.
     */
    ge_entity_set_mesh(entity: EntityId, meshName: Ptr): number;

    /** Adds the component with its defaults. */
    ge_component_add(entity: EntityId, typeId: TypeId): number;
    ge_component_remove(entity: EntityId, typeId: TypeId): number;
    /** 1 when the entity has the component, 0 when not, negative on failure. */
    ge_component_has(entity: EntityId, typeId: TypeId): number;

    /** Copies the field's bytes to `out`. */
    ge_field_get(entity: EntityId, typeId: TypeId, fieldId: FieldId, out: Ptr): number;
    /** Copies the field's bytes from `value`. */
    ge_field_set(entity: EntityId, typeId: TypeId, fieldId: FieldId, value: Ptr): number;

    /**
     * Starts a query over the entities that have every component of `readTypeIds` and
     * `writeTypeIds` (uint64 arrays), and writes each column's stride in bytes (reads first,
     * then writes) to `outStrides` as uint32. One query runs at a time; until it ends, the calls
     * that change the world's structure (entity create, destroy and parent, component add and
     * remove, model instantiation) and ge_tick and ge_update_assets fail; ge_shutdown ends it.
     */
    ge_query_begin(readTypeIds: Ptr, readCount: number, writeTypeIds: Ptr, writeCount: number, outStrides: Ptr): number;
    /**
     * Advances the query to its next chunk: writes the address of the chunk's entity ids
     * (uint32) and then each column's address (reads first, then writes) to `outColumns` as
     * uint32, and the chunk's entity count to `outCount`; returns 1. Returns 0, ending the query,
     * after the last chunk. A write column is the chunk's own memory and is marked written; a
     * read column must not be written.
     */
    ge_query_next_chunk(outColumns: Ptr, outCount: Ptr): number;
    /** Ends the running query early; nothing happens when none runs. */
    ge_query_end(): number;

    /**
     * The clip names of the animated model `entity` (the root scene.load returned for a skinned
     * model with clips) as a JSON array of strings in the model's order; null (0) on failure. The
     * Animator component is not reflected, so animation has these three calls of its own.
     */
    ge_animation_clips(entity: EntityId): Ptr;
    /**
     * Plays the clip `clipName` (UTF-8) on the animated model, looping, at `speed` (0 or more)
     * times its authored rate. The clip already playing keeps its time (a speed change, or a
     * resume after ge_animation_pause); another clip starts from its beginning. The next tick
     * applies it.
     */
    ge_animation_play(entity: EntityId, clipName: Ptr, speed: number): number;
    /** Holds the animated model at its current pose; the next tick applies it. */
    ge_animation_pause(entity: EntityId): number;

    /** The live registry as JSON (ReflectionJson), editor-only components left out. */
    ge_reflection_json(): Ptr;
    /** The message of the last failed call. */
    ge_last_error(): Ptr;
}

/** The FieldTypeId names (ECS/Reflection.h), as ge_reflection_json spells them. */
export type FieldKind =
    | 'Unknown' | 'Bool' | 'Int8' | 'Int16' | 'Int32' | 'Int64' | 'UInt8' | 'UInt16' | 'UInt32' | 'UInt64'
    | 'Float' | 'Double' | 'Vec2' | 'Vec3' | 'Vec4' | 'Quat' | 'Mat4' | 'Color' | 'AssetGuid'
    | 'EntityHandle' | 'String' | 'Bytes';

/**
 * ge_reflection_json's document: the live registry. Its components and fields are the ones the
 * scanner's --emit-json lists for the same build (the WebLibraryAbiSmoke drift case), but its
 * shape is this one: typeId, field offsets and sizes, and inline enum tables, which the
 * scanner's file lacks.
 */
export interface ReflectionJson {
    /** The engine's release, e.g. "0.3.1". */
    engineVersion: string;
    components: Array<{
        /** The unqualified name (`Light`). */
        name: string;
        /** The ComponentTypeId in decimal: JSON numbers cannot carry 64 bits. */
        typeId: string;
        fields: Array<{
            /** The C++ member name (`Intensity`). */
            name: string;
            /** The member name in the TypeScript types (`intensity`), as the generated components.d.ts spells it. */
            tsName: string;
            kind: FieldKind;
            /** The field's byte offset in the component. */
            offset: number;
            /** Bytes, for the whole field (all elements of an array). */
            size: number;
            /** Elements: 1, or the C array's length. */
            count: number;
            /** The enumerator table of an enum field. */
            enum?: Array<{ name: string; value: number }>;
            readOnly?: boolean;
        }>;
    }>;
}

/** The two engine builds: single-threaded and threaded. */
export type CoreBuild = 'st' | 'mt';

/**
 * The module the facade imports to obtain an Abi: `opengine-core-binding.js`, beside the
 * engine's glue files. It instantiates `opengine-core.<build>.js` and adapts its exports,
 * reporting the downloads through `track`; `wasmBytes` is the build's wasm file size, when known.
 */
export interface CoreBindingModule {
    loadCore(options: { build: CoreBuild; coreUrl: string; canvas: HTMLCanvasElement; wasmBytes?: number; track: TrackDownload }): Promise<Abi>;
}
