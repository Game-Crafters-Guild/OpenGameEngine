// The facade's one door to the engine module: the contract-guarded ABI, the scratch arena
// strings and field values cross through, the reflected registry, and the conversion of a
// failed call into an OpenEngineError carrying the engine's message.

import { kOk, type Abi, type EntityId, type Ptr, type ReflectionJson } from './abi.js';
import { CallContract, guardAbi } from './contract.js';
import { OpenEngineError } from './errors.js';
import { decodeField, encodeField } from './field-codec.js';
import { kQueryBlockBytes } from './query.js';
import { Reflection, type ComponentInfo, type FieldInfo } from './reflection.js';

const kMinimumScratchBytes = 1024;

export class Bridge {
    readonly contract = new CallContract();
    /** Every `ge_` call goes through this guarded view. */
    readonly abi: Abi;
    private readonly m_Raw: Abi;
    private m_Reflection: Reflection | null = null;
    private m_Scratch: Ptr = 0;
    private m_ScratchSize = 0;
    private m_QueryBlock: Ptr = 0;

    constructor(raw: Abi) {
        this.m_Raw = raw;
        this.abi = guardAbi(raw, this.contract);
    }

    get reflection(): Reflection {
        if (!this.m_Reflection) throw new Error('the registry is read after ge_create');
        return this.m_Reflection;
    }

    /** Reads ge_reflection_json; returns the JSON text for the release check. */
    loadReflection(): string {
        const text = this.readString(this.abi.ge_reflection_json());
        this.m_Reflection = new Reflection(JSON.parse(text) as ReflectionJson);
        return text;
    }

    /** A scratch block of at least `size` bytes; valid until the next call to scratch. */
    scratch(size: number): Ptr {
        if (size > this.m_ScratchSize) {
            if (this.m_Scratch !== 0) this.m_Raw._free(this.m_Scratch);
            this.m_ScratchSize = Math.max(size, kMinimumScratchBytes);
            this.m_Scratch = this.m_Raw._malloc(this.m_ScratchSize);
        }
        return this.m_Scratch;
    }

    /**
     * The block a running query's calls use, allocated once for the module's lifetime. Not the
     * scratch arena: a query callback's own calls may reallocate that between two chunks.
     */
    queryBlock(): Ptr {
        if (this.m_QueryBlock === 0) this.m_QueryBlock = this.m_Raw._malloc(kQueryBlockBytes);
        return this.m_QueryBlock;
    }

    /** Writes `text` as NUL-terminated UTF-8 into the scratch arena. */
    writeString(text: string): Ptr {
        const bytes = new TextEncoder().encode(text);
        const ptr = this.scratch(bytes.length + 1);
        const heap = this.m_Raw.HEAPU8;
        heap.set(bytes, ptr);
        heap[ptr + bytes.length] = 0;
        return ptr;
    }

    readString(ptr: Ptr): string {
        const heap = this.m_Raw.HEAPU8;
        let end = ptr;
        while (heap[end] !== 0) ++end;
        return new TextDecoder().decode(heap.slice(ptr, end));
    }

    /** Shuts the engine down and waits for it; the contract must already be marked disposed. */
    async shutdown(): Promise<void> {
        this.check(await this.m_Raw.ge_shutdown(), 'engine.dispose');
    }

    /** Throws the engine's message when `code` is not kOk. */
    check(code: number, call: string): void {
        if (code !== kOk) throw this.failure(call);
    }

    failure(call: string): OpenEngineError {
        return new OpenEngineError('Engine', `${call} failed: ${this.readString(this.abi.ge_last_error())}`);
    }

    hasComponent(entity: EntityId, component: ComponentInfo): boolean {
        const result = this.abi.ge_component_has(entity, component.typeId);
        if (result < 0) throw this.failure(`has(${component.name})`);
        return result === 1;
    }

    getField(entity: EntityId, component: ComponentInfo, field: FieldInfo): unknown {
        const ptr = this.scratch(field.size);
        this.check(this.abi.ge_field_get(entity, component.typeId, field.id, ptr), `reading ${component.name}.${field.jsName}`);
        const heap = this.m_Raw.HEAPU8;
        return decodeField(field, new DataView(heap.buffer, heap.byteOffset + ptr, field.size), 0);
    }

    /** The entity's local LocalBounds box (center, half extents), or null when it has none. */
    entityBounds(entity: EntityId): { center: [number, number, number]; half: [number, number, number] } | null {
        const ptr = this.scratch(24);
        const result = this.abi.ge_entity_bounds(entity, ptr);
        if (result < 0) throw this.failure('entity.bounds');
        if (result === 0) return null;
        const heap = this.m_Raw.HEAPU8;
        const floats = new DataView(heap.buffer, heap.byteOffset + ptr, 24);
        const read = (first: number): [number, number, number] =>
            [floats.getFloat32(4 * first, true), floats.getFloat32(4 * first + 4, true), floats.getFloat32(4 * first + 8, true)];
        return { center: read(0), half: read(3) };
    }

    /** The ids of the entity's direct children. */
    entityChildren(entity: EntityId): EntityId[] {
        let capacity = 16;
        for (;;) {
            const ptr = this.scratch(4 * capacity);
            const count = this.abi.ge_entity_children(entity, ptr, capacity);
            if (count < 0) throw this.failure('entity.children');
            if (count <= capacity) {
                const heap = this.m_Raw.HEAPU8;
                return Array.from(new Uint32Array(heap.buffer, heap.byteOffset + ptr, count));
            }
            capacity = count;
        }
    }

    /** The field's bytes for `value`, encoded on the JavaScript side so a bad value throws before any write. */
    encode(field: FieldInfo, value: unknown): Uint8Array {
        const bytes = new Uint8Array(field.size);
        encodeField(field, value, new DataView(bytes.buffer), 0);
        return bytes;
    }

    setFieldBytes(entity: EntityId, component: ComponentInfo, field: FieldInfo, bytes: Uint8Array): void {
        const ptr = this.scratch(bytes.length);
        this.m_Raw.HEAPU8.set(bytes, ptr);
        this.check(this.abi.ge_field_set(entity, component.typeId, field.id, ptr), `writing ${component.name}.${field.jsName}`);
    }

    setField(entity: EntityId, component: ComponentInfo, field: FieldInfo, value: unknown): void {
        this.setFieldBytes(entity, component, field, this.encode(field, value));
    }
}
