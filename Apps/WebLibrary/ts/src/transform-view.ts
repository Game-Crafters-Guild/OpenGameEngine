// entity.transform: position, rotation, scale and parent over the Transform component's
// reflected matrix. Every access reads the matrix from the engine; a write changes only what
// it names (a position write leaves the rotation and scale bytes untouched).

import type { Entity, TransformView } from '../opengine.js';
import {
    composeMatrix, eulerDegreesFromQuat, matrixDeterminant3, matrixMultiply, matrixPosition, matrixRotation,
    matrixScale, matrixSolveDirection, quatFromAxisDegrees, quatFromEulerDegrees, quatMultiply, type Mat4,
    type Quat, type Vec3,
} from './math.js';
import { LiveQuaternion } from './live-quaternion.js';
import { LiveVector3 } from './live-vector3.js';

/** What the view needs from its entity. */
export interface TransformAccess {
    readMatrix(): Mat4;
    writeMatrix(matrix: Mat4): void;
    readParent(): Entity | null;
    /** The entities whose parent is this one. */
    readChildren(): Entity[];
    /** The product of every parent's matrix, root first; identity for a root entity. */
    readParentWorldMatrix(): Mat4;
    writeParent(parent: Entity | null): void;
}

export class TransformViewImpl implements TransformView {
    readonly position: LiveVector3;
    /** Euler degrees, the editor inspector's X, Y, Z convention. */
    readonly rotation: LiveVector3;
    readonly quaternion: LiveQuaternion;
    readonly scale: LiveVector3;
    private readonly m_Access: TransformAccess;

    constructor(access: TransformAccess) {
        this.m_Access = access;
        this.position = new LiveVector3(() => matrixPosition(access.readMatrix()), (v) => this.writePosition(v));
        this.rotation = new LiveVector3(() => eulerDegreesFromQuat(matrixRotation(access.readMatrix())),
            (v) => this.writeRotation(quatFromEulerDegrees(v[0], v[1], v[2])));
        this.quaternion = new LiveQuaternion(() => matrixRotation(access.readMatrix()), (q) => this.writeRotation(q));
        this.scale = new LiveVector3(() => matrixScale(access.readMatrix()), (v) => this.writeScale(v));
    }

    get parent(): Entity | null {
        return this.m_Access.readParent();
    }

    set parent(parent: Entity | null) {
        this.m_Access.writeParent(parent);
    }

    get children(): Entity[] {
        return this.m_Access.readChildren();
    }

    rotateX(degrees: number): this {
        return this.rotateLocal([1, 0, 0], degrees);
    }

    rotateY(degrees: number): this {
        return this.rotateLocal([0, 1, 0], degrees);
    }

    rotateZ(degrees: number): this {
        return this.rotateLocal([0, 0, 1], degrees);
    }

    rotateWorldY(degrees: number): this {
        // World +Y carried into the parent's space is the axis to turn about there. A mirroring
        // parent reverses the sense of a turn, so the angle flips with it. The turn multiplies the
        // local matrix's linear part on the left and leaves its translation, so the entity turns
        // about its own position and keeps its own scale and mirroring. A parent scaled to zero
        // or flattened has no such axis (the solve divides by its zero determinant): no turn.
        const parentWorld = this.m_Access.readParentWorldMatrix();
        const [x, y, z] = matrixSolveDirection(parentWorld, [0, 1, 0]);
        const length = Math.hypot(x, y, z);
        if (!Number.isFinite(length) || length === 0) return this;
        const angle = matrixDeterminant3(parentWorld) < 0 ? -degrees : degrees;
        const turn = composeMatrix([0, 0, 0], quatFromAxisDegrees([x / length, y / length, z / length], angle), [1, 1, 1]);
        const m = this.m_Access.readMatrix();
        const turned = matrixMultiply(turn, m);
        turned[12] = m[12];
        turned[13] = m[13];
        turned[14] = m[14];
        this.m_Access.writeMatrix(turned);
        return this;
    }

    private rotateLocal(axis: Vec3, degrees: number): this {
        const m = this.m_Access.readMatrix();
        const q = quatMultiply(matrixRotation(m), quatFromAxisDegrees(axis, degrees));
        this.m_Access.writeMatrix(composeMatrix(matrixPosition(m), q, matrixScale(m)));
        return this;
    }

    private writePosition(v: Vec3): void {
        const m = this.m_Access.readMatrix();
        m[12] = v[0];
        m[13] = v[1];
        m[14] = v[2];
        this.m_Access.writeMatrix(m);
    }

    private writeRotation(q: Quat): void {
        const m = this.m_Access.readMatrix();
        this.m_Access.writeMatrix(composeMatrix(matrixPosition(m), q, matrixScale(m)));
    }

    private writeScale(v: Vec3): void {
        const m = this.m_Access.readMatrix();
        this.m_Access.writeMatrix(composeMatrix(matrixPosition(m), matrixRotation(m), v));
    }
}
