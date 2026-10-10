// An in-memory stand-in for the engine module: a world of entities whose components are
// byte arrays laid out from a small reflected registry, a heap the facade reads and writes,
// and a log of every call. It enforces nothing about the call contract, so a facade that
// broke the contract would go unnoticed here; the tests assert the facade's own refusals.

import type { Abi, EntityId, FieldKind, Ptr, ReflectionJson, TypeId } from '../src/abi.js';
import { kInvalidEntity } from '../src/abi.js';

interface StubField { name: string; tsName: string; kind: FieldKind; size: number; count: number; enum?: Array<{ name: string; value: number }> }
interface StubComponent { name: string; typeId: bigint; fields: StubField[]; offsets: number[]; size: number }

const kLightTypes = ['Directional', 'Point', 'Spot', 'Ambient', 'Area', 'Volume'].map((name, value) => ({ name, value }));
const kLightUnits = ['Unitless', 'Lux', 'Lumen', 'Candela'].map((name, value) => ({ name, value }));

/** The generator's camelCase for these names: the leading capital lowercased. */
function field(name: string, kind: FieldKind, size: number, count = 1, enumTable?: StubField['enum']): StubField {
    return { name, tsName: name[0].toLowerCase() + name.slice(1), kind, size, count, enum: enumTable };
}

const kSchema: Array<[string, StubField[]]> = [
    ['Name', [field('value', 'String', 64)]],
    ['Parent', [field('parent', 'EntityHandle', 4)]],
    ['Transform', [field('matrix', 'Float', 64, 16)]],
    ['Light', [field('Type', 'UInt32', 4, 1, kLightTypes), field('Color', 'Float', 12, 3), field('Intensity', 'Float', 4),
        field('CastsShadows', 'Bool', 1), field('IntensityUnit', 'UInt32', 4, 1, kLightUnits)]],
    ['Camera', [field('Perspective', 'Bool', 1), field('FovY', 'Float', 4)]],
    ['SkyEnvironment', [field('TimeOfDayHours', 'Float', 4), field('Latitude', 'Float', 4), field('DayOfYear', 'Int32', 4),
        field('NorthHeading', 'Float', 4), field('SunLight', 'EntityHandle', 4)]],
    ['LocalBounds', [field('DynamicObject', 'Bool', 1), field('CastShadows', 'Bool', 1)]],
    ['MeshRenderer', [field('meshId', 'UInt32', 4), field('modelAssetGuid', 'AssetGuid', 16)]],
    ['DDGIVolume', [field('Fit', 'Int32', 4, 1, ['Manual', 'FollowCamera'].map((name, value) => ({ name, value }))),
        field('Intensity', 'Float', 4)]],
];

/** The built-in meshes ge_entity_set_mesh knows and their LocalBounds boxes (center, half extents). */
const kBuiltInMeshes = new Map<string, number[]>([
    ['plane', [0, 0, 0, 0.5, 0, 0.5]], ['cube', [0, 0, 0, 0.5, 0.5, 0.5]],
    ['sphere', [0, 0, 0, 0.5, 0.5, 0.5]], ['capsule', [0, 0, 0, 0.5, 1, 0.5]],
]);

export interface Deferred<T> { promise: Promise<T>; resolve(value: T): void }

export function deferred<T>(): Deferred<T> {
    let resolve!: (value: T) => void;
    const promise = new Promise<T>((r) => { resolve = r; });
    return { promise, resolve };
}

export class StubAbi implements Abi {
    readonly calls: string[] = [];
    engineVersion = '0.1.0';
    /** When set, ge_create waits for it. */
    createGate: Deferred<number> | null = null;
    shutdownGate: Deferred<number> | null = null;
    /** When true, a new load stays Loading until finishLoad; otherwise it finishes on the next update. */
    holdLoads = false;
    /** When true, ge_load_asset refuses the URL (returns kInvalidAsset), as the module does a URL it cannot name. */
    refuseLoads = false;
    /** When true, ge_model_emissive_strength fails, as the module does without a renderer. */
    refuseEmissiveStrength = false;
    /** Called inside ge_tick: the engine's own writes. */
    onTick: (() => void) | null = null;
    lastResize: [number, number, number] | null = null;

    private m_Buffer = new ArrayBuffer(1 << 20);
    private m_Next = 16;
    private m_LastError = '';
    private m_NextEntity = 1;
    private m_NextAsset = 1;
    private readonly m_AssetStatus = new Map<number, number>();
    /** Statuses the next asset update applies: loads finish only inside ge_tick or ge_update_assets. */
    private readonly m_Completions = new Map<number, number>();
    private readonly m_Components = new Map<string, StubComponent>();
    private readonly m_World = new Map<EntityId, Map<bigint, Uint8Array>>();
    /** LocalBounds boxes: center then half extents, local space. */
    private readonly m_Bounds = new Map<EntityId, number[]>();
    private readonly m_Animations = new Map<EntityId, { clips: string[]; clip: string | null; speed: number; paused: boolean }>();

    constructor() {
        kSchema.forEach(([name, fields], index) => {
            const offsets: number[] = [];
            let size = 0;
            // C++ layout: a field of four bytes or more starts on a four-byte boundary, and the
            // component's size is padded to one.
            const align = (at: number, to: number) => Math.ceil(at / to) * to;
            for (const f of fields) {
                size = align(size, f.size / f.count >= 4 ? 4 : 1);
                offsets.push(size);
                size += f.size;
            }
            size = align(size, 4);
            this.m_Components.set(name, { name, typeId: 0x1000n + BigInt(index) * 0x10001n, fields, offsets, size });
        });
    }

    get HEAPU8(): Uint8Array { return new Uint8Array(this.m_Buffer); }

    /** Blocks the facade has allocated in module memory. */
    mallocs = 0;

    _malloc(size: number): Ptr {
        ++this.mallocs;
        return this.allocate(size);
    }

    /** The stub's own module memory (its query chunks), apart from the facade's count. */
    private allocate(size: number): Ptr {
        const ptr = this.m_Next;
        this.m_Next += (size + 15) & ~15;
        return ptr;
    }

    _free(): void {}

    ge_create(selector: Ptr, flags: number): Promise<number> {
        this.calls.push(`ge_create(${this.string(selector)}, ${flags})`);
        return this.createGate ? this.createGate.promise : Promise.resolve(0);
    }

    ge_tick(): number {
        this.calls.push('ge_tick');
        this.onTick?.();
        this.updateAssets();
        return 0;
    }

    ge_resize(width: number, height: number, dpr: number): number {
        this.calls.push('ge_resize');
        this.lastResize = [width, height, dpr];
        return 0;
    }

    ge_shutdown(): Promise<number> {
        this.calls.push('ge_shutdown');
        // The module ends a running query with the engine.
        this.m_Query = null;
        return this.shutdownGate ? this.shutdownGate.promise : Promise.resolve(0);
    }

    ge_load_asset(url: Ptr): number {
        this.calls.push(`ge_load_asset(${this.string(url)})`);
        if (this.refuseLoads) {
            this.fail('the URL was refused');
            return 0;
        }
        const handle = this.m_NextAsset++;
        this.m_AssetStatus.set(handle, 0);
        if (!this.holdLoads) this.m_Completions.set(handle, 1);
        return handle;
    }

    ge_asset_status(handle: number): number {
        this.calls.push(`ge_asset_status(${handle})`);
        const status = this.m_AssetStatus.get(handle) ?? 2;
        if (status === 2) this.fail(`asset ${handle} failed to load`);
        return status;
    }

    /** Lets a held load end, Ready or Failed, at the next asset update. */
    finishLoad(handle: number, ok: boolean): void {
        this.m_Completions.set(handle, ok ? 1 : 2);
    }

    ge_update_assets(): number {
        this.calls.push('ge_update_assets');
        this.updateAssets();
        return 0;
    }

    private updateAssets(): void {
        for (const [handle, status] of this.m_Completions) this.m_AssetStatus.set(handle, status);
        this.m_Completions.clear();
    }

    ge_instantiate_model(_handle: number, parent: EntityId): EntityId {
        this.calls.push('ge_instantiate_model');
        const id = this.ge_entity_create();
        if (parent !== kInvalidEntity) this.ge_entity_parent(id, parent);
        this.ge_component_add(id, this.component('LocalBounds').typeId);
        this.m_Bounds.set(id, [0, 0.5, 0, 0.5, 0.5, 0.5]);
        return id;
    }

    ge_model_emissive_strength(handle: number, strength: number): number {
        this.calls.push(`ge_model_emissive_strength(${handle}, ${strength})`);
        return this.refuseEmissiveStrength ? this.fail('ge_model_emissive_strength needs the renderer') : 0;
    }

    ge_entity_create(): EntityId {
        const id = this.m_NextEntity++;
        this.m_World.set(id, new Map());
        this.ge_component_add(id, this.component('Transform').typeId);
        return id;
    }

    ge_entity_bounds(entity: EntityId, out: Ptr): number {
        if (!this.m_World.has(entity)) { this.fail(`no entity ${entity}`); return -1; }
        const box = this.m_Bounds.get(entity);
        if (!box || !this.hasComponent(entity, 'LocalBounds')) return 0;
        const view = new DataView(this.m_Buffer, out, 24);
        box.forEach((v, i) => view.setFloat32(4 * i, v, true));
        return 1;
    }

    ge_entity_destroy(entity: EntityId): number {
        this.calls.push('ge_entity_destroy');
        return this.m_World.delete(entity) ? 0 : this.fail(`no entity ${entity}`);
    }

    ge_entity_children(entity: EntityId, out: Ptr, capacity: number): number {
        if (!this.m_World.has(entity)) { this.fail(`no entity ${entity}`); return -1; }
        const parentType = this.component('Parent').typeId;
        const children = [...this.m_World].filter(([, components]) => {
            const bytes = components.get(parentType);
            return bytes !== undefined && new DataView(bytes.buffer).getUint32(0, true) === entity;
        }).map(([id]) => id);
        const view = new DataView(this.m_Buffer, out, 4 * capacity);
        children.slice(0, capacity).forEach((id, i) => view.setUint32(4 * i, id, true));
        return children.length;
    }

    ge_entity_set_mesh(entity: EntityId, meshName: Ptr): number {
        const name = this.string(meshName);
        this.calls.push(`ge_entity_set_mesh(${entity}, ${name})`);
        if (!this.m_World.has(entity)) return this.fail(`ge_entity_set_mesh: entity ${entity} does not exist`);
        const box = kBuiltInMeshes.get(name);
        if (!box) return this.fail(`ge_entity_set_mesh: '${name}' is not a built-in mesh; use 'plane', 'cube', 'sphere' or 'capsule'.`);
        for (const component of ['MeshRenderer', 'LocalBounds']) {
            if (!this.hasComponent(entity, component)) this.ge_component_add(entity, this.component(component).typeId);
        }
        this.m_Bounds.set(entity, box);
        return 0;
    }

    ge_entity_parent(entity: EntityId, parent: EntityId): number {
        const parentType = this.component('Parent').typeId;
        if (!this.m_World.get(entity)?.has(parentType)) this.ge_component_add(entity, parentType);
        new DataView(this.bytes(entity, parentType)!.buffer).setUint32(0, parent, true);
        return 0;
    }

    ge_component_add(entity: EntityId, typeId: TypeId): number {
        const components = this.m_World.get(entity);
        const component = this.byType(typeId);
        if (!components || !component) return this.fail(`cannot add ${typeId} to ${entity}`);
        const bytes = new Uint8Array(component.size);
        if (component.name === 'Transform') new Float32Array(bytes.buffer).set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
        if (component.name === 'Parent') new DataView(bytes.buffer).setUint32(0, kInvalidEntity, true);
        if (component.name === 'Camera') new DataView(bytes.buffer).setFloat32(component.offsets[1], 60, true);
        if (component.name === 'Light') new DataView(bytes.buffer).setFloat32(16, 1, true);
        components.set(typeId, bytes);
        return 0;
    }

    ge_component_remove(entity: EntityId, typeId: TypeId): number {
        return this.m_World.get(entity)?.delete(typeId) ? 0 : this.fail('no such component');
    }

    ge_component_has(entity: EntityId, typeId: TypeId): number {
        const components = this.m_World.get(entity);
        if (!components) { this.fail(`no entity ${entity}`); return -1; }
        return components.has(typeId) ? 1 : 0;
    }

    ge_field_get(entity: EntityId, typeId: TypeId, fieldId: number, out: Ptr): number {
        const located = this.locate(entity, typeId, fieldId);
        if (!located) return this.fail('no such field');
        this.HEAPU8.set(located.bytes.subarray(located.offset, located.offset + located.size), out);
        return 0;
    }

    ge_field_set(entity: EntityId, typeId: TypeId, fieldId: number, value: Ptr): number {
        this.calls.push(`ge_field_set(${this.byType(typeId)?.fields[fieldId]?.name})`);
        const located = this.locate(entity, typeId, fieldId);
        if (!located) return this.fail('no such field');
        located.bytes.set(this.HEAPU8.subarray(value, value + located.size), located.offset);
        return 0;
    }

    /** Entities per chunk in the stub's queries: small, so a query spans chunks. */
    readonly queryChunkSize = 3;
    private m_Query: { read: StubComponent[]; write: StubComponent[]; entities: EntityId[]; next: number; chunk: { at: Ptr; entities: EntityId[] } | null } | null = null;

    ge_query_begin(readTypeIds: Ptr, readCount: number, writeTypeIds: Ptr, writeCount: number, outStrides: Ptr): number {
        this.calls.push('ge_query_begin');
        if (this.m_Query) return this.fail('ge_query_begin: another query is still running');
        const typeIds = (at: Ptr, count: number) => Array.from(new BigUint64Array(this.m_Buffer, at, count));
        const read = typeIds(readTypeIds, readCount).map((id) => this.byType(id));
        const write = typeIds(writeTypeIds, writeCount).map((id) => this.byType(id));
        if ([...read, ...write].some((c) => !c)) return this.fail('ge_query_begin: not a component');
        const columns = [...read, ...write] as StubComponent[];
        new Uint32Array(this.m_Buffer, outStrides, columns.length).set(columns.map((c) => c.size));
        const entities = [...this.m_World].filter(([, components]) => columns.every((c) => components.has(c.typeId))).map(([id]) => id);
        this.m_Query = { read: read as StubComponent[], write: write as StubComponent[], entities, next: 0, chunk: null };
        return 0;
    }

    ge_query_next_chunk(outColumns: Ptr, outCount: Ptr): number {
        this.calls.push('ge_query_next_chunk');
        const query = this.m_Query;
        if (!query) return this.fail('ge_query_next_chunk: no query running');
        this.closeChunk();
        if (query.next >= query.entities.length) {
            this.m_Query = null;
            return 0;
        }
        const entities = query.entities.slice(query.next, query.next + this.queryChunkSize);
        query.next += entities.length;
        // The chunk's memory: the entity ids, then each column's components back to back.
        const columns = [...query.read, ...query.write];
        const at = this.allocate(4 * entities.length + columns.reduce((sum, c) => sum + c.size * entities.length, 0));
        new Uint32Array(this.m_Buffer, at, entities.length).set(entities);
        const addresses = [at];
        let next = at + 4 * entities.length;
        for (const column of columns) {
            addresses.push(next);
            entities.forEach((e, i) => this.HEAPU8.set(this.bytes(e, column.typeId)!, next + i * column.size));
            next += column.size * entities.length;
        }
        new Uint32Array(this.m_Buffer, outColumns, addresses.length).set(addresses);
        new Uint32Array(this.m_Buffer, outCount, 1)[0] = entities.length;
        query.chunk = { at, entities };
        return 1;
    }

    ge_query_end(): number {
        this.calls.push('ge_query_end');
        this.closeChunk();
        this.m_Query = null;
        return 0;
    }

    /** True while a query is walking. */
    get queryRunning(): boolean {
        return this.m_Query !== null;
    }

    /**
     * Copies the visited chunk's columns back into the world, read and write alike: the engine
     * hands out its own memory, so a page's write through any address it got lands there.
     */
    private closeChunk(): void {
        const query = this.m_Query, chunk = query?.chunk;
        if (!query || !chunk) return;
        let next = chunk.at + 4 * chunk.entities.length;
        for (const column of [...query.read, ...query.write]) {
            chunk.entities.forEach((e, i) => this.bytes(e, column.typeId)!.set(this.HEAPU8.subarray(next + i * column.size, next + (i + 1) * column.size)));
            next += column.size * chunk.entities.length;
        }
        query.chunk = null;
    }

    /** Makes `entity` an animated model with these clips, at rest. */
    animate(entity: EntityId, clips: string[]): void {
        this.m_Animations.set(entity, { clips, clip: null, speed: 1, paused: false });
    }

    /** The playback an animated model's last ge_animation_play / pause stamped. */
    animation(entity: EntityId): { clips: string[]; clip: string | null; speed: number; paused: boolean } | undefined {
        return this.m_Animations.get(entity);
    }

    ge_animation_clips(entity: EntityId): Ptr {
        const animation = this.m_Animations.get(entity);
        if (!animation) { this.fail(`entity ${entity} is not an animated model`); return 0; }
        return this.writeString(JSON.stringify(animation.clips));
    }

    ge_animation_play(entity: EntityId, clipName: Ptr, speed: number): number {
        const name = this.string(clipName);
        this.calls.push(`ge_animation_play(${name}, ${speed})`);
        const animation = this.m_Animations.get(entity);
        if (!animation) return this.fail(`entity ${entity} is not an animated model`);
        if (!animation.clips.includes(name)) return this.fail(`the model has no clip '${name}'; its clips are ${animation.clips.join(', ')}`);
        Object.assign(animation, { clip: name, speed, paused: false });
        return 0;
    }

    ge_animation_pause(entity: EntityId): number {
        this.calls.push('ge_animation_pause');
        const animation = this.m_Animations.get(entity);
        if (!animation) return this.fail(`entity ${entity} is not an animated model`);
        animation.paused = true;
        return 0;
    }

    ge_reflection_json(): Ptr {
        return this.writeString(this.reflectionJson());
    }

    ge_last_error(): Ptr {
        return this.writeString(this.m_LastError);
    }

    reflectionJson(): string {
        const document: ReflectionJson = {
            engineVersion: this.engineVersion,
            components: [...this.m_Components.values()].map((c) => ({
                name: c.name, typeId: c.typeId.toString(), fields: c.fields.map((f, i) => ({ ...f, offset: c.offsets[i] })),
            })),
        };
        return JSON.stringify(document);
    }

    /** An engine-side write, as physics or the sky would make it. */
    setFloats(entity: EntityId, componentName: string, fieldName: string, values: number[], at = 0): void {
        const component = this.component(componentName);
        const index = component.fields.findIndex((f) => f.name === fieldName);
        const bytes = this.bytes(entity, component.typeId)!;
        const view = new DataView(bytes.buffer);
        values.forEach((v, i) => view.setFloat32(component.offsets[index] + 4 * (at + i), v, true));
    }

    floats(entity: EntityId, componentName: string, fieldName: string): number[] {
        const component = this.component(componentName);
        const index = component.fields.findIndex((f) => f.name === fieldName);
        const bytes = this.bytes(entity, component.typeId)!;
        const view = new DataView(bytes.buffer);
        return Array.from({ length: component.fields[index].size / 4 }, (_, i) => view.getFloat32(component.offsets[index] + 4 * i, true));
    }

    uint32(entity: EntityId, componentName: string, fieldName: string): number {
        const component = this.component(componentName);
        const index = component.fields.findIndex((f) => f.name === fieldName);
        return new DataView(this.bytes(entity, component.typeId)!.buffer).getUint32(component.offsets[index], true);
    }

    hasComponent(entity: EntityId, componentName: string): boolean {
        return this.m_World.get(entity)?.has(this.component(componentName).typeId) ?? false;
    }

    private component(name: string): StubComponent {
        return this.m_Components.get(name)!;
    }

    private byType(typeId: bigint): StubComponent | undefined {
        return [...this.m_Components.values()].find((c) => c.typeId === typeId);
    }

    private bytes(entity: EntityId, typeId: bigint): Uint8Array | undefined {
        return this.m_World.get(entity)?.get(typeId);
    }

    private locate(entity: EntityId, typeId: bigint, fieldId: number) {
        const component = this.byType(typeId);
        const bytes = this.bytes(entity, typeId);
        if (!component || !bytes || fieldId >= component.fields.length) return null;
        return { bytes, offset: component.offsets[fieldId], size: component.fields[fieldId].size };
    }

    private fail(message: string): number {
        this.m_LastError = message;
        return 1;
    }

    private string(ptr: Ptr): string {
        const heap = this.HEAPU8;
        let end = ptr;
        while (heap[end] !== 0) ++end;
        return new TextDecoder().decode(heap.slice(ptr, end));
    }

    private writeString(text: string): Ptr {
        const bytes = new TextEncoder().encode(text);
        const ptr = this._malloc(bytes.length + 1);
        this.HEAPU8.set(bytes, ptr);
        this.HEAPU8[ptr + bytes.length] = 0;
        return ptr;
    }
}
