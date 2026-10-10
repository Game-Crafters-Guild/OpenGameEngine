import type { SkyView } from '../opengine.js';
import type { SkyEnvironment as SkyEnvironmentValues } from '../components.js';
import { SkyEnvironment } from './components.js';
import type { EntityImpl } from './entity.js';
import { OpenEngineError } from './errors.js';

/** scene.sky: the default sky; the sun's direction follows its time and place. */
export class SkyViewImpl implements SkyView {
    readonly entity: EntityImpl;

    constructor(entity: EntityImpl) {
        this.entity = entity;
    }

    get timeOfDay(): number { return this.sky().timeOfDayHours; }
    set timeOfDay(hours: number) { this.sky().timeOfDayHours = hours; }
    get latitude(): number { return this.sky().latitude; }
    set latitude(degrees: number) { this.sky().latitude = degrees; }
    get dayOfYear(): number { return this.sky().dayOfYear; }
    set dayOfYear(day: number) { this.sky().dayOfYear = day; }
    get northHeading(): number { return this.sky().northHeading; }
    set northHeading(degrees: number) { this.sky().northHeading = degrees; }

    private sky(): SkyEnvironmentValues {
        const sky = this.entity.get(SkyEnvironment);
        if (!sky) throw new OpenEngineError('InvalidArgument', `scene.sky's entity has no SkyEnvironment any more; add it back with entity.set(SkyEnvironment, {}).`);
        return sky;
    }
}
