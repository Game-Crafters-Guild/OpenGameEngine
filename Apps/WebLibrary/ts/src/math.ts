// Transform math in double, ported from Components/Transform.h so a page and the engine agree
// on what a matrix means: left-handed TRS (matrix * v == q.Rotate(v)), column-major, the
// rotation and scale extracted in double and rounded to float32 once, on the write.

export type Vec3 = [number, number, number];
export interface Quat { x: number; y: number; z: number; w: number }
/** 16 numbers, column-major; translation in elements 12 to 14. */
export type Mat4 = number[];

const kDegToRad = Math.PI / 180;

export function quatMultiply(a: Quat, b: Quat): Quat {
    return {
        w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

/** Rotation by `degrees` about a unit axis. */
export function quatFromAxisDegrees(axis: Vec3, degrees: number): Quat {
    const half = 0.5 * degrees * kDegToRad;
    const s = Math.sin(half);
    return { x: axis[0] * s, y: axis[1] * s, z: axis[2] * s, w: Math.cos(half) };
}

/** Rotates `v` by the unit quaternion `q`. */
export function quatRotate(q: Quat, v: Vec3): Vec3 {
    const p = quatMultiply(quatMultiply(q, { x: v[0], y: v[1], z: v[2], w: 0 }), { x: -q.x, y: -q.y, z: -q.z, w: q.w });
    return [p.x, p.y, p.z];
}

function columnLength(m: Mat4, first: number): number {
    return Math.hypot(m[first], m[first + 1], m[first + 2]);
}

export function matrixPosition(m: Mat4): Vec3 {
    return [m[12], m[13], m[14]];
}

export function matrixScale(m: Mat4): Vec3 {
    return [columnLength(m, 0), columnLength(m, 4), columnLength(m, 8)];
}

/** Transform::GetRotation. */
export function matrixRotation(m: Mat4): Quat {
    const sx = columnLength(m, 0);
    const sy = columnLength(m, 4);
    const sz = columnLength(m, 8);
    if (sx <= 0 || sy <= 0 || sz <= 0) return { x: 0, y: 0, z: 0, w: 1 };
    const r00 = m[0] / sx, r10 = m[1] / sx, r20 = m[2] / sx;
    const r01 = m[4] / sy, r11 = m[5] / sy, r21 = m[6] / sy;
    const r02 = m[8] / sz, r12 = m[9] / sz, r22 = m[10] / sz;
    const trace = r00 + r11 + r22;
    if (trace > 0) {
        const s = Math.sqrt(trace + 1) * 2;
        return { w: 0.25 * s, x: (r21 - r12) / s, y: (r02 - r20) / s, z: (r10 - r01) / s };
    }
    if (r00 > r11 && r00 > r22) {
        const s = Math.sqrt(1 + r00 - r11 - r22) * 2;
        return { w: (r21 - r12) / s, x: 0.25 * s, y: (r01 + r10) / s, z: (r02 + r20) / s };
    }
    if (r11 > r22) {
        const s = Math.sqrt(1 + r11 - r00 - r22) * 2;
        return { w: (r02 - r20) / s, x: (r01 + r10) / s, y: 0.25 * s, z: (r12 + r21) / s };
    }
    const s = Math.sqrt(1 + r22 - r00 - r11) * 2;
    return { w: (r10 - r01) / s, x: (r02 + r20) / s, y: (r12 + r21) / s, z: 0.25 * s };
}

/** Transform::FromTRS (left-handed); a non-unit quaternion rotates by q / |q|. */
export function composeMatrix(position: Vec3, q: Quat, scale: Vec3): Mat4 {
    const normSquared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    const s = normSquared > 0 ? 2 / normSquared : 0;
    const x2 = q.x * s, y2 = q.y * s, z2 = q.z * s;
    const xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    const yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    const wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    const [sx, sy, sz] = scale;
    return [
        (1 - (yy + zz)) * sx, (xy + wz) * sx, (xz - wy) * sx, 0,
        (xy - wz) * sy, (1 - (xx + zz)) * sy, (yz + wx) * sy, 0,
        (xz + wy) * sz, (yz - wx) * sz, (1 - (xx + yy)) * sz, 0,
        position[0], position[1], position[2], 1,
    ];
}

/** QuaternionFromEulerXYZDegrees (Components/Transform.h): x roll, y pitch, z yaw. */
export function quatFromEulerDegrees(x: number, y: number, z: number): Quat {
    const roll = x * kDegToRad, pitch = y * kDegToRad, yaw = z * kDegToRad;
    const cy = Math.cos(yaw * 0.5), sy = Math.sin(yaw * 0.5);
    const cp = Math.cos(pitch * 0.5), sp = Math.sin(pitch * 0.5);
    const cr = Math.cos(roll * 0.5), sr = Math.sin(roll * 0.5);
    return {
        w: cr * cp * cy + sr * sp * sy,
        x: sr * cp * cy - cr * sp * sy,
        y: cr * sp * cy + sr * cp * sy,
        z: cr * cp * sy - sr * sp * cy,
    };
}

/** EulerXYZDegreesFromQuaternion (Components/Transform.h). */
export function eulerDegreesFromQuat(q: Quat): Vec3 {
    const sinrCosp = 2 * (q.w * q.x + q.y * q.z);
    const cosrCosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    const sinp = 2 * (q.w * q.y - q.z * q.x);
    const pitch = Math.abs(sinp) >= 1 ? Math.sign(sinp) * Math.PI / 2 : Math.asin(sinp);
    const sinyCosp = 2 * (q.w * q.z + q.x * q.y);
    const cosyCosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    return [Math.atan2(sinrCosp, cosrCosp) / kDegToRad, pitch / kDegToRad, Math.atan2(sinyCosp, cosyCosp) / kDegToRad];
}

/** The affine product a * b (b applied first). */
export function matrixMultiply(a: Mat4, b: Mat4): Mat4 {
    const out = new Array<number>(16).fill(0);
    for (let col = 0; col < 4; ++col) {
        for (let row = 0; row < 4; ++row) {
            let sum = 0;
            for (let k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = sum;
        }
    }
    return out;
}

/** The determinant of the matrix's linear (3x3) part: negative for a mirroring matrix. */
export function matrixDeterminant3(m: Mat4): number {
    return m[0] * (m[5] * m[10] - m[9] * m[6])
         - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

/** The direction d with linear(m) * d == v: `v` carried back through the matrix's linear part. */
export function matrixSolveDirection(m: Mat4, v: Vec3): Vec3 {
    const det = matrixDeterminant3(m);
    const replace = (c: number): Mat4 => {
        const r = m.slice();
        r[c * 4] = v[0]; r[c * 4 + 1] = v[1]; r[c * 4 + 2] = v[2];
        return r;
    };
    return [matrixDeterminant3(replace(0)) / det, matrixDeterminant3(replace(1)) / det, matrixDeterminant3(replace(2)) / det];
}

/** The axis-aligned box enclosing a box (center, half extents) after the affine matrix `m`. */
export function transformBox(m: Mat4, center: Vec3, half: Vec3): { center: Vec3; half: Vec3 } {
    const out: Vec3 = [m[12], m[13], m[14]];
    const extent: Vec3 = [0, 0, 0];
    for (let row = 0; row < 3; ++row) {
        for (let col = 0; col < 3; ++col) {
            const element = m[col * 4 + row];
            out[row] += element * center[col];
            extent[row] += Math.abs(element) * half[col];
        }
    }
    return { center: out, half: extent };
}
