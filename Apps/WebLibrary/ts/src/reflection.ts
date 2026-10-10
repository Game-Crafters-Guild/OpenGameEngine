// The live component registry as the facade sees it: names, ids and field layouts parsed
// from ge_reflection_json, and the release check.

import type { FieldKind, ReflectionJson, TypeId } from './abi.js';
import { OpenEngineError } from './errors.js';

export interface FieldInfo {
    /** The index ge_field_get / ge_field_set take. */
    readonly id: number;
    /** The C++ member name. */
    readonly name: string;
    /** The member name the types and views use (the JSON's tsName). */
    readonly jsName: string;
    readonly kind: FieldKind;
    /** The field's byte offset in the component. */
    readonly offset: number;
    readonly size: number;
    readonly count: number;
    readonly enumByName: ReadonlyMap<string, number> | null;
    readonly enumByValue: ReadonlyMap<number, string> | null;
}

export interface ComponentInfo {
    readonly name: string;
    readonly typeId: TypeId;
    readonly fields: readonly FieldInfo[];
    readonly fieldsByJsName: ReadonlyMap<string, FieldInfo>;
}

/** FNV-1a 64 over the UTF-8 bytes of `text`, as 16 lowercase hex digits. */
export function fingerprint(text: string): string {
    const kPrime = 0x100000001b3n;
    const kMask = 0xffffffffffffffffn;
    let hash = 0xcbf29ce484222325n;
    for (const byte of new TextEncoder().encode(text)) {
        hash ^= BigInt(byte);
        hash = (hash * kPrime) & kMask;
    }
    return hash.toString(16).padStart(16, '0');
}

/**
 * Throws when the engine module's reflection differs from the one the facade was built
 * against: a field would otherwise be read at the wrong offset.
 */
export function checkRelease(json: string, engineVersion: string, expected: { version: string; fingerprint: string | null }): void {
    if (expected.fingerprint === null || fingerprint(json) === expected.fingerprint) return;
    throw new OpenEngineError('VersionMismatch',
        `The engine module is release ${engineVersion} and this library is ${expected.version}; their component layouts differ. ` +
        `Update @openengine/web to ${engineVersion}, or serve the engine module that ships with ${expected.version}.`);
}

export class Reflection {
    readonly engineVersion: string;
    private readonly m_ByName = new Map<string, ComponentInfo>();

    constructor(document: ReflectionJson) {
        this.engineVersion = document.engineVersion;
        for (const component of document.components) {
            const fields = component.fields.map((field, id): FieldInfo => ({
                id,
                name: field.name,
                jsName: field.tsName,
                kind: field.kind,
                offset: field.offset,
                size: field.size,
                count: field.count,
                enumByName: field.enum ? new Map(field.enum.map((e) => [e.name, e.value])) : null,
                enumByValue: field.enum ? new Map(field.enum.map((e) => [e.value, e.name])) : null,
            }));
            this.m_ByName.set(component.name, {
                name: component.name,
                typeId: BigInt(component.typeId),
                fields,
                fieldsByJsName: new Map(fields.map((f) => [f.jsName, f])),
            });
        }
    }

    /** The component named `name`; throws naming the fix when this engine does not reflect it. */
    component(name: string): ComponentInfo {
        const info = this.m_ByName.get(name);
        if (!info) {
            throw new OpenEngineError('InvalidArgument',
                `This engine module has no reflected component named '${name}'. Check the name against components.d.ts, ` +
                `or serve the engine module that ships with this library.`);
        }
        return info;
    }

    /** The field `jsName` of `component`; throws listing the valid names when it is missing. */
    field(component: ComponentInfo, jsName: string): FieldInfo {
        const field = component.fieldsByJsName.get(jsName);
        if (!field) {
            const names = component.fields.map((f) => f.jsName).join(', ');
            throw new OpenEngineError('InvalidArgument', `${component.name} has no field '${jsName}'. Its fields are: ${names}.`);
        }
        return field;
    }
}
