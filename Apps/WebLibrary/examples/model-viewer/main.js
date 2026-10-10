// A model viewer: pick a sample model or open your own .glb, orbit it (drag to orbit, right-drag
// to pan, wheel to zoom), set the time of day and the exposure, play its clips and stand it on a floor; with ?gi=1
// the panel also offers global illumination as a preview (light bouncing off the floor and the model). The panel is panel.js.
// Models: Khronos glTF Sample Assets, listed in panel.js and ../ASSET_PROVENANCE.md.
// License: CC0 1.0, CC BY 4.0 and CC BY-NC 4.0 (the listed models, credited on screen; the last is
// non-commercial); MIT (coi-serviceworker.js).
// Needs: WebGPU; @openengine/web; coi-serviceworker.js beside the engine module for the threaded build.
import { Camera, DDGIVolume, Engine, OrbitControls } from '@openengine/web';
import { downloadProgress, fitStage, fitToFreeArea, showFailure, standardise, turnAbout } from '../shared/page-status.js';
import { controlPanel } from './panel.js';

const canvas = /** @type {HTMLCanvasElement} */ (document.querySelector('canvas'));
const engine = await Engine.create({ canvas, threads: 'auto', onProgress: downloadProgress('the engine', 'Starting the engine...') })
    .catch(showFailure);   // the status shows the facade's text, which names what to do (no WebGPU, ...)
const { scene } = engine;
scene.camera.transform.position.set(0.55, 0.15, 0.78);   // the orbit starts at the front three-quarter, faces toward the panel, 9 degrees up (glTF models face +Z)
const controls = new OrbitControls(scene.camera, canvas), startPitch = controls.pitch;   // every model starts at this elevation unless its entry gives one
let model = /** @type {import('@openengine/web').Model | null} */ (null), shape = /** @type {import('../shared/page-status.js').Shape | null} */ (null), fitted = 1;   // fitted: the distance the last fit chose; the visitor's zoom is distance / fitted
const stage = /** @type {Record<'floor' | 'gi', import('@openengine/web').Entity | null>} */ ({ floor: null, gi: null });   // the floor and the GI volume, each null while off
globalThis.viewer = { engine, controls, model: () => model, shape: () => shape, stage };   // for the browser console and the gate

/** Shows the model at `url` in place of the current one: the orbit at its starting elevation (`pitch`), the model at its starting turn (`yaw`), degrees, its emission `emissiveStrength` times the file's. @param {string} url @param {(progress: import('@openengine/web').LoadProgress) => void} onProgress @param {{ pitch?: number, yaw?: number, emissiveStrength?: number }} [look] */
async function show(url, onProgress, { pitch = startPitch, yaw = 0, emissiveStrength } = {}) {
    const next = await scene.load(url, { onProgress, emissiveStrength });
    model?.destroy();
    model = next;
    controls.pitch = pitch;
    shape = standardise(next, yaw);   // the box as shown (before its starting turn): the turntable turns the model about its centre
    frame();
    fitStage(stage, shape);
    try { return next.animation.clips; } catch { return []; }   // a model without clips throws
}

/** Fits the shown model to the area the panel and the overlays leave free (its box as it stands at rest, the hull it sweeps while turning), keeping the visitor's zoom when `keepZoom`. @param {boolean} [keepZoom] */
function frame(keepZoom = false) {
    if (!shape) return;
    controls.update();   // the camera as the orbit now stands: the fit reads its rotation
    const zoom = keepZoom ? controls.distance / fitted : 1;
    const fit = fitToFreeArea(canvas, shape, scene.camera.get(Camera)?.fovY ?? 60, scene.camera.transform.quaternion, zoom, panel.turning() ? null : model?.transform.quaternion);
    fitted = fit.fitted;
    controls.target.copy(fit.target);
    controls.distance = fit.distance;
    controls.minDistance = fit.radius / 3;
    scene.camera.set(Camera, { nearZ: fit.radius / 100 });
}

/** Switches the floor (floor.glb, a disc fading out toward its rim) or GI (a DDGIVolume) on, or off, destroyed. */
async function setStage(/** @type {'floor' | 'gi'} */ part, /** @type {boolean} */ on) {
    stage[part]?.destroy();
    stage[part] = !on ? null : part === 'floor' ? await scene.load('floor.glb') : scene.create('Global illumination');
    if (part === 'gi') stage.gi?.set(DDGIVolume, {});
    fitStage(stage, shape);
}
const panel = controlPanel(document, { show, timeOfDay: 15, setTimeOfDay: (hours) => { scene.sky.timeOfDay = hours; }, exposure: 2, setExposure: (ev) => scene.camera.set(Camera, { exposureCompensation: ev }), setStage, play: (clip, speed) => model?.animation.play(clip, { speed }), pause: () => model?.animation.pause(), reframe: () => frame(true) });
engine.onFrame((dt) => {
    if (panel.turning() && model && shape) turnAbout(model, shape, 10 * dt);
    panel.frame(dt);
});
engine.run();   // the sky draws while the first model downloads
