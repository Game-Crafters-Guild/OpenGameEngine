// The web library's public API: the hand-written half of the declarations.
//
// Every object is a handle on an engine entity and every property is a reflected component
// field, read from the engine on access and written to it on assignment; the library keeps
// no copy of the world. The component types themselves (`Light`, `Camera`, ...) are in
// components.d.ts, which imports the shared value types below.
//
// Conventions: left-handed, +X right, +Y up, +Z forward; meters; degrees for every angle in
// this file (the component views keep the engine's radians); seconds for time steps.

export * from './components.js';

/** A reflected component, passed to `entity.get / set / add / remove / has`. */
export interface ComponentType<T extends object = object> {
    /** The reflected component name (`Light`). */
    readonly name: string;
    /** The engine's ComponentTypeId; null until an engine has been created. */
    readonly typeId: bigint | null;
    /** Type-only marker carrying the component's fields; never present at runtime. */
    readonly __values?: T;
}

/** Marks a component every entity always has (`Transform`): it cannot be added or removed. */
export interface NoAdd {
    readonly noAdd: true;
}

/** A component `entity.add` and `entity.remove` accept. */
export type AddableComponentType<T extends object = object> = ComponentType<T> & { readonly noAdd?: false };

/**
 * A live view of one component on one entity. Every property read calls into the engine and
 * every assignment writes through to it; nothing is cached, so a change made by the engine
 * (physics, animation, the sky) is what the next read returns. Tuple and object fields read
 * as fresh values: assign the whole value to change them.
 */
export type ComponentView<T extends object> = T;

/** A 3D vector value: meters for positions. */
export interface Vec3 {
    x: number;
    y: number;
    z: number;
}

/** A linear RGBA color, each channel 0..1 unless a field says otherwise. */
export interface Color {
    r: number;
    g: number;
    b: number;
    a: number;
}

/** A reference to an asset by its GUID (32 lowercase hexadecimal digits). */
export interface AssetRef {
    readonly guid: string;
}

/** A plain 3D vector owned by the page (for example `OrbitControls.target`). */
export declare class Vector3 implements Vec3 {
    constructor(x?: number, y?: number, z?: number);
    x: number;
    y: number;
    z: number;
    set(x: number, y: number, z: number): this;
    copy(v: Vec3): this;
}

/**
 * A live vector inside a component: reading `x` reads the engine, assigning `x` writes it.
 * `set` and `copy` write all three components in one engine call.
 */
export interface Vector3View extends Vec3 {
    x: number;
    y: number;
    z: number;
    set(x: number, y: number, z: number): this;
    copy(v: Vec3): this;
}

/** A live rotation as Euler angles in degrees, the editor inspector's X, Y, Z convention. */
export interface EulerView extends Vector3View {}

/** A live unit quaternion (x, y, z, w). */
export interface QuaternionView {
    x: number;
    y: number;
    z: number;
    w: number;
    set(x: number, y: number, z: number, w: number): this;
}

/**
 * The entity's Transform, local to its parent (world space for a root entity). Each
 * property reads the engine's matrix on access; nothing is cached.
 */
export interface TransformView {
    /** Meters. */
    readonly position: Vector3View;
    /** Degrees. */
    readonly rotation: EulerView;
    readonly quaternion: QuaternionView;
    readonly scale: Vector3View;
    /** The parent entity, or null for a root. Assigning reparents; null makes it a root. */
    parent: Entity | null;
    /**
     * The entities whose parent is this one, read from the engine at access. A loaded model's
     * meshes are its children (or deeper descendants), each with a MeshRenderer.
     */
    readonly children: Entity[];
    /** Rotates about the entity's own X axis by `degrees`; a positive turn takes +Y toward +Z. */
    rotateX(degrees: number): this;
    /** Rotates about the entity's own Y axis by `degrees`; a positive turn takes +Z toward +X. */
    rotateY(degrees: number): this;
    /** Rotates about the entity's own Z axis by `degrees`; a positive turn takes +X toward +Y. */
    rotateZ(degrees: number): this;
    /**
     * Rotates about the world's up axis (+Y) through the entity's position by `degrees`, whatever
     * the entity's own rotation and its parents' rotations and mirroring: a turntable or a
     * character's yaw. A positive turn takes +Z toward +X, clockwise seen from above. Under a
     * parent with non-uniform scale the entity stays unsheared, so the turn is close to, not
     * exactly, a turn about world up. Under a parent scaled to zero or flattened (a zero scale
     * on any axis) the turn does nothing.
     */
    rotateWorldY(degrees: number): this;
}

/**
 * An axis-aligned box in the entity's parent space, read from its LocalBounds at access.
 * Hand-written: the box has no reflected kind, so the engine module reads it for the facade.
 */
export interface Bounds {
    readonly center: Vector3;
    readonly size: Vector3;
    readonly min: Vector3;
    readonly max: Vector3;
}

/**
 * A loaded model's animation clips, played one at a time and looping. Works on the entity
 * `scene.load` returned for a skinned model with clips; on any other entity every member throws.
 * A change takes effect on the next frame.
 */
export interface AnimationView {
    /** The model's clip names, in the model's order. */
    readonly clips: string[];
    /**
     * Plays the clip named `clipName`, looping, at `speed` times its authored rate (default 1; 0
     * holds the pose). Playing the clip that is already playing keeps its time: call it again to
     * change the speed, or to resume after `pause()`. Another clip starts from its beginning.
     * Throws naming the model's clips when it has no clip by that name.
     */
    play(clipName: string, options?: { speed?: number }): void;
    /** Holds the model at its current pose; `play` resumes. */
    pause(): void;
}

/** A handle on one engine entity. */
export interface Entity {
    /** The engine's entity id. */
    readonly id: number;
    /** The entity's name, or '' when it has none. */
    readonly name: string;
    readonly transform: TransformView;
    /**
     * The entity's LocalBounds transformed into its parent's space, or null when it has none.
     */
    readonly bounds: Bounds | null;
    /** The entity's animation clips, when it is a skinned model with clips. */
    readonly animation: AnimationView;
    /** A live view of `component`, or undefined when the entity does not have it. */
    get<T extends object>(component: ComponentType<T>): ComponentView<T> | undefined;
    /**
     * Adds `component` when the entity lacks it, then writes `values` over the engine's
     * defaults (creates or updates). Unnamed fields keep their current values.
     */
    set<T extends object>(component: ComponentType<T>, values: Partial<T>): ComponentView<T>;
    /** Adds `component` with the engine's defaults. Throws when the entity already has it; use `set` to update. */
    add<T extends object>(component: AddableComponentType<T>): ComponentView<T>;
    /** Removes `component`; does nothing when the entity lacks it. */
    remove(component: AddableComponentType): void;
    has(component: ComponentType): boolean;
    /** Destroys the entity and its children. Using the handle afterwards throws. */
    destroy(): void;
}

/** A loaded model's root entity: its bounds enclose the whole model. */
export interface Model extends Entity {
    readonly bounds: Bounds;
}

/** The sun: the directional light the sky drives. */
export interface SunView {
    readonly entity: Entity;
    /** Lux on a surface facing the sun; 100 000 is a clear noon. */
    intensity: number;
    /** Linear RGB, 0..1. While the sky drives the sun's color, the sky overwrites this each frame. */
    color: [number, number, number];
    castsShadows: boolean;
}

/** The physical sky. The sun's direction follows from these fields. */
export interface SkyView {
    readonly entity: Entity;
    /** Local solar time in hours, 0..24; 12 is solar noon. */
    timeOfDay: number;
    /** Degrees north (positive) or south of the equator, -90..90. */
    latitude: number;
    /** 1 = 1 January, 172 = 21 June, 355 = 21 December. */
    dayOfYear: number;
    /** Degrees clockwise seen from above where north points: 0 = +Z, 90 = +X. */
    northHeading: number;
}

/** A mesh the engine builds in, for `scene.create({ mesh })`. */
export type BuiltInMesh = 'plane' | 'cube' | 'sphere' | 'capsule';

/** What `scene.create` makes. */
export interface CreateOptions {
    name?: string;
    /** A built-in mesh the entity draws, with its MeshRenderer and LocalBounds. */
    mesh?: BuiltInMesh;
}

/** The engine's world. */
export interface Scene {
    /**
     * Fetches a glTF or GLB model from a URL and instantiates it; resolves to the model's root
     * entity, whose bounds enclose the whole model. Loads run one at a time: a second call
     * waits for the first. Called from an `onFrame` callback, the load starts after that frame.
     * The engine keeps running while a load fetches; the promise resolves on the first frame
     * after the model is ready, and also completes before `engine.run()`. Each call places
     * another copy of the model; a URL is fetched once, and later calls reuse what it loaded.
     * `options.onProgress` reports the file's download in the `'model'` phase; a URL already
     * loaded reports nothing.
     */
    load(url: string, options?: LoadOptions): Promise<Model>;
    /**
     * Creates an entity with an identity transform: empty, or named (a string, or `options.name`),
     * or drawing a built-in mesh with the engine's default material (`options.mesh`). The meshes
     * are unit-sized at the origin: the plane is 1 m square in XZ facing +Y, the cube and the
     * sphere fit a 1 m box, the capsule is 1 m wide and 2 m tall; scale them through the transform.
     */
    create(nameOrOptions?: string | CreateOptions): Entity;
    /** The camera the canvas renders from. */
    readonly camera: Entity;
    readonly sun: SunView;
    readonly sky: SkyView;
    /**
     * The entities that have every component in `read` and in `options.write`, walked a chunk
     * at a time with `forEachChunk`: one engine call per chunk of entities rather than one per
     * entity per field. Make the query once and run it every frame. A component in `write` is
     * written in place and the engine sees the write (a moved `Transform` moves the entity on
     * the next frame); a component in `read` is handed out as a copy. Every entity a walk visits
     * counts as changed in each `write` component, assigned or not, so list a component the page
     * only reads in `read`. Name each component once; at most 16 components.
     */
    query(read: readonly ComponentType[], options?: { write?: readonly ComponentType[] }): Query;
}

/** A query over the entities that have a set of components (`scene.query`). */
export interface Query {
    /**
     * Calls `visit` once for each chunk of entities the query matches. While it runs the world
     * holds still: creating or destroying entities, adding or removing components, reparenting
     * and `engine` frames are refused with an error; make them after `forEachChunk` returns.
     */
    forEachChunk(visit: (chunk: QueryChunk) => void): void;
}

/** Up to a chunk of the entities a query matches, as `forEachChunk` hands them to its callback. */
export interface QueryChunk {
    /** How many entities the chunk holds. */
    readonly count: number;
    /** The chunk's entity at `index` (0 to `count - 1`). */
    entity(index: number): Entity;
    /**
     * The field `field` of `component` for every entity in the chunk, as 32-bit floats: element
     * `k` of entity `i` is `values[offset + i * stride + k]`. For a component the query writes,
     * `values` is the engine's own memory, and assigning to it changes the entities. For a
     * component it reads, `values` is a copy. Valid until the callback returns or calls into the
     * engine. The field must hold floats (`Float`, `Vec2`, `Vec3`, `Vec4`, `Quat`, `Color`, `Mat4`;
     * a `Mat4` is column-major).
     */
    floats(component: ComponentType, field: string): FloatColumn;
}

/** Floats strided across a chunk's entities (`QueryChunk.floats`). */
export interface FloatColumn {
    readonly values: Float32Array;
    /** The index of entity 0's first element. */
    readonly offset: number;
    /** Elements from one entity's field to the next's. */
    readonly stride: number;
}

/**
 * Which engine build `Engine.create` loads. `'auto'` (the default): the threaded build when
 * the page is cross-origin isolated, else the single-threaded one. `'single'`: the
 * single-threaded build, which works on any host. `'multi'`: the threaded build, which needs a
 * cross-origin isolated page (COOP/COEP headers or the service worker shim).
 */
export type ThreadsOption = 'auto' | 'single' | 'multi';

export interface EngineOptions {
    /** The canvas the engine draws into. Its CSS size is tracked; the drawing buffer follows it times devicePixelRatio. */
    canvas: HTMLCanvasElement;
    threads?: ThreadsOption;
    /** URL of the folder holding opengine-core.st/mt.js and .wasm; defaults to the folder of the library's own module. */
    coreUrl?: string;
    /**
     * Reports the engine's download while `Engine.create` runs: the `'pack'` phase (the engine
     * pack, when the folder serves it as parts) and the `'wasm'` phase (the engine module).
     * Both phases report their first progress, with `loaded` 0 and their totals, before either
     * reports bytes, so a page can add them up from the start (the package knows its own
     * files' sizes, so these phases always have a total).
     */
    onProgress?: (progress: LoadProgress) => void;
}

/** What a download is fetching: the engine pack, the engine module, or a model. */
export type LoadPhase = 'pack' | 'wasm' | 'model';

/**
 * A download's progress in bytes: network bytes when the server sends Content-Length (a file
 * it compresses counts its compressed size), otherwise the file's decompressed bytes.
 * `loaded / total` is the fraction done either way; show it as a percentage, since the bytes
 * are not always the network's. `total` is 0 while a phase's size is unknown: a compressed
 * model, whose decompressed size the library cannot know beforehand, has none until it has
 * arrived. `loaded` never decreases, and a phase's last report has `loaded` equal to `total`.
 */
export interface LoadProgress {
    phase: LoadPhase;
    loaded: number;
    total: number;
}

export interface LoadOptions {
    /** Reports the model's download in the `'model'` phase. */
    onProgress?: (progress: LoadProgress) => void;
    /**
     * Multiplies the brightness of the model's emissive materials, 0 or more (default 1: as the
     * file gives it). The materials belong to the URL, so every copy of the model shows the
     * strength of its latest load.
     */
    emissiveStrength?: number;
}

/** The running engine. */
export interface Engine {
    readonly scene: Scene;
    /**
     * Starts ticking: once per animation frame while the tab is visible, every 33 ms while it
     * is hidden.
     */
    run(): void;
    /** Stops ticking; `run` resumes. */
    pause(): void;
    /**
     * Calls `callback` once per frame before the engine's systems run, with the frame's time
     * step in seconds. Returns a function that unsubscribes.
     */
    onFrame(callback: (dt: number) => void): () => void;
    /** Re-reads the canvas size and devicePixelRatio. Called automatically when the canvas resizes. */
    resize(): void;
    /**
     * Stops the engine and releases the GPU device and the module's memory; resolves when the
     * engine is gone. Every handle throws from the moment it is called.
     */
    dispose(): Promise<void>;
}

export interface EngineStatic {
    /**
     * Loads the engine module, brings up WebGPU on the canvas and creates a world with a
     * camera, a sun and a sky. Rejects when the browser has no WebGPU, and when the library
     * and the engine module come from different releases.
     */
    create(options: EngineOptions): Promise<Engine>;
}
export declare const Engine: EngineStatic;

/** Mouse orbit, zoom and pan around a target, written into a camera entity's transform. */
export interface OrbitControls {
    /** The point the camera orbits and looks at. */
    readonly target: Vector3;
    /** Meters from the target. */
    distance: number;
    /**
     * The camera's elevation over the target, degrees: negative looks down, held within ±89.
     * Setting it drops any drag still settling toward another elevation. Throws InvalidArgument
     * for a value that is not a finite number.
     */
    pitch: number;
    /** Meters. */
    minDistance: number;
    /** Meters. */
    maxDistance: number;
    /** Smooths motion; on by default. */
    enableDamping: boolean;
    /** Fraction of the remaining motion applied per 1/60 s, 0..1. */
    dampingFactor: number;
    rotateSpeed: number;
    zoomSpeed: number;
    panSpeed: number;
    /** Writes the camera's transform. Called every frame automatically; call it after changing a property by hand. */
    update(dt?: number): void;
    /** Removes the pointer and wheel listeners and stops updating. */
    dispose(): void;
}

export interface OrbitControlsStatic {
    /**
     * Orbits `camera` with the pointer on `domElement`: left drag rotates, right drag or
     * shift-drag pans, the wheel zooms. Starts from the camera's current position, orbiting
     * the origin.
     */
    new (camera: Entity, domElement: HTMLElement): OrbitControls;
}
export declare const OrbitControls: OrbitControlsStatic;

/**
 * What went wrong, so a page can branch on it. `'NoWebGPU'`: the browser has no WebGPU.
 * `'VersionMismatch'`: the library and the engine module are from different releases.
 * `'Engine'`: the engine refused a call; the message is the engine's. `'CallOrder'`: a call
 * broke the library's call order (for example a field write while the engine is starting).
 * `'Disposed'`: the engine or the entity was disposed or destroyed. `'InvalidArgument'`: a
 * component, field or value the engine does not have (a misspelled field, an unknown
 * enumerator).
 */
export type OpenEngineErrorCode = 'NoWebGPU' | 'VersionMismatch' | 'Engine' | 'CallOrder' | 'Disposed' | 'InvalidArgument';

/** Every error the library throws or rejects with. The message states the fix. */
export declare class OpenEngineError extends Error {
    constructor(code: OpenEngineErrorCode, message: string);
    readonly code: OpenEngineErrorCode;
}
