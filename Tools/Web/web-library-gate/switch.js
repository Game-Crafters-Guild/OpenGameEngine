// The browser gate's model-switch check, staged beside the model-viewer example by
// Tools/Web/stage_web_library.py: a viewer that shows one model at a time, as a page switching
// between models does. browser_gate.py --step "geSwitchTo('<name>')" loads the next one (a URL
// shown before comes back as another copy of the asset already loaded), destroys the previous
// one, frames it and resolves once it is shown; the gate then reads the frame.
// ?threads=single|auto picks the build, as Engine.create's option does.
import { Engine, OrbitControls } from '@openengine/web';

const kSampleBase = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models';

const threads = new URLSearchParams(location.search).get('threads') ?? 'auto';
const canvas = document.querySelector('canvas');
const engine = await Engine.create({ canvas, threads });
const { scene } = engine;
const controls = new OrbitControls(scene.camera, canvas);
scene.sky.timeOfDay = 15;
engine.run();

let model = null;

/** Shows the sample model `name` (WaterBottle, Lantern, ...) in place of the current one. */
globalThis.geSwitchTo = async (name) => {
    const next = await scene.load(`${kSampleBase}/${name}/glTF-Binary/${name}.glb`);
    model?.destroy();
    model = next;
    const { center, size } = model.bounds;
    controls.target.copy(center);
    controls.distance = 2 * Math.max(size.x, size.y, size.z);
    console.log(`switch-step: showing ${name}`);
};
console.log('switch-page: ready');
