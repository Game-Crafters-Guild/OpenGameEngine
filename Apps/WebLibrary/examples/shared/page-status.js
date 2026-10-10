// What every example page shows around its canvas: the status (in the panel, over the canvas
// until the first frame, and beside a closed panel's title), the download bar along the top edge,
// the frame-rate readout, the canvas area the panel and the overlays leave free, and where an orbit
// frames a model in that area (fitToFreeArea). The pages import it; it calls into the page's
// document and never into the engine.
// Needs: the page's `.panel output[name=status]`, `.canvas-status`, `.panel .summary-progress`
// and `progress.download` elements; an `.overlays` column is optional.

const kFpsWindowSeconds = 1;
/** The CSS pixels kept between the overlays across a narrow page's top and the free area. */
const kOverlayGap = 8;
/** How much larger than the model's projected box the free area stays, on its tighter axis. */
const kFitMargin = 1.15;
/** The refinements of the offset that puts the projected box's middle in the free area's middle. */
const kCentreSteps = 8;
/** The points sampled on each circle of the hull a model sweeps on a turntable. */
const kHullPoints = 16;
// The longest side of every model's box as the viewer shows it, in metres: person-sized, the scale
// the engine's metric defaults are set for. The fitted camera then stands a few metres off, far
// inside the sun's shadow distance (200 m, fading from 180 m), whatever unit the file was authored in.
const kModelReach = 2;

/** The frames of the last second: their rate and the longest one. */
export class FrameMeter {
    /** @param {HTMLElement} readout */
    constructor(readout) {
        this.readout = readout;
        /** @type {number[]} */
        this.frames = [];
        this.sinceUpdate = 0;
    }
    /** @param {number} dt the frame's time step, in seconds */
    add(dt) {
        this.frames.push(dt);
        let span = this.frames.reduce((sum, frame) => sum + frame, 0);
        while (span - this.frames[0] >= kFpsWindowSeconds) span -= this.frames.shift() ?? 0;
        this.sinceUpdate += dt;
        if (this.readout.hidden || this.sinceUpdate < 0.25) return;
        this.sinceUpdate = 0;
        const worst = Math.max(...this.frames) * 1000;
        this.readout.textContent = `${(this.frames.length / span).toFixed(0)} fps · worst ${worst.toFixed(1)} ms`;
    }
}

/**
 * Shows a failure's message as the status and rethrows it: the facade's own text names what to
 * do (no WebGPU, ...). For a promise's catch.
 * @param {Error} error
 * @returns {never}
 */
export function showFailure(error) {
    setStatus(error.message);
    throw error;
}

/**
 * Shows `text` as the status: in the panel, over the canvas until its first frame draws, and,
 * while a download runs (`downloading`), beside the panel's title, where a closed panel shows it.
 * @param {string} text @param {boolean} [downloading]
 */
export function setStatus(text, downloading = false) {
    /** @type {HTMLOutputElement} */ (document.querySelector('.panel output[name=status]')).value = text;
    /** @type {HTMLElement} */ (document.querySelector('.canvas-status')).textContent = text;
    /** @type {HTMLElement} */ (document.querySelector('.panel .summary-progress')).textContent = downloading ? `· ${text}` : '';
}

/**
 * An onProgress for a download (the engine's, or a model's) that shows "Loading <label> 37%"
 * as the status and fills the bar along the page's top edge from empty; the engine's phases add
 * up. A percentage, not megabytes: the bytes are the network's only when the host sends their
 * length. The bar runs without a value while a size is unknown, and once every byte is in when
 * the status then says `whenDownloaded` (work that follows the download, of unknown length).
 * @param {string} label @param {string} [whenDownloaded]
 */
export function downloadProgress(label, whenDownloaded) {
    const bar = /** @type {HTMLProgressElement} */ (document.querySelector('progress.download'));
    bar.value = 0;
    /** @type {Map<string, { loaded: number, total: number }>} */
    const phases = new Map();
    return (/** @type {import('@openengine/web').LoadProgress} */ { phase, loaded, total }) => {
        phases.set(phase, { loaded, total });
        const all = [...phases.values()];
        bar.hidden = false;
        if (all.some((one) => !one.total)) {
            bar.removeAttribute('value');
            setStatus(`Loading ${label}...`, true);
            return;
        }
        const sum = all.reduce((sum, one) => ({ loaded: sum.loaded + one.loaded, total: sum.total + one.total }));
        if (sum.loaded === sum.total && whenDownloaded) {
            bar.removeAttribute('value');
            setStatus(whenDownloaded, true);
            return;
        }
        const percent = `${Math.floor((100 * sum.loaded) / sum.total)}%`;
        bar.max = sum.total;
        bar.value = sum.loaded;
        setStatus(`Loading ${label} ${percent}`, true);
    };
}

/**
 * The canvas area, in CSS pixels, that the panel and the page's overlays leave free: the open
 * panel is a column on the right, or a strip along the bottom on a narrow page, where its closed
 * title strip counts too; each overlay that shows (children of an `.overlays` column: a model's
 * credit, and the frame-rate readout where the column runs across a narrow page's top) takes the
 * area's top when it sits in the top half and its bottom when it sits in the bottom half.
 * @param {HTMLCanvasElement} canvas
 */
export function freeArea(canvas) {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    let top = 0, bottom = height, right = width;
    const panel = /** @type {HTMLDetailsElement | null} */ (document.querySelector('.panel'));
    if (panel) {
        // Open: a column on the right, or a strip along the bottom. Closed: a narrow page's title
        // strip along the bottom still covers it; a wide page's title sits in its corner.
        const card = panel.getBoundingClientRect();
        if (panel.open && card.left > width / 2) right = card.left;
        else if (panel.open || (card.top > height / 2 && card.left < width / 2)) bottom = card.top - (panel.open ? 0 : kOverlayGap);
    }
    // On a narrow page the overlays column runs across the top; on a wide page it runs down the left
    // edge, and the frame-rate readout in its corner leaves the area alone (F does not re-frame).
    const overlays = document.querySelector('.overlays');
    const acrossTop = !!overlays && overlays.getBoundingClientRect().bottom < height / 2;
    for (const element of overlays?.children ?? []) {
        if ((/** @type {HTMLElement} */ (element)).hidden) continue;
        if (element.classList.contains('fps') && !acrossTop) continue;
        const box = element.getBoundingClientRect();
        if (box.bottom < height / 2) top = Math.max(top, box.bottom + kOverlayGap);
        else bottom = Math.min(bottom, box.top - kOverlayGap);
    }
    return { left: 0, top, width: right, height: bottom - top };
}

/**
 * @typedef {{ center: { x: number, y: number, z: number }, size: { x: number, y: number, z: number },
 *     min: { x: number, y: number, z: number }, rest: { x: number, y: number, z: number, w: number } }} Shape
 * A shown model's box as it stood when shown (yaw 0), and its rotation then: the turntable's turn
 * is measured from that rotation, about the box's centre.
 */

/**
 * Where a point lands after a turn of `degrees` about the vertical axis through `centre`, the way
 * rotateWorldY turns (+Z toward +X).
 * @param {{ x: number, y: number, z: number }} point @param {{ x: number, z: number }} centre @param {number} degrees
 * @returns {[number, number, number]}
 */
function turnedAbout(point, centre, degrees) {
    const angle = (degrees * Math.PI) / 180, x = point.x - centre.x, z = point.z - centre.z;
    return [centre.x + x * Math.cos(angle) + z * Math.sin(angle), point.y, centre.z - x * Math.sin(angle) + z * Math.cos(angle)];
}

/**
 * Turns a shown model `degrees` about the vertical axis through its box's centre, as the turntable does.
 * @param {import('@openengine/web').Model} model @param {Shape} shape @param {number} degrees
 */
export function turnAbout(model, shape, degrees) {
    model.transform.position.set(...turnedAbout(model.transform.position, shape.center, degrees));
    model.transform.rotateWorldY(degrees);
}

/**
 * The turn about world up, in degrees, that takes the rotation `rest` to `rotation`.
 * @param {{ x: number, y: number, z: number, w: number }} rotation @param {{ x: number, y: number, z: number, w: number }} rest
 */
function turnFrom(rotation, rest) {
    const { x, y, z, w } = rotation;
    return (2 * Math.atan2(-w * rest.y + x * rest.z + y * rest.w - z * rest.x, w * rest.w + x * rest.x + y * rest.y + z * rest.z) * 180) / Math.PI;
}

/**
 * Stands a loaded model the way the viewer shows every model: scaled uniformly so that its box's
 * longest side is kModelReach metres, then turned `yaw` degrees on the turntable (its starting turn).
 * Returns its shape (the box before that turn, and its rotation then), or null for a model with
 * nothing to draw (no box).
 * @param {import('@openengine/web').Model} model @param {number} [yaw]
 * @returns {Shape | null}
 */
export function standardise(model, yaw = 0) {
    const box = /** @type {import('@openengine/web').Bounds | null} */ (model.bounds);
    if (!box) return null;
    const reach = Math.max(box.size.x, box.size.y, box.size.z), k = model.transform.scale;
    if (reach > 0) k.set((k.x * kModelReach) / reach, (k.y * kModelReach) / reach, (k.z * kModelReach) / reach);
    const { center, size, min } = /** @type {import('@openengine/web').Bounds} */ (model.bounds), q = model.transform.quaternion;
    const shape = { center, size, min, rest: { x: q.x, y: q.y, z: q.z, w: q.w } };
    turnAbout(model, shape, yaw);
    return shape;
}

/**
 * Stands the stage under a model: the floor centred on the model's feet, opaque out to 1.05 times
 * the model's size from there and gone by 2.85 times, a disc that ends inside the frame rather than
 * at the horizon, and the GI volume fitted to the model. Does nothing before a model is shown.
 * @param {Record<'floor' | 'gi', import('@openengine/web').Entity | null>} stage the floor and the GI volume, each null while off
 * @param {{ center: { x: number, y: number, z: number }, size: { x: number, y: number, z: number }, min: { y: number } } | null} shape the model's box as the page loaded it
 */
export function fitStage(stage, shape) {
    if (!shape) return;
    const { center, size, min } = shape;
    const reach = Math.max(size.x, size.y, size.z);
    stage.floor?.transform.position.set(center.x, min.y, center.z);
    stage.floor?.transform.scale.set(3 * reach, 1, 3 * reach);   // floor.glb: a 1 m disc, opaque to 0.35 m, clear by 0.95 m
    stage.gi?.transform.position.set(center.x, center.y, center.z);
    stage.gi?.transform.scale.set(1.3 * reach, 1.3 * reach, 1.3 * reach);
}

/**
 * Where an orbit frames a model in the canvas area the open panel and the overlays leave free: the
 * distance at which the hull that holds the model at any turn of the turntable (the circle its box
 * sweeps about its own centre, at the box's bottom and top), projected along the camera's view, fits that area with a
 * margin, times `zoom` (the visitor's zoom over the fit, 1 for none); and the orbit target moved
 * off the box's centre along the view's right and up so that, for a model at rest (`rotation` given,
 * the model's own), its box as it now stands has its projected middle in the area's middle, and for a
 * turning model (no `rotation`) the hull has, so the model stays balanced as it turns. The view comes from the camera's rotation, which the
 * orbit writes every frame, not from its position, which lags a target the page has just moved.
 * Plain numbers; the page writes them into its OrbitControls.
 * @param {HTMLCanvasElement} canvas
 * @param {Shape} shape the model's shape (standardise)
 * @param {number} fovY the camera's vertical field of view, in degrees
 * @param {{ x: number, y: number, z: number, w: number }} rotation the camera's rotation (it looks along its +Z)
 * @param {number} [zoom] the camera's distance over the fitted one
 * @param {{ x: number, y: number, z: number, w: number } | null} [modelRotation] the model's rotation when it is at rest; none while it turns
 */
export function fitToFreeArea(canvas, shape, fovY, rotation, zoom = 1, modelRotation = null) {
    const { center, size } = shape;
    const radius = 0.5 * Math.hypot(size.x, size.y, size.z);
    // A model with no extent has nothing to fit: the orbit looks at it from where it is.
    if (!(radius > 0)) return { target: center, distance: 1, fitted: 1, radius: 1 };
    // A zoom that is not a positive number (a ratio over a fit that failed) is no zoom.
    if (!(zoom > 0 && Number.isFinite(zoom))) zoom = 1;
    const tanHalf = Math.tan((fovY * Math.PI) / 360);
    // An area with no room (a short page under an open panel) leaves the whole canvas.
    let { left, top, width, height } = freeArea(canvas);
    if (!(width > 0 && height > 0)) ({ left, top, width, height } = { left: 0, top: 0, width: canvas.clientWidth, height: canvas.clientHeight });
    // The view's forward (the rotation's +Z), right (world up × forward) and up (forward × right).
    const { x, y, z, w } = rotation;
    const [fx, fy, fz] = [2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)];
    const across = Math.hypot(fz, fx) || 1;
    const right = [fz / across, 0, -fx / across];
    const up = [fy * right[2] - fz * right[1], fz * right[0] - fx * right[2], fx * right[1] - fy * right[0]];
    // The box's projected extent, in CSS pixels about the view axis (y up), at `distance` along the
    // view from the box's centre, with the camera moved `dx` metres left and `dy` up of that line;
    // each side with the depth of the corner that sets it.
    const dot = (/** @type {number[]} */ a, /** @type {number[]} */ b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const pixels = canvas.clientHeight / (2 * tanHalf);
    // The model's box as it now stands (its corners turned by its turn since it was shown), and the
    // hull that holds it at every turn of the turntable, which turns it about the box's centre: the
    // circle through the box's corners, sampled at the box's bottom and top.
    const turn = modelRotation ? (turnFrom(modelRotation, shape.rest) * Math.PI) / 180 : 0;
    /** @type {number[][]} */
    const corners = [];
    for (const sx of [-0.5, 0.5]) for (const sy of [-0.5, 0.5]) for (const sz of [-0.5, 0.5]) {
        const x = sx * size.x, z = sz * size.z;
        corners.push([x * Math.cos(turn) + z * Math.sin(turn), sy * size.y, -x * Math.sin(turn) + z * Math.cos(turn)]);
    }
    const sweep = 0.5 * Math.hypot(size.x, size.z);
    /** @type {number[][]} */
    const hull = [];
    for (let i = 0; i < kHullPoints; ++i) {
        const angle = (2 * Math.PI * i) / kHullPoints;
        for (const sy of [-0.5, 0.5]) hull.push([sweep * Math.cos(angle), sy * size.y, sweep * Math.sin(angle)]);
    }
    const extent = (/** @type {number[][]} */ points, /** @type {number} */ distance, /** @type {number} */ dx,
        /** @type {number} */ dy) => {
        const sides = { left: [Infinity, 0], right: [-Infinity, 0], bottom: [Infinity, 0], top: [-Infinity, 0] };
        for (const corner of points) {
            const depth = distance + dot(corner, [fx, fy, fz]);
            const px = ((dot(corner, right) + dx) / depth) * pixels, py = ((dot(corner, up) - dy) / depth) * pixels;
            if (px < sides.left[0]) sides.left = [px, depth];
            if (px > sides.right[0]) sides.right = [px, depth];
            if (py < sides.bottom[0]) sides.bottom = [py, depth];
            if (py > sides.top[0]) sides.top = [py, depth];
        }
        return sides;
    };
    // The offsets that put the centred shape's projected middle (the box at rest, the hull while
    // turning) in the free area's middle (about the view axis): moving the camera sideways shifts each
    // side by the inverse of its point's depth, which gives each refinement's step.
    const centred = modelRotation ? corners : hull;
    const wantX = left + width / 2 - canvas.clientWidth / 2, wantUp = canvas.clientHeight / 2 - (top + height / 2);
    const centre = (/** @type {number} */ distance) => {
        let dx = 0, dy = 0;
        for (let step = 0; step < kCentreSteps; ++step) {
            const box = extent(centred, distance, dx, dy);
            dx += (wantX - (box.left[0] + box.right[0]) / 2) / (pixels * (1 / box.left[1] + 1 / box.right[1]) / 2);
            dy += ((box.bottom[0] + box.top[0]) / 2 - wantUp) / (pixels * (1 / box.bottom[1] + 1 / box.top[1]) / 2);
        }
        return { dx, dy };
    };
    // Whether the hull, centred, lies inside the free area shrunk by the margin.
    const fits = (/** @type {number} */ distance) => {
        const { dx, dy } = centre(distance);
        const box = extent(hull, distance, dx, dy);
        const halfWidth = width / (2 * kFitMargin), halfHeight = height / (2 * kFitMargin);
        return box.left[0] >= wantX - halfWidth && box.right[0] <= wantX + halfWidth
            && box.bottom[0] >= wantUp - halfHeight && box.top[0] <= wantUp + halfHeight;
    };
    // The nearest such distance, never inside the bounding sphere, found by halving a bracket.
    let near = radius, far = radius;
    while (!fits(far)) { near = far; far *= 2; }
    for (let step = 0; step < 40 && far - near > 1e-6 * far; ++step) {
        const middle = (near + far) / 2;
        if (fits(middle)) far = middle; else near = middle;
    }
    const fitted = far;
    // At a zoom the visitor chose the offsets scale with the distance, so the model's centre keeps
    // its place on screen.
    const distance = fitted * zoom;
    const { dx: fittedDx, dy: fittedDy } = centre(fitted);
    const dx = fittedDx * zoom, dy = fittedDy * zoom;
    const target = { x: center.x - right[0] * dx + up[0] * dy, y: center.y - right[1] * dx + up[1] * dy,
        z: center.z - right[2] * dx + up[2] * dy };
    return { target, distance, fitted, radius };
}

/**
 * The orbit distance at which a sphere of `radius` fits the shorter side of the free area (see
 * freeArea) with a margin, for a camera of vertical field of view `fovYDegrees`.
 * @param {HTMLCanvasElement} canvas @param {number} radius @param {number} fovYDegrees
 */
export function fitDistance(canvas, radius, fovYDegrees) {
    const { width, height } = freeArea(canvas);
    const halfFov = (fovYDegrees * Math.PI) / 360;
    return (1.15 * radius) / Math.sin(Math.atan((Math.tan(halfFov) * Math.min(width, height)) / canvas.clientHeight));
}
