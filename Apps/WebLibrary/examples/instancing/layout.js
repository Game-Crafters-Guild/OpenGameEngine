// The instancing page's arithmetic: where copy `index` sits on the sunflower spiral, how far the
// spiral's first copies reach, which the orbit frames, and the turn of a chunk of copies'
// matrices. It makes no engine call; main.js hands it the numbers and the arrays.

const kStep = 0.6;                                   // meters: neighbors on the spiral sit about 1 m apart
const kGoldenAngle = Math.PI * (3 - Math.sqrt(5));   // radians between consecutive copies
const kFramedCopies = 2500;   // past this many the view holds and the spiral runs off the frame's edges

/**
 * Copy `index`'s place: x and z in meters on the ground, and its yaw in degrees (its golden angle).
 * @param {number} index
 */
export function spiralPlace(index) {
    const radius = kStep * Math.sqrt(index), angle = index * kGoldenAngle;
    return { x: radius * Math.sin(angle), z: radius * Math.cos(angle), yawDegrees: (angle * 180) / Math.PI };
}

/**
 * The radius, in meters, of the sphere around the spiral's first `count` copies (at most
 * kFramedCopies): what the orbit frames.
 * @param {number} count
 */
export function framedRadius(count) {
    return kStep * Math.sqrt(Math.min(count, kFramedCopies)) + 0.5;
}

/**
 * Turns `count` matrices `degrees` about world up through their own positions, +Z toward +X: the
 * matrices' layout as a query chunk hands it (QueryChunk.floats), element `k` of matrix `i` at
 * `values[offset + i * stride + k]`. A matrix is column-major: its first three columns are the
 * copy's axes and turn; the fourth, its position, stays.
 * @param {Float32Array} values @param {number} offset @param {number} stride @param {number} count @param {number} degrees
 */
export function turnAboutUp(values, offset, stride, count, degrees) {
    const c = Math.cos((degrees * Math.PI) / 180), s = Math.sin((degrees * Math.PI) / 180);
    for (let i = 0; i < count; ++i) {
        for (let column = offset + i * stride; column < offset + i * stride + 12; column += 4) {
            const x = values[column], z = values[column + 2];
            values[column] = c * x + s * z;
            values[column + 2] = c * z - s * x;
        }
    }
}
