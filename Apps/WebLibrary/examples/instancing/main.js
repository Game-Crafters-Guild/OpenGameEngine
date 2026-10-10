// Instancing: one model drawn up to 100,000 times, each copy an entity with the model's MeshRenderer on a
// sunflower spiral; switched on, every copy turns about world up each frame by one query over their Transforms.
// Model: Avocado, Khronos glTF Sample Assets (../ASSET_PROVENANCE.md).
// License: CC0 1.0 (the model); MIT (coi-serviceworker.js).
// Needs: WebGPU; @openengine/web; coi-serviceworker.js beside the engine module for the threaded build.
import { Camera, Engine, MeshRenderer, OrbitControls, Transform } from '@openengine/web';
import { downloadProgress, fitDistance, setStatus } from '../shared/page-status.js';
import { controlPanel } from './panel.js';
import { framedRadius, spiralPlace, turnAboutUp } from './layout.js';

const kModel = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Avocado/glTF-Binary/Avocado.glb';
const kScale = 15;   // the avocado is 6 cm long: 90 cm here
const kTurnDegreesPerSecond = 45;
const canvas = /** @type {HTMLCanvasElement} */ (document.querySelector('canvas'));
const onProgress = downloadProgress('the engine', 'Starting the engine...');
const engine = await Engine.create({ canvas, threads: 'auto', onProgress }).catch((error) => {
    setStatus(error.message);   // the facade's own text names what to do (no WebGPU, ...)
    throw error;
});
const { scene } = engine;
const avocado = await scene.load(kModel, { onProgress: downloadProgress('the avocado') });
// The model's mesh is a child of its root (at the origin); the copies draw it with its material.
const part = avocado.transform.children.find((child) => child.has(MeshRenderer)), mesh = part?.get(MeshRenderer);
if (!part || !mesh) throw new Error('The model has no mesh under its root; this page copies a single mesh.');
const { meshId, meshGpuHandleId, materialAssetGuid, modelAssetGuid } = mesh;
const instances = [part];   // the model's mesh and its copies, in spiral order
let turning = false;   // the panel's switch (with ?turntable=1): off, so the page opens at the rate of copies at rest
/** Places copy `index` on the spiral, turned by its golden angle. @param {import('@openengine/web').Entity} entity @param {number} index */
function place(entity, index) {
    const { x, z, yawDegrees } = spiralPlace(index);
    entity.transform.position.set(x, 0, z);
    entity.transform.scale.set(kScale, kScale, kScale);
    entity.transform.rotation.set(0, yawDegrees, 0);
}
scene.camera.transform.position.set(0, 1.5, -4);   // the orbit starts 20 degrees above the spiral
const controls = new OrbitControls(scene.camera, canvas);
globalThis.demo = { engine, controls, instances, get turning() { return turning; } };   // for the browser console and the gate
const copies = scene.query([MeshRenderer], { write: [Transform] });   // the model's mesh and every copy
/** Adds or destroys copies until there are `count`; the model viewer's fit puts the spiral's sphere in the free area. @param {number} count */
function setCount(count) {
    while (instances.length > count) instances.pop()?.destroy();
    while (instances.length < count) {
        const copy = scene.create();
        copy.set(MeshRenderer, { meshId, meshGpuHandleId, materialAssetGuid, modelAssetGuid });
        place(copy, instances.length);
        instances.push(copy);
    }
    controls.distance = fitDistance(canvas, framedRadius(count), scene.camera.get(Camera)?.fovY ?? 60);
}
place(part, 0);
const panel = controlPanel(document, { setCount, setTurning: (on) => { turning = on; } });
engine.onFrame((dt) => {
    if (turning) copies.forEachChunk((chunk) => {   // one walk over every copy's Transform, a chunk per engine call
        const { values, offset, stride } = chunk.floats(Transform, 'matrix');
        turnAboutUp(values, offset, stride, chunk.count, kTurnDegreesPerSecond * dt);
    });
    panel.frame(dt);
});
engine.run();
