// Mouse orbit, zoom and pan around a target, written into a camera entity's transform once
// per frame. The pose math is the editor camera rig's (SceneViewCameraRig.h) in double:
// yaw and pitch in degrees, look = (cos p cos y, sin p, cos p sin y), yaw 90 looks along +Z,
// the camera at target - look * distance.

import type { Entity, OrbitControls } from '../opengine.js';
import { Camera } from './components.js';
import { EntityImpl } from './entity.js';
import { OpenEngineError } from './errors.js';
import { quatFromAxisDegrees, quatMultiply, type Vec3 } from './math.js';
import { Vector3 } from './vector3.js';

const kDegToRad = Math.PI / 180;
const kPitchLimit = 89;
/** Wheel pixels per e-fold of distance at zoomSpeed 1. */
const kWheelPixelsPerEFold = 1000;
const kLinePixels = 16;

export class OrbitControlsImpl implements OrbitControls {
    readonly target = new Vector3();
    distance: number;
    minDistance = 0.01;
    maxDistance = Infinity;
    enableDamping = true;
    dampingFactor = 0.1;
    rotateSpeed = 1;
    zoomSpeed = 1;
    panSpeed = 1;

    private readonly m_Camera: EntityImpl;
    private readonly m_Element: HTMLElement;
    private m_Yaw: number;
    private m_Pitch: number;
    private m_YawDelta = 0;
    private m_PitchDelta = 0;
    private m_ZoomDelta = 0;
    private m_PanDelta: Vec3 = [0, 0, 0];
    /** Pan drag pixels not yet converted to meters; update() converts them at the current pose. */
    private m_PanPixels: [number, number] = [0, 0];
    private m_Drag: { mode: 'rotate' | 'pan'; pointerId: number; x: number; y: number } | null = null;
    private readonly m_Detach: Array<() => void> = [];

    constructor(camera: Entity, element: HTMLElement) {
        if (!(camera instanceof EntityImpl)) throw new OpenEngineError('InvalidArgument', 'OrbitControls takes an entity from this engine, such as scene.camera.');
        this.m_Camera = camera;
        this.m_Element = element;
        const p = camera.transform.position;
        const offset: Vec3 = [p.x - this.target.x, p.y - this.target.y, p.z - this.target.z];
        this.distance = Math.hypot(...offset) || 1;
        this.m_Pitch = Math.asin(Math.max(-1, Math.min(1, -offset[1] / this.distance))) / kDegToRad;
        this.m_Yaw = Math.atan2(-offset[2], -offset[0]) / kDegToRad;
        this.listen('pointerdown', (e) => this.onPointerDown(e as PointerEvent));
        this.listen('pointermove', (e) => this.onPointerMove(e as PointerEvent));
        this.listen('pointerup', (e) => this.onPointerUp(e as PointerEvent));
        this.listen('pointercancel', (e) => this.onPointerUp(e as PointerEvent));
        this.listen('wheel', (e) => this.onWheel(e as WheelEvent), { passive: false });
        this.listen('contextmenu', (e) => e.preventDefault());
        this.m_Detach.push(camera.owner.onFrame((dt) => this.update(dt)));
    }

    update(dt = 0): void {
        this.convertPanPixels();
        const f = this.enableDamping ? 1 - Math.pow(1 - this.dampingFactor, dt * 60) : 1;
        this.m_Yaw += this.m_YawDelta * f;
        this.m_YawDelta -= this.m_YawDelta * f;
        const pitch = this.m_Pitch + this.m_PitchDelta * f;
        this.m_Pitch = Math.max(-kPitchLimit, Math.min(kPitchLimit, pitch));
        // Motion past the pole is dropped rather than kept pushing against the clamp.
        this.m_PitchDelta = pitch === this.m_Pitch ? this.m_PitchDelta - this.m_PitchDelta * f : 0;
        this.target.set(this.target.x + this.m_PanDelta[0] * f, this.target.y + this.m_PanDelta[1] * f, this.target.z + this.m_PanDelta[2] * f);
        this.m_PanDelta = this.m_PanDelta.map((v) => v - v * f) as Vec3;
        this.distance = Math.max(this.minDistance, Math.min(this.maxDistance, this.distance * Math.exp(this.m_ZoomDelta * f)));
        this.m_ZoomDelta -= this.m_ZoomDelta * f;

        const look = this.look();
        const transform = this.m_Camera.transform;
        transform.position.set(this.target.x - look[0] * this.distance, this.target.y - look[1] * this.distance, this.target.z - look[2] * this.distance);
        // Heading 90 - yaw about +Y turns +Z onto the look's horizontal part; pitch up is a negative turn about +X.
        const q = quatMultiply(quatFromAxisDegrees([0, 1, 0], 90 - this.m_Yaw), quatFromAxisDegrees([1, 0, 0], -this.m_Pitch));
        transform.quaternion.set(q.x, q.y, q.z, q.w);
    }

    get pitch(): number {
        return this.m_Pitch;
    }

    set pitch(degrees: number) {
        if (!Number.isFinite(degrees)) throw new OpenEngineError('InvalidArgument', `OrbitControls.pitch takes a finite number of degrees, not ${degrees}.`);
        this.m_Pitch = Math.max(-kPitchLimit, Math.min(kPitchLimit, degrees));
        this.m_PitchDelta = 0;
    }

    dispose(): void {
        for (const detach of this.m_Detach.splice(0)) detach();
    }

    private look(): Vec3 {
        const yaw = this.m_Yaw * kDegToRad;
        const pitch = this.m_Pitch * kDegToRad;
        return [Math.cos(pitch) * Math.cos(yaw), Math.sin(pitch), Math.cos(pitch) * Math.sin(yaw)];
    }

    private listen(type: string, handler: (event: Event) => void, options?: AddEventListenerOptions): void {
        this.m_Element.addEventListener(type, handler, options);
        this.m_Detach.push(() => this.m_Element.removeEventListener(type, handler, options));
    }

    private onPointerDown(event: PointerEvent): void {
        const pan = event.button === 2 || (event.button === 0 && event.shiftKey);
        if (!pan && event.button !== 0) return;
        if (this.m_Drag) return; // a second pointer does not take over the drag
        this.m_Drag = { mode: pan ? 'pan' : 'rotate', pointerId: event.pointerId, x: event.clientX, y: event.clientY };
        this.m_Element.setPointerCapture?.(event.pointerId);
    }

    private onPointerUp(event: PointerEvent): void {
        if (this.m_Drag?.pointerId === event.pointerId) this.m_Drag = null;
    }

    private onPointerMove(event: PointerEvent): void {
        if (!this.m_Drag || this.m_Drag.pointerId !== event.pointerId) return;
        const dx = event.clientX - this.m_Drag.x;
        const dy = event.clientY - this.m_Drag.y;
        this.m_Drag.x = event.clientX;
        this.m_Drag.y = event.clientY;
        if (this.m_Drag.mode === 'rotate') this.rotateBy(dx, dy);
        else {
            this.m_PanPixels[0] += dx;
            this.m_PanPixels[1] += dy;
        }
    }

    private onWheel(event: WheelEvent): void {
        event.preventDefault();
        const pixels = event.deltaMode === 1 ? event.deltaY * kLinePixels
            : event.deltaMode === 2 ? event.deltaY * (this.m_Element.clientHeight || 1) : event.deltaY;
        this.m_ZoomDelta += (pixels / kWheelPixelsPerEFold) * this.zoomSpeed;
    }

    /** A drag of (dx, dy) pixels: the full height of the element turns the camera 360 degrees. */
    private rotateBy(dx: number, dy: number): void {
        const degreesPerPixel = (360 / (this.m_Element.clientHeight || 1)) * this.rotateSpeed;
        this.m_YawDelta -= dx * degreesPerPixel;
        this.m_PitchDelta -= dy * degreesPerPixel;
    }

    /** The pending pan drag: the scene under the pointer follows it at the target's depth. */
    private convertPanPixels(): void {
        const [dx, dy] = this.m_PanPixels;
        if (dx === 0 && dy === 0) return;
        this.m_PanPixels = [0, 0];
        const fovY = this.m_Camera.get(Camera)?.fovY ?? 60;
        const metersPerPixel = (2 * this.distance * Math.tan(0.5 * fovY * kDegToRad) / (this.m_Element.clientHeight || 1)) * this.panSpeed;
        const yaw = this.m_Yaw * kDegToRad;
        const pitch = this.m_Pitch * kDegToRad;
        const right: Vec3 = [Math.sin(yaw), 0, -Math.cos(yaw)];
        const up: Vec3 = [-Math.sin(pitch) * Math.cos(yaw), Math.cos(pitch), -Math.sin(pitch) * Math.sin(yaw)];
        for (let i = 0; i < 3; ++i) this.m_PanDelta[i] += (-right[i] * dx + up[i] * dy) * metersPerPixel;
    }
}
