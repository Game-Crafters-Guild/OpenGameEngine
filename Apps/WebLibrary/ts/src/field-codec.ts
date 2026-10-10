// Field values to and from their bytes in module memory, in the layout abi.ts states.

import type { FieldKind } from './abi.js';
import { OpenEngineError } from './errors.js';
import type { FieldInfo } from './reflection.js';

const kElementSize: Partial<Record<FieldKind, number>> = {
    Bool: 1, Int8: 1, Int16: 2, Int32: 4, Int64: 8, UInt8: 1, UInt16: 2, UInt32: 4, UInt64: 8,
    Float: 4, Double: 8, Vec2: 8, Vec3: 12, Vec4: 16, Quat: 16, Mat4: 64, Color: 16, AssetGuid: 16, EntityHandle: 4,
};

const kVectorKeys: Partial<Record<FieldKind, readonly string[]>> = {
    Vec2: ['x', 'y'], Vec3: ['x', 'y', 'z'], Vec4: ['x', 'y', 'z', 'w'], Quat: ['x', 'y', 'z', 'w'], Color: ['r', 'g', 'b', 'a'],
};

function invalidValue(field: FieldInfo, value: unknown, expected: string): OpenEngineError {
    return new OpenEngineError('InvalidArgument', `${field.jsName} takes ${expected}; got ${String(value)}.`);
}

/** The integer kind an enum field is stored as, from its size. */
function enumStorageKind(field: FieldInfo): FieldKind {
    return field.size === 1 ? 'UInt8' : field.size === 2 ? 'UInt16' : 'Int32';
}

function decodeElement(kind: FieldKind, view: DataView, offset: number): unknown {
    switch (kind) {
        case 'Bool': return view.getUint8(offset) !== 0;
        case 'Int8': return view.getInt8(offset);
        case 'Int16': return view.getInt16(offset, true);
        case 'Int32': return view.getInt32(offset, true);
        case 'UInt8': return view.getUint8(offset);
        case 'UInt16': return view.getUint16(offset, true);
        case 'UInt32': case 'EntityHandle': return view.getUint32(offset, true);
        case 'Int64': return view.getBigInt64(offset, true);
        case 'UInt64': return view.getBigUint64(offset, true);
        case 'Float': return view.getFloat32(offset, true);
        case 'Double': return view.getFloat64(offset, true);
        case 'Mat4': return Array.from({ length: 16 }, (_, i) => view.getFloat32(offset + 4 * i, true));
        case 'AssetGuid': {
            let hex = '';
            for (let i = 0; i < 16; ++i) hex += view.getUint8(offset + i).toString(16).padStart(2, '0');
            return { guid: hex };
        }
        default: {
            const out: Record<string, number> = {};
            (kVectorKeys[kind] ?? []).forEach((key, i) => { out[key] = view.getFloat32(offset + 4 * i, true); });
            return out;
        }
    }
}

function encodeElement(field: FieldInfo, kind: FieldKind, value: unknown, view: DataView, offset: number): void {
    const number = (): number => {
        if (typeof value !== 'number' || !Number.isFinite(value)) throw invalidValue(field, value, 'a finite number');
        return value;
    };
    switch (kind) {
        case 'Bool':
            if (typeof value !== 'boolean') throw invalidValue(field, value, 'true or false');
            view.setUint8(offset, value ? 1 : 0);
            return;
        case 'Int8': view.setInt8(offset, number()); return;
        case 'Int16': view.setInt16(offset, number(), true); return;
        case 'Int32': view.setInt32(offset, number(), true); return;
        case 'UInt8': view.setUint8(offset, number()); return;
        case 'UInt16': view.setUint16(offset, number(), true); return;
        case 'UInt32': case 'EntityHandle': view.setUint32(offset, number(), true); return;
        case 'Int64': case 'UInt64':
            if (typeof value !== 'bigint') throw invalidValue(field, value, 'a bigint');
            if (kind === 'Int64') view.setBigInt64(offset, value, true);
            else view.setBigUint64(offset, value, true);
            return;
        case 'Float': view.setFloat32(offset, number(), true); return;
        case 'Double': view.setFloat64(offset, number(), true); return;
        case 'Mat4':
            if (!Array.isArray(value) || value.length !== 16) throw invalidValue(field, value, 'an array of 16 numbers');
            value.forEach((v, i) => view.setFloat32(offset + 4 * i, Number(v), true));
            return;
        case 'AssetGuid': {
            // The compact form, or the dashed form the editor shows (8-4-4-4-12), in either case.
            const text = (value as { guid?: unknown } | null)?.guid;
            const guid = typeof text === 'string' && /^[0-9a-fA-F]{8}(-?[0-9a-fA-F]{4}){3}-?[0-9a-fA-F]{12}$/.test(text)
                ? text.replaceAll('-', '').toLowerCase() : null;
            if (guid === null) throw invalidValue(field, value, '{ guid } with 32 hex digits, dashed (8-4-4-4-12) or not');
            for (let i = 0; i < 16; ++i) view.setUint8(offset + i, parseInt(guid.slice(2 * i, 2 * i + 2), 16));
            return;
        }
        default: {
            const keys = kVectorKeys[kind];
            if (!keys) throw invalidValue(field, value, 'nothing: the field cannot be written from a page');
            const source = value as Record<string, unknown> | null;
            keys.forEach((key, i) => {
                const element = source?.[key];
                if (typeof element !== 'number') throw invalidValue(field, value, `{ ${keys.join(', ')} }`);
                view.setFloat32(offset + 4 * i, element, true);
            });
        }
    }
}

/** Decodes the field's bytes at `offset` into the value a view returns. */
export function decodeField(field: FieldInfo, view: DataView, offset: number): unknown {
    if (field.enumByValue) {
        const raw = decodeElement(enumStorageKind(field), view, offset) as number;
        return field.enumByValue.get(raw) ?? raw;
    }
    if (field.kind === 'String') {
        const bytes = new Uint8Array(view.buffer, view.byteOffset + offset, field.size);
        const end = bytes.indexOf(0);
        return new TextDecoder().decode(bytes.slice(0, end < 0 ? bytes.length : end));
    }
    if (field.kind === 'Unknown' || field.kind === 'Bytes') {
        return new Uint8Array(view.buffer.slice(view.byteOffset + offset, view.byteOffset + offset + field.size));
    }
    if (field.count === 1) return decodeElement(field.kind, view, offset);
    const step = kElementSize[field.kind] ?? field.size / field.count;
    return Array.from({ length: field.count }, (_, i) => decodeElement(field.kind, view, offset + i * step));
}

/** Encodes `value` into the field's bytes at `offset`; throws naming the expected shape. */
export function encodeField(field: FieldInfo, value: unknown, view: DataView, offset: number): void {
    if (field.enumByName) {
        const raw = typeof value === 'string' ? field.enumByName.get(value) : undefined;
        if (raw === undefined) throw invalidValue(field, value, `one of ${[...field.enumByName.keys()].map((n) => `'${n}'`).join(', ')}`);
        encodeElement(field, enumStorageKind(field), raw, view, offset);
        return;
    }
    if (field.kind === 'String') {
        if (typeof value !== 'string') throw invalidValue(field, value, 'a string');
        const bytes = new TextEncoder().encode(value);
        if (bytes.length >= field.size) throw invalidValue(field, value, `a string under ${field.size} bytes`);
        const target = new Uint8Array(view.buffer, view.byteOffset + offset, field.size);
        target.fill(0);
        target.set(bytes);
        return;
    }
    if (field.kind === 'Unknown' || field.kind === 'Bytes') throw invalidValue(field, value, 'nothing: the field cannot be written from a page');
    if (field.count === 1) {
        encodeElement(field, field.kind, value, view, offset);
        return;
    }
    if (!Array.isArray(value) || value.length !== field.count) throw invalidValue(field, value, `an array of ${field.count} values`);
    const step = kElementSize[field.kind] ?? field.size / field.count;
    value.forEach((element, i) => encodeElement(field, field.kind, element, view, offset + i * step));
}

