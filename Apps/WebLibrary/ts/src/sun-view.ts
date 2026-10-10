import type { SunView } from '../opengine.js';
import type { Light as LightValues } from '../components.js';
import { Light } from './components.js';
import type { EntityImpl } from './entity.js';
import { OpenEngineError } from './errors.js';

/** scene.sun: the default directional light, its intensity in lux. */
export class SunViewImpl implements SunView {
    readonly entity: EntityImpl;

    constructor(entity: EntityImpl) {
        this.entity = entity;
    }

    get intensity(): number { return this.light().intensity; }
    set intensity(lux: number) { this.light().intensity = lux; }
    get color(): [number, number, number] { return this.light().color; }
    set color(rgb: [number, number, number]) { this.light().color = rgb; }
    get castsShadows(): boolean { return this.light().castsShadows; }
    set castsShadows(value: boolean) { this.light().castsShadows = value; }

    private light(): LightValues {
        const light = this.entity.get(Light);
        if (!light) throw new OpenEngineError('InvalidArgument', `scene.sun's entity has no Light any more; add it back with entity.set(Light, {}).`);
        return light;
    }
}
