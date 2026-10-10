// The component values a page passes to entity.get / set / add / remove / has: the reflected
// name, and the type id the live registry assigns, filled in by Engine.create. The list is
// the one components.d.ts declares.

import type {
    Camera as CameraValues, DDGIVolume as DDGIVolumeValues, Light as LightValues, LocalBounds as LocalBoundsValues, MeshRenderer as MeshRendererValues,
    SkyEnvironment as SkyEnvironmentValues, Transform as TransformValues,
} from '../components.js';
import type { ComponentType, NoAdd } from '../opengine.js';
import type { Reflection } from './reflection.js';

class ComponentToken<T extends object> implements ComponentType<T> {
    readonly name: string;
    typeId: bigint | null = null;

    constructor(name: string) {
        this.name = name;
    }
}

class FixedComponentToken<T extends object> extends ComponentToken<T> implements NoAdd {
    readonly noAdd = true as const;
}

export const Transform: ComponentType<TransformValues> & NoAdd = new FixedComponentToken<TransformValues>('Transform');
export const Light: ComponentType<LightValues> = new ComponentToken<LightValues>('Light');
export const LocalBounds: ComponentType<LocalBoundsValues> = new ComponentToken<LocalBoundsValues>('LocalBounds');
export const MeshRenderer: ComponentType<MeshRendererValues> = new ComponentToken<MeshRendererValues>('MeshRenderer');
export const Camera: ComponentType<CameraValues> = new ComponentToken<CameraValues>('Camera');
export const SkyEnvironment: ComponentType<SkyEnvironmentValues> = new ComponentToken<SkyEnvironmentValues>('SkyEnvironment');
export const DDGIVolume: ComponentType<DDGIVolumeValues> = new ComponentToken<DDGIVolumeValues>('DDGIVolume');

const kTokens = [Transform, Light, LocalBounds, MeshRenderer, Camera, SkyEnvironment, DDGIVolume] as Array<ComponentToken<object>>;

/** Fills every token's type id from the live registry. */
export function bindComponentTokens(reflection: Reflection): void {
    for (const token of kTokens) token.typeId = reflection.component(token.name).typeId;
}
