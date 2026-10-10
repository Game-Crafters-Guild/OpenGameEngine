// Live component views: objects whose properties read the engine on access and write it on
// assignment. A view holds an entity and a field table, never a value.

import type { ComponentInfo } from './reflection.js';

/** What a view needs from its entity. */
export interface FieldAccess {
    readField(component: ComponentInfo, jsName: string): unknown;
    writeField(component: ComponentInfo, jsName: string, value: unknown): void;
}

const kAccess = Symbol('access');
const g_Prototypes = new WeakMap<ComponentInfo, object>();

function prototypeFor(component: ComponentInfo): object {
    let prototype = g_Prototypes.get(component);
    if (prototype) return prototype;
    prototype = {};
    for (const field of component.fields) {
        Object.defineProperty(prototype, field.jsName, {
            enumerable: true,
            get(this: { [kAccess]: FieldAccess }) {
                return this[kAccess].readField(component, field.jsName);
            },
            set(this: { [kAccess]: FieldAccess }, value: unknown) {
                this[kAccess].writeField(component, field.jsName, value);
            },
        });
    }
    g_Prototypes.set(component, prototype);
    return prototype;
}

/** A view whose properties are the component's fields by their camelCase names. */
export function componentView<Values extends object>(component: ComponentInfo, access: FieldAccess): Values {
    return Object.create(prototypeFor(component), { [kAccess]: { value: access } }) as Values;
}
