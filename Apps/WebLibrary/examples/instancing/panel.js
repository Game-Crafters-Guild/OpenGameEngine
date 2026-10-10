// The instancing page's control panel: how many copies, the turning switch (on the page only with
// ?turntable=1), and a frame-rate readout toggled with F or its button. It calls back into the page
// (main.js) and never into the engine; the status, the download bar and the readout are the shared
// page helpers.
// Needs: the shared status helpers (../shared/page-status.js).
import { FrameMeter, setStatus } from '../shared/page-status.js';

const kDefaultCount = 1000;

/**
 * Starts the panel once the engine and the model are in: the downloads are over.
 * @param {Document} page
 * @param {{ setCount: (count: number) => void, setTurning: (on: boolean) => void }} callbacks
 */
export function controlPanel(page, { setCount, setTurning }) {
    const field = (/** @type {string} */ name) => /** @type {any} */ (page.querySelector(`.panel [name=${name}]`));
    const [slider, countOutput, turn, fpsButton] = ['count', 'shown', 'turn', 'fps'].map(field);
    const canvasStatus = /** @type {HTMLElement} */ (page.querySelector('.canvas-status'));
    const summary = /** @type {HTMLElement} */ (page.querySelector('.panel .summary-progress'));
    /** @type {HTMLElement} */ (page.querySelector('progress.download')).hidden = true;
    const readout = /** @type {HTMLElement} */ (page.querySelector('.fps'));
    const meter = new FrameMeter(readout);
    const toggleFps = () => { readout.hidden = !readout.hidden; };
    const apply = () => {
        const count = Number(slider.value).toLocaleString('en-US');
        setCount(Number(slider.value));
        countOutput.value = count;
        setStatus(`${count} entities, each with its own Transform and MeshRenderer`);
        summary.textContent = `· ${count} copies`;   // what a closed panel (a phone) shows
    };

    slider.value = String(kDefaultCount);
    slider.addEventListener('input', apply);
    turn?.addEventListener('change', () => setTurning(turn.checked));
    fpsButton.addEventListener('click', toggleFps);
    page.addEventListener('keydown', (event) => {
        if (event.code === 'KeyF' && !['INPUT', 'SELECT'].includes(/** @type {Element} */ (event.target).tagName)) toggleFps();
    });
    apply();
    for (const control of [slider, turn, fpsButton]) if (control) control.disabled = false;
    return {
        frame: (/** @type {number} */ dt) => {
            canvasStatus.hidden = true;   // the canvas draws from here on
            meter.add(dt);
        },
    };
}
