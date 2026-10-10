// An Entity handle: an engine entity id and the scene it belongs to. Everything it returns
// reads the engine at access; destroying it invalidates the handle.

import type { AddableComponentType, Bounds, ComponentType, ComponentView, Entity, NoAdd } from '../opengine.js';
import { kInvalidEntity, type EntityId } from './abi.js';
import { AnimationViewImpl } from './animation-view.js';
import type { Bridge } from './bridge.js';
import { OpenEngineError } from './errors.js';
import { matrixMultiply, transformBox, type Mat4 } from './math.js';
import type { ComponentInfo, FieldInfo } from './reflection.js';
import { TransformViewImpl } from './transform-view.js';
import { Vector3 } from './vector3.js';
import { componentView, type FieldAccess } from './component-view.js';

/** What an entity needs from its scene. */
export interface EntityOwner {
    readonly bridge: Bridge;
    entityFor(id: EntityId): EntityImpl;
    forget(id: EntityId): void;
    onFrame(callback: (dt: number) => void): () => void;
}

/** Transform and the other @ge-no-add components are on every entity: plain JavaScript gets the type check at runtime. */
function refuseFixed(component: ComponentType, call: 'add' | 'remove'): void {
    if ((component as Partial<NoAdd>).noAdd !== true) return;
    throw new OpenEngineError('InvalidArgument',
        `Every entity has a ${component.name}, so entity.${call}(${component.name}) is refused; use entity.set(${component.name}, values) to change it.`);
}

export class EntityImpl implements Entity, FieldAccess {
    readonly id: EntityId;
    readonly owner: EntityOwner;
    readonly transform: TransformViewImpl;
    readonly animation: AnimationViewImpl;
    private m_Alive = true;

    constructor(owner: EntityOwner, id: EntityId) {
        this.owner = owner;
        this.id = id;
        this.animation = new AnimationViewImpl(owner.bridge, id, () => this.assertAlive());
        this.transform = new TransformViewImpl({
            readMatrix: () => this.readField(this.component('Transform'), 'matrix') as Mat4,
            writeMatrix: (matrix) => this.writeField(this.component('Transform'), 'matrix', matrix),
            readParent: () => this.readParent(),
            readChildren: () => this.readChildren(),
            readParentWorldMatrix: () => this.readParentWorldMatrix(),
            writeParent: (parent) => this.writeParent(parent),
        });
    }

    get name(): string {
        const name = this.component('Name');
        return this.hasInfo(name) ? (this.readField(name, 'value') as string) : '';
    }

    get bounds(): Bounds | null {
        this.assertAlive();
        const box = this.owner.bridge.entityBounds(this.id);
        if (!box) return null;
        const matrix = this.readField(this.component('Transform'), 'matrix') as Mat4;
        const { center, half } = transformBox(matrix, box.center, box.half);
        return {
            center: new Vector3(...center),
            size: new Vector3(2 * half[0], 2 * half[1], 2 * half[2]),
            min: new Vector3(center[0] - half[0], center[1] - half[1], center[2] - half[2]),
            max: new Vector3(center[0] + half[0], center[1] + half[1], center[2] + half[2]),
        };
    }

    get<T extends object>(component: ComponentType<T>): ComponentView<T> | undefined {
        const info = this.component(component.name);
        return this.hasInfo(info) ? componentView<T>(info, this) : undefined;
    }

    set<T extends object>(component: ComponentType<T>, values: Partial<T>): ComponentView<T> {
        const info = this.component(component.name);
        const bridge = this.owner.bridge;
        // Encode every value before the first write, so a bad one leaves the entity unchanged.
        const writes = Object.entries(values).map(([jsName, value]) => {
            const field = bridge.reflection.field(info, jsName);
            return { field, bytes: bridge.encode(field, this.toEngineValue(field, value)) };
        });
        if (!this.hasInfo(info)) bridge.check(bridge.abi.ge_component_add(this.id, info.typeId), `set(${info.name})`);
        for (const { field, bytes } of writes) bridge.setFieldBytes(this.id, info, field, bytes);
        return componentView<T>(info, this);
    }

    add<T extends object>(component: AddableComponentType<T>): ComponentView<T> {
        refuseFixed(component, 'add');
        const info = this.component(component.name);
        if (this.hasInfo(info)) {
            throw new OpenEngineError('InvalidArgument', `The entity already has ${info.name}; use entity.set(${info.name}, values) to change it.`);
        }
        this.owner.bridge.check(this.owner.bridge.abi.ge_component_add(this.id, info.typeId), `add(${info.name})`);
        return componentView<T>(info, this);
    }

    remove(component: AddableComponentType): void {
        refuseFixed(component, 'remove');
        const info = this.component(component.name);
        if (!this.hasInfo(info)) return;
        this.owner.bridge.check(this.owner.bridge.abi.ge_component_remove(this.id, info.typeId), `remove(${info.name})`);
    }

    has(component: ComponentType): boolean {
        return this.hasInfo(this.component(component.name));
    }

    destroy(): void {
        this.assertAlive();
        this.owner.bridge.check(this.owner.bridge.abi.ge_entity_destroy(this.id), 'destroy()');
        this.m_Alive = false;
        this.owner.forget(this.id);
    }

    readField(component: ComponentInfo, jsName: string): unknown {
        this.assertAlive();
        const bridge = this.owner.bridge;
        const field = bridge.reflection.field(component, jsName);
        const value = bridge.getField(this.id, component, field);
        if (field.kind !== 'EntityHandle') return value;
        return value === kInvalidEntity ? null : this.owner.entityFor(value as EntityId);
    }

    writeField(component: ComponentInfo, jsName: string, value: unknown): void {
        this.assertAlive();
        const bridge = this.owner.bridge;
        const field = bridge.reflection.field(component, jsName);
        bridge.setField(this.id, component, field, this.toEngineValue(field, value));
    }

    /** An EntityHandle field takes an Entity (or null for none); every other field its value as is. */
    private toEngineValue(field: FieldInfo, value: unknown): unknown {
        if (field.kind !== 'EntityHandle') return value;
        if (value === null) return kInvalidEntity;
        if (value instanceof EntityImpl) return value.id;
        throw new OpenEngineError('InvalidArgument', `${field.jsName} takes an Entity or null; got ${String(value)}.`);
    }

    private component(name: string): ComponentInfo {
        return this.owner.bridge.reflection.component(name);
    }

    private hasInfo(info: ComponentInfo): boolean {
        this.assertAlive();
        return this.owner.bridge.hasComponent(this.id, info);
    }

    private readParent(): Entity | null {
        const parent = this.component('Parent');
        if (!this.hasInfo(parent)) return null;
        return this.readField(parent, 'parent') as Entity | null;
    }

    private readChildren(): Entity[] {
        this.assertAlive();
        return this.owner.bridge.entityChildren(this.id).map((id) => this.owner.entityFor(id));
    }

    private readParentWorldMatrix(): Mat4 {
        let world: Mat4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
        for (let parent = this.readParent() as EntityImpl | null; parent; parent = parent.readParent() as EntityImpl | null)
            world = matrixMultiply(parent.readField(parent.component('Transform'), 'matrix') as Mat4, world);
        return world;
    }

    private writeParent(parent: Entity | null): void {
        this.assertAlive();
        const parentId = parent === null ? kInvalidEntity : parent.id;
        this.owner.bridge.check(this.owner.bridge.abi.ge_entity_parent(this.id, parentId), 'transform.parent');
    }

    private assertAlive(): void {
        if (!this.m_Alive) throw new OpenEngineError('Disposed', `Entity ${this.id} was destroyed; create a new one with scene.create.`);
    }
}
