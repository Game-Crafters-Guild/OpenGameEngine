import type { Vec3 as Vec3Value, Vector3View } from '../opengine.js';
import type { Vec3 } from './math.js';

/** A Vector3View over a read and a write of three numbers. */
export class LiveVector3 implements Vector3View {
    private readonly m_Read: () => Vec3;
    private readonly m_Write: (v: Vec3) => void;

    constructor(read: () => Vec3, write: (v: Vec3) => void) {
        this.m_Read = read;
        this.m_Write = write;
    }

    get x(): number { return this.m_Read()[0]; }
    set x(value: number) { this.writeComponent(0, value); }
    get y(): number { return this.m_Read()[1]; }
    set y(value: number) { this.writeComponent(1, value); }
    get z(): number { return this.m_Read()[2]; }
    set z(value: number) { this.writeComponent(2, value); }

    set(x: number, y: number, z: number): this {
        this.m_Write([x, y, z]);
        return this;
    }

    copy(v: Vec3Value): this {
        return this.set(v.x, v.y, v.z);
    }

    private writeComponent(index: number, value: number): void {
        const v = this.m_Read();
        v[index] = value;
        this.m_Write(v);
    }
}
