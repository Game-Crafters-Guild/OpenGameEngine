import type { Vec3 } from '../opengine.js';

/** A plain 3D vector owned by the page. */
export class Vector3 implements Vec3 {
    x: number;
    y: number;
    z: number;

    constructor(x = 0, y = 0, z = 0) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    set(x: number, y: number, z: number): this {
        this.x = x;
        this.y = y;
        this.z = z;
        return this;
    }

    copy(v: Vec3): this {
        return this.set(v.x, v.y, v.z);
    }
}
