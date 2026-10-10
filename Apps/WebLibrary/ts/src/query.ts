// scene.query: the entities that have a set of components, walked a chunk at a time over the
// engine's own memory (design: workbench designs/web/web-api.md 3.4). One ge_query_next_chunk
// call serves a chunk of entities; the page reads and writes their fields as typed arrays.

import type { ComponentType, Entity, FloatColumn, Query, QueryChunk } from '../opengine.js';
import { kOk, type EntityId, type FieldKind } from './abi.js';
import type { Bridge } from './bridge.js';
import { OpenEngineError } from './errors.js';
import type { ComponentInfo } from './reflection.js';

const kFloatKinds = new Set<FieldKind>(['Float', 'Vec2', 'Vec3', 'Vec4', 'Quat', 'Color', 'Mat4']);
/** The most components one query takes (the module's limit, ge_query_begin). */
export const kMaxQueryColumns = 16;
/** Bytes of the block a query's calls use: type ids, strides, column addresses, the count. */
export const kQueryBlockBytes = 8 * kMaxQueryColumns + 4 * kMaxQueryColumns + 4 * (kMaxQueryColumns + 1) + 4;

/** What a query needs from its scene. */
export interface QueryOwner {
    readonly bridge: Bridge;
    entityFor(id: EntityId): Entity;
}

/** One query column: a component, whether the query writes it, and its stride in bytes. */
interface Column {
    readonly info: ComponentInfo;
    readonly write: boolean;
    stride: number;
}

export class QueryImpl implements Query {
    private readonly m_Owner: QueryOwner;
    /** Reads first, then writes: the order ge_query_begin and ge_query_next_chunk use. */
    private readonly m_Columns: Column[];
    private readonly m_ReadCount: number;

    constructor(owner: QueryOwner, read: readonly ComponentType[], write: readonly ComponentType[]) {
        this.m_Owner = owner;
        const reflection = owner.bridge.reflection;
        const columns = [...read.map((c) => ({ c, write: false })), ...write.map((c) => ({ c, write: true }))];
        if (columns.length === 0) throw new OpenEngineError('InvalidArgument', 'scene.query needs at least one component.');
        if (columns.length > kMaxQueryColumns) {
            throw new OpenEngineError('InvalidArgument',
                `scene.query takes at most ${kMaxQueryColumns} components; this one lists ${columns.length}.`);
        }
        const names = new Set<string>();
        this.m_Columns = columns.map(({ c, write: isWrite }) => {
            if (names.has(c.name)) {
                throw new OpenEngineError('InvalidArgument',
                    `scene.query lists ${c.name} twice; name it once, in read or in write (a written component can also be read).`);
            }
            names.add(c.name);
            return { info: reflection.component(c.name), write: isWrite, stride: 0 };
        });
        this.m_ReadCount = read.length;
    }

    forEachChunk(visit: (chunk: QueryChunk) => void): void {
        const bridge = this.m_Owner.bridge;
        const abi = bridge.abi;
        const n = this.m_Columns.length;
        // One query runs at a time, so every query shares the bridge's block.
        const ids = bridge.queryBlock(), strides = ids + 8 * n, addresses = strides + 4 * n, count = addresses + 4 * (n + 1);
        const typeIds = new BigUint64Array(abi.HEAPU8.buffer, ids, n);
        this.m_Columns.forEach((column, i) => { typeIds[i] = column.info.typeId; });
        bridge.check(abi.ge_query_begin(ids, this.m_ReadCount, ids + 8 * this.m_ReadCount, n - this.m_ReadCount, strides),
            'query.forEachChunk');
        const heapStrides = new Uint32Array(abi.HEAPU8.buffer, strides, n);
        this.m_Columns.forEach((column, i) => { column.stride = heapStrides[i]; });
        const chunk = new ChunkImpl(this.m_Owner, this.m_Columns);
        try {
            for (;;) {
                const more = abi.ge_query_next_chunk(addresses, count);
                if (more < 0) throw bridge.failure('query.forEachChunk');
                if (more === 0) return;
                // A heap growth replaces the buffer: read it again for every chunk.
                const buffer = abi.HEAPU8.buffer;
                chunk.point(Array.from(new Uint32Array(buffer, addresses, n + 1)), new Uint32Array(buffer, count, 1)[0]);
                visit(chunk);
                // engine.dispose() from the callback shut the engine down, which ended the walk.
                if (bridge.contract.disposed) return;
            }
        } catch (error) {
            if (!bridge.contract.disposed && abi.ge_query_end() !== kOk) throw bridge.failure('query.forEachChunk');
            throw error;
        }
    }
}

/** The chunk forEachChunk hands its callback, pointed at each chunk in turn. */
class ChunkImpl implements QueryChunk {
    count = 0;
    private readonly m_Owner: QueryOwner;
    private readonly m_Columns: readonly Column[];
    /** The entity ids' address, then each column's. */
    private m_Addresses: number[] = [];

    constructor(owner: QueryOwner, columns: readonly Column[]) {
        this.m_Owner = owner;
        this.m_Columns = columns;
    }

    point(addresses: number[], count: number): void {
        this.m_Addresses = addresses;
        this.count = count;
    }

    entity(index: number): Entity {
        if (!Number.isInteger(index) || index < 0 || index >= this.count) {
            throw new OpenEngineError('InvalidArgument', `chunk.entity takes 0 to ${this.count - 1}; got ${String(index)}.`);
        }
        const heap = this.m_Owner.bridge.abi.HEAPU8;
        return this.m_Owner.entityFor(new Uint32Array(heap.buffer, this.m_Addresses[0], this.count)[index]);
    }

    floats(component: ComponentType, fieldName: string): FloatColumn {
        const at = this.m_Columns.findIndex((c) => c.info.name === component.name);
        if (at < 0) {
            const names = this.m_Columns.map((c) => c.info.name).join(', ');
            throw new OpenEngineError('InvalidArgument', `This query's components are ${names}; ${component.name} is not one of them.`);
        }
        const { info, write, stride } = this.m_Columns[at];
        const field = this.m_Owner.bridge.reflection.field(info, fieldName);
        if (!kFloatKinds.has(field.kind)) {
            throw new OpenEngineError('InvalidArgument',
                `chunk.floats reads float fields; ${info.name}.${field.jsName} is ${field.kind}. Use entity.get(${info.name}) for it.`);
        }
        if (stride % 4 !== 0 || field.offset % 4 !== 0) {
            throw new OpenEngineError('InvalidArgument', `${info.name} is not laid out in whole floats; use entity.get(${info.name}) for it.`);
        }
        const heap = this.m_Owner.bridge.abi.HEAPU8;
        const address = this.m_Addresses[1 + at];
        const bytes = this.count * stride;
        const values = write
            ? new Float32Array(heap.buffer, address, bytes / 4)
            : new Float32Array(heap.buffer.slice(address, address + bytes));
        return { values, offset: field.offset / 4, stride: stride / 4 };
    }
}
