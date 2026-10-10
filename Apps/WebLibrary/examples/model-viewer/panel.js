// The model viewer's control panel: the sample model list, opening or dropping a local file,
// the time of day, the exposure compensation, the turntable and floor switches, the global
// illumination switch (a preview shown only with ?gi=1), the shown model's animation (animation.js),
// a frame-rate readout toggled with F or its button, and the download progress of the engine and of each model.
// It calls back into the page (main.js) and never into the engine.
// Models: Khronos glTF Sample Assets (sources and credits in ../ASSET_PROVENANCE.md):
// https://github.com/KhronosGroup/glTF-Sample-Assets. The two helmets ship there as .gltf with
// separate files; the demo site hosts single-file .glb copies of them under models/.
// License: CC0 1.0, except the models credited in the list (CC BY 4.0; the Damaged helmet also
// CC BY-NC 4.0, non-commercial). A model that needs a credit shows it, with links, while it is on screen.
// Needs: the shared status helpers (../shared/page-status.js).
import { downloadProgress, FrameMeter, freeArea } from '../shared/page-status.js';
import { animationSection } from './animation.js';

const kSampleBase = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models';
const sample = (/** @type {string} */ file) => `${kSampleBase}/${file.replace(/\.glb$/, '')}/glTF-Binary/${file}`;
const onSite = (/** @type {string} */ file) => new URL(`../../../models/${file}`, import.meta.url).href;
// A credit or a note is a list of [text, link?] pieces, shown while the model is on screen; a
// piece ['\n'] starts a new line. A license name and a copyright year keep no-break spaces.
const kOpaqueGlass = [['Its glass draws opaque: the importer does not read glass (transmission) yet.']];
const kCcBy = ['CC BY 4.0', 'https://creativecommons.org/licenses/by/4.0/'];
const kKhronos = 'https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models';
const kFoxCredit = [['Fox', `${kKhronos}/Fox`], [': model © 2014 PixelMannen (CC0); rigging and animation © 2014 tomkranis, '],
    kCcBy, ['; glTF conversion © 2017 @AsoboStudio and @scurest, '], kCcBy, ['.']];
const kTruckCredit = [['Cesium Milk Truck', `${kKhronos}/CesiumMilkTruck`], [': © 2017 Cesium, '], kCcBy, ['.']];
const kCcByNc = ['CC BY‑NC 4.0', 'https://creativecommons.org/licenses/by-nc/4.0/'];
const kDamagedHelmetCredit = [['Damaged Helmet', `${kKhronos}/DamagedHelmet`], [': © 2018 ctxwing, '], kCcBy,
    [' (rebuild and conversion);'], ['\n'], ['© 2016 theblueturtle_, '], kCcByNc, [' (the model; non-commercial).']];
/** [name, url, megabytes of the download (shown in the list and while it loads), credit or note shown with the model,
 * how the page shows it (main.js: `pitch`, the orbit's starting elevation, negative looking down; `yaw`, the model's starting
 * turn on the turntable; degrees; `emissiveStrength`, a multiple of the file's emission, so that it reads in daylight at
 * +2 EV without clipping)];
 * the first opens with the page, playing its first clip. */
const kSamples = [
    ['Fox', sample('Fox.glb'), 0, kFoxCredit],
    ['Water bottle', sample('WaterBottle.glb'), 9],
    ['SciFi helmet', onSite('SciFiHelmet.glb'), 30],
    ['Flight helmet', onSite('FlightHelmet.glb'), 48, kOpaqueGlass],
    ['Damaged helmet', sample('DamagedHelmet.glb'), 4, kDamagedHelmetCredit, { pitch: -20, yaw: 35, emissiveStrength: 256 }],   // its face toward the camera
    ['Lantern', sample('Lantern.glb'), 0, kOpaqueGlass, { emissiveStrength: 128 }],
    ['Antique camera', sample('AntiqueCamera.glb')],
    ['Boom box', sample('BoomBox.glb')],
    ['Corset', sample('Corset.glb')],
    ['Avocado', sample('Avocado.glb')],
    ['Barramundi fish', sample('BarramundiFish.glb')],
    ['Metal and roughness spheres', sample('MetalRoughSpheresNoTextures.glb')],
    ['Cesium milk truck', sample('CesiumMilkTruck.glb'), 0, kTruckCredit],
];
// The engine fetches one file per model, so a .gltf must embed its buffers and images; FBX has
// no importer on the web.
const kLocalRule = 'Open a .glb, or a .gltf with its buffers and images embedded. FBX is not supported on the web.';

const clock = (/** @type {number} */ hours) => `${Math.floor(hours)}:${String(Math.round((hours % 1) * 60) % 60).padStart(2, '0')}`;
const stops = (/** @type {number} */ ev) => `${ev > 0 ? '+' : ev < 0 ? '−' : ''}${Math.abs(ev).toFixed(1)} EV`;

/**
 * The files a .gltf names outside itself (its buffers' and images' URIs that are not data: URIs),
 * which a page cannot hand the engine with it.
 * @param {File} gltf
 */
async function externalFiles(gltf) {
    try {
        const { buffers = [], images = [] } = JSON.parse(await gltf.text());
        return [...buffers, ...images].map((entry) => entry.uri).filter((uri) => uri && !uri.startsWith('data:'));
    } catch {
        return [];   // not JSON: the engine's loader reports what is wrong with it
    }
}

/** Writes [text, link?] pieces into `element`. @param {HTMLElement} element @param {string[][]} pieces */
function writePieces(element, pieces) {
    element.replaceChildren(...pieces.map(([text, href]) => {
        if (text === '\n') return document.createElement('br');
        if (!href) return document.createTextNode(text);
        const link = Object.assign(document.createElement('a'), { href, textContent: text, target: '_blank' });
        return link;
    }));
}

/**
 * @param {Document} page
 * @param {{ show: (url: string, onProgress: (progress: import('@openengine/web').LoadProgress) => void, look: { pitch?: number, yaw?: number, emissiveStrength?: number }) => Promise<string[]>,
 *     timeOfDay: number, setTimeOfDay: (hours: number) => void, exposure: number, setExposure: (ev: number) => void,
 *     setStage: (part: 'floor' | 'gi', on: boolean) => Promise<void>, play: (clip: string, speed: number) => void, pause: () => void,
 *     reframe: () => void }} callbacks
 *     `show` stands the model by `look` (the sample's entry) and resolves to its clip names; `exposure` is the starting
 *     compensation in stops (the camera meters the whole frame, so at 0 a model under the daylight sky reads darker than
 *     the eye sees it); `reframe` fits the model to the area the panel
 *     and the overlays leave free after the panel opens or closes, or the credit or the readout changes,
 *     keeping the visitor's zoom.
 * @returns {{ frame: (dt: number) => void, turning: () => boolean }} what the page calls each frame,
 *     and whether the turntable switch is on
 */
export function controlPanel(page, { show, timeOfDay, setTimeOfDay, exposure, setExposure, setStage, play, pause, reframe }) {
    const field = (/** @type {string} */ name) => /** @type {any} */ (page.querySelector(`.panel [name=${name}]`));
    const [list, file, slider, timeOutput, exposureSlider, exposureOutput, fpsButton, status] =
        ['model', 'file', 'time', 'clock', 'exposure', 'ev', 'fps', 'status'].map(field);
    const credit = /** @type {HTMLElement} */ (page.querySelector('.credit'));
    const bar = /** @type {HTMLProgressElement} */ (page.querySelector('progress.download'));
    const summaryProgress = /** @type {HTMLElement} */ (page.querySelector('.panel .summary-progress'));
    const canvasStatus = /** @type {HTMLElement} */ (page.querySelector('.canvas-status'));
    const readout = /** @type {HTMLElement} */ (page.querySelector('.fps'));
    const meter = new FrameMeter(readout);
    /** A model download runs: the panel's title counts it, and the playback waits for its end. */
    let loading = false;
    const setTitle = (/** @type {string} */ title) => { if (!loading) summaryProgress.textContent = title; };
    const animation = animationSection(page, { play, pause, setTitle });
    /** @type {HTMLElement} */ (page.querySelector('.panel')).addEventListener('toggle', reframe);
    const canvas = /** @type {HTMLCanvasElement} */ (page.querySelector('canvas'));
    /**
     * Runs `change` (to the credit or the readout) and re-frames the model only when the free area
     * moved: on a wide page it does not, and a camera the visitor zoomed or panned stays put.
     * @param {() => void} change
     */
    const reframeIfMoved = (change) => {
        const before = JSON.stringify(freeArea(canvas));
        change();
        if (JSON.stringify(freeArea(canvas)) !== before) reframe();
    };

    /** The model on screen, named in the status once a refusal or an error is behind us. */
    let shown = '';
    const settle = () => { if (shown) status.value = `Showing ${shown}`; };
    const load = async (/** @type {string} */ url, /** @type {string} */ label, /** @type {string[][]} */ credited = [],
        megabytes = 0, /** @type {{ pitch?: number, yaw?: number, emissiveStrength?: number }} */ look = {}) => {
        status.value = megabytes ? `Loading ${label} (${megabytes} MB)...` : `Loading ${label}...`;
        loading = true;
        try {
            const clips = await show(url, downloadProgress(label), look);
            shown = label;
            status.value = `Showing ${label}`;
            reframeIfMoved(() => {
                writePieces(credit, credited);
                credit.hidden = credited.length === 0;
            });
            animation.show(clips);
            return true;
        } catch (error) {
            status.value = `Could not load ${label}: ${error instanceof Error ? error.message : error}`;
            return false;
        } finally {
            bar.hidden = true;
            loading = false;
            setTitle(animation.title());   // the model on screen's playback
        }
    };
    // A local file reaches the engine as an object URL; its name after '#' picks the loader.
    const open = async (/** @type {File | undefined} */ chosen) => {
        if (!chosen || !/\.(glb|gltf)$/i.test(chosen.name)) {
            status.value = kLocalRule;
            return;
        }
        const missing = /\.gltf$/i.test(chosen.name) ? await externalFiles(chosen) : [];
        if (missing.length) {
            status.value = `${chosen.name} refers to files beside it (${missing.join(', ')}). ${kLocalRule}`;
            return;
        }
        // The list names the file while it is shown; picking a sample takes the entry away. A file
        // that does not load gives the list back to what is still on screen.
        const shownOption = list.selectedOptions[0];
        const shownFile = yourFile;
        shownFile?.remove();
        yourFile = new Option(`Your file: ${chosen.name}`, '', true, true);
        yourFile.disabled = true;
        list.add(yourFile);
        const url = URL.createObjectURL(chosen);
        const loaded = await load(`${url}#${chosen.name}`, chosen.name);
        URL.revokeObjectURL(url);   // the engine has read the file, or failed to
        if (loaded) return;
        yourFile.remove();
        yourFile = shownFile;
        if (shownFile) list.add(shownFile);
        shownOption.selected = true;
    };
    /** @type {HTMLOptionElement | null} */
    let yourFile = null;
    const setTime = (/** @type {number} */ hours) => {
        setTimeOfDay(hours);
        timeOutput.value = clock(hours);
    };
    const setStops = (/** @type {number} */ ev) => {
        setExposure(ev);
        exposureOutput.value = stops(ev);
    };
    const toggleFps = () => reframeIfMoved(() => {
        readout.hidden = !readout.hidden;
        readout.textContent ||= 'Measuring...';   // its height while the first second is measured
    });

    for (const [name, url, megabytes] of kSamples) {
        const option = new Option(megabytes ? `${name} (${megabytes} MB)` : String(name), String(url));
        option.dataset.name = String(name);
        list.add(option);
    }
    const pick = () => {
        yourFile?.remove();
        yourFile = null;
        const [name, url, megabytes, credited, look] = kSamples[list.selectedIndex];
        return load(String(url), String(name), /** @type {string[][]} */ (credited ?? []), Number(megabytes ?? 0), /** @type {{ pitch?: number, yaw?: number, emissiveStrength?: number }} */ (look ?? {}));
    };
    list.addEventListener('change', pick);
    file.addEventListener('change', () => {
        const chosen = file.files[0];
        file.value = '';   // so that choosing the same file again opens it again
        open(chosen);
    });
    page.addEventListener('dragover', (event) => { event.preventDefault(); page.body.classList.add('dragging'); });
    page.addEventListener('dragleave', () => page.body.classList.remove('dragging'));
    page.addEventListener('drop', (event) => {
        event.preventDefault();
        page.body.classList.remove('dragging');
        open(event.dataTransfer?.files[0]);
    });
    slider.value = String(timeOfDay);
    slider.addEventListener('input', () => { setTime(Number(slider.value)); settle(); });
    exposureSlider.value = String(exposure);
    if (exposure !== 0) exposureSlider.list.append(new Option('', String(exposure)));
    exposureSlider.addEventListener('input', () => { setStops(Number(exposureSlider.value)); settle(); });
    fpsButton.addEventListener('click', toggleFps);
    const turntable = field('turn');
    turntable.addEventListener('change', settle);   // the switch starts or stops the turn where the model stands: the orbit stays put
    const [floor, gi] = ['floor', 'gi'].map(field);
    // Global illumination waits for probe depth on the web: a preview behind ?gi=1 until then.
    const giPreview = new URLSearchParams(page.location.search).get('gi') === '1';
    for (const element of [gi.closest('label'), page.querySelector('.panel .gi-hint')]) element.hidden = !giPreview;
    for (const [part, box] of /** @type {const} */ ([['floor', floor], ['gi', gi]])) {
        // One change at a time, each applying the switch as it then stands: loading the floor
        // takes a moment, and a quick off and on again must end with the switch's state.
        let changes = setStage(part, box.checked);
        box.addEventListener('change', () => { changes = changes.then(() => setStage(part, box.checked)); settle(); });
    }
    page.addEventListener('keydown', (event) => {
        if (event.code === 'KeyF' && !['INPUT', 'SELECT'].includes(/** @type {Element} */ (event.target).tagName)) toggleFps();
    });
    setTime(timeOfDay);
    setStops(exposure);
    for (const control of [list, file, slider, exposureSlider, turntable, floor, gi, fpsButton]) control.disabled = false;
    pick();   // the first model; the page runs the engine meanwhile

    return {
        frame: (/** @type {number} */ dt) => {
            canvasStatus.hidden = true;   // the canvas draws from here on
            meter.add(dt);
        },
        turning: () => turntable.checked,
    };
}
