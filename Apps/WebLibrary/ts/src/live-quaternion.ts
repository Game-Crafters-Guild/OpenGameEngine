import type { QuaternionView } from '../opengine.js';
import type { Quat } from './math.js';

/** A QuaternionView over a read and a write of a quaternion. */
export class LiveQuaternion implements QuaternionView {
    private readonly m_Read: () => Quat;
    private readonly m_Write: (q: Quat) => void;

    constructor(read: () => Quat, write: (q: Quat) => void) {
        this.m_Read = read;
        this.m_Write = write;
    }

    get x(): number { return this.m_Read().x; }
    set x(value: number) { this.m_Write({ ...this.m_Read(), x: value }); }
    get y(): number { return this.m_Read().y; }
    set y(value: number) { this.m_Write({ ...this.m_Read(), y: value }); }
    get z(): number { return this.m_Read().z; }
    set z(value: number) { this.m_Write({ ...this.m_Read(), z: value }); }
    get w(): number { return this.m_Read().w; }
    set w(value: number) { this.m_Write({ ...this.m_Read(), w: value }); }

    set(x: number, y: number, z: number, w: number): this {
        this.m_Write({ x, y, z, w });
        return this;
    }
}
