// Type test of the public declarations: the day-one calls compile, and each misuse below is
// a type error (tsc fails on a @ts-expect-error with no error). check-declarations.mjs also
// compiles the README's and llms.txt's examples as written.
import { Engine, OrbitControls, Light, Camera, SkyEnvironment, Transform, OpenEngineError } from '@openengine/web';
import type { Entity, Scene } from '@openengine/web';

export async function daySnippet(canvas: HTMLCanvasElement): Promise<void> {
    const engine = await Engine.create({ canvas, threads: 'auto' });
    const scene = engine.scene;
    const robot = await scene.load('models/robot.glb');
    robot.transform.position.set(0, 0, 0);
    const controls = new OrbitControls(scene.camera, canvas);
    controls.target.copy(robot.bounds.center);
    scene.sun.intensity = 100_000;
    scene.sky.timeOfDay = 9.5;
    const lamp = scene.create('lamp');
    lamp.set(Light, { type: 'Point', intensity: 800 });
    const stop = engine.onFrame((dt) => robot.transform.rotateWorldY(20 * dt));
    engine.run();
    stop();
}

export function componentDoor(entity: Entity): void {
    const light = entity.set(Light, { type: 'Spot', color: [1, 0.9, 0.8], outerAngle: 0.6 });
    light.intensity *= 0.5;
    const camera = entity.get(Camera);
    if (camera) camera.fovY = 50;
    entity.get(SkyEnvironment)?.timeOfDayHours;
    entity.add(Camera);
    entity.remove(Light);
    const present: boolean = entity.has(Light);
    void present;
    entity.transform.parent = null;
    entity.set(SkyEnvironment, { sunLight: entity });
    entity.set(SkyEnvironment, { sunLight: null });
    entity.set(Transform, { matrix: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] });
}

export function animation(model: Entity): void {
    const clips: string[] = model.animation.clips;
    model.animation.play(clips[0]);
    model.animation.play('Run', { speed: 0.5 });
    model.animation.pause();
    // @ts-expect-error the clip list is read, not assigned
    model.animation.clips = [];
    // @ts-expect-error the speed is a number
    model.animation.play('Run', { speed: 'fast' });
}

export function batched(scene: Scene): void {
    const turning = scene.query([Light], { write: [Transform] });
    turning.forEachChunk((chunk) => {
        const { values, offset, stride } = chunk.floats(Transform, 'matrix');
        for (let i = 0; i < chunk.count; ++i) values[offset + i * stride + 12] += 1;
        const first: Entity = chunk.entity(0);
        void first;
    });
    // @ts-expect-error the written components are a list
    scene.query([Light], { write: Transform });
    // @ts-expect-error a chunk's columns are floats, not a component view
    turning.forEachChunk((chunk) => chunk.floats(Transform, 'matrix').intensity);
}

export function misuse(entity: Entity, error: OpenEngineError): void {
    // @ts-expect-error an enum field takes the enumerator names only
    entity.set(Light, { type: 'Pointy' });
    // @ts-expect-error a field the component does not reflect
    entity.set(Light, { brightness: 2 });
    // @ts-expect-error a value of the wrong type
    entity.set(Camera, { fovY: '60' });
    // @ts-expect-error a component's fields are not on the entity
    entity.intensity = 3;
    // @ts-expect-error bounds can be absent on an arbitrary entity
    entity.bounds.center;
    // @ts-expect-error every entity has a Transform: it is not added
    entity.add(Transform);
    // @ts-expect-error every entity has a Transform: it is not removed
    entity.remove(Transform);
    // @ts-expect-error an EntityHandle field takes an Entity, not an id
    entity.set(SkyEnvironment, { sunLight: 3 });
    // @ts-expect-error the error codes are a closed set
    const code: 'Oops' = error.code;
    void code;
}
