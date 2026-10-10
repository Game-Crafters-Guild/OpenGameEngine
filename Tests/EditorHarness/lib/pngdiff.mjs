// Pure-Node PNG decoder + simple delta metrics.
// Decodes 8-bit PNG (RGB, RGBA, grayscale, grayscale+alpha) using zlib.
// Sufficient for screenshots produced by the editor (RGBA8 PNG).

import fs from 'node:fs';
import zlib from 'node:zlib';
import crypto from 'node:crypto';

const PNG_SIG = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);

function paeth(a, b, c) {
    const p = a + b - c;
    const pa = Math.abs(p - a);
    const pb = Math.abs(p - b);
    const pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

export function decodePNG(buf) {
    if (!PNG_SIG.equals(buf.subarray(0, 8))) throw new Error('Not a PNG');
    let off = 8;
    let width = 0, height = 0, bitDepth = 0, colorType = 0;
    let idat = Buffer.alloc(0);
    while (off < buf.length) {
        const len = buf.readUInt32BE(off); off += 4;
        const type = buf.subarray(off, off + 4).toString('ascii'); off += 4;
        const data = buf.subarray(off, off + len); off += len;
        off += 4; // CRC
        if (type === 'IHDR') {
            width = data.readUInt32BE(0);
            height = data.readUInt32BE(4);
            bitDepth = data[8];
            colorType = data[9];
        } else if (type === 'IDAT') {
            idat = Buffer.concat([idat, data]);
        } else if (type === 'IEND') {
            break;
        }
    }
    if (bitDepth !== 8) throw new Error(`Unsupported bit depth ${bitDepth}`);
    const channels = colorType === 0 ? 1 : colorType === 2 ? 3 : colorType === 3 ? 1 : colorType === 4 ? 2 : colorType === 6 ? 4 : 0;
    if (!channels || colorType === 3) throw new Error(`Unsupported color type ${colorType}`);
    const bpp = channels;
    const stride = width * bpp;
    const raw = zlib.inflateSync(idat);
    const out = Buffer.alloc(width * height * bpp);
    let prevRow = Buffer.alloc(stride);
    let inPos = 0;
    for (let y = 0; y < height; y++) {
        const filter = raw[inPos++];
        const row = Buffer.from(raw.subarray(inPos, inPos + stride));
        inPos += stride;
        // Apply filter to row in-place; produce reconstructed row.
        for (let x = 0; x < stride; x++) {
            const left = x >= bpp ? row[x - bpp] : 0;
            const up = prevRow[x];
            const upleft = x >= bpp ? prevRow[x - bpp] : 0;
            let val = row[x];
            switch (filter) {
                case 0: break;
                case 1: val = (val + left) & 0xff; break;
                case 2: val = (val + up) & 0xff; break;
                case 3: val = (val + ((left + up) >> 1)) & 0xff; break;
                case 4: val = (val + paeth(left, up, upleft)) & 0xff; break;
                default: throw new Error(`Unknown PNG filter ${filter}`);
            }
            row[x] = val;
        }
        row.copy(out, y * stride);
        prevRow = row;
    }
    return { width, height, channels, pixels: out };
}

// Compute simple delta metrics between two decoded images.
// Returns { meanRGB, meanRGBA, maxChannelDiff, diffPixelFrac, sameSize }.
// Uses absolute per-channel diff, scaled 0..255.
export function diffImages(a, b) {
    const sameSize = a.width === b.width && a.height === b.height && a.channels === b.channels;
    if (!sameSize) {
        return { sameSize: false, meanRGB: null, meanRGBA: null, maxChannelDiff: null, diffPixelFrac: null };
    }
    const ch = a.channels;
    const px = a.width * a.height;
    let sumRGB = 0, sumRGBA = 0;
    let maxDiff = 0;
    let diffPx = 0;
    const A = a.pixels, B = b.pixels;
    for (let i = 0; i < px; i++) {
        const off = i * ch;
        let pixDiff = 0;
        const channelsToSum = Math.min(3, ch);
        for (let c = 0; c < channelsToSum; c++) {
            const d = Math.abs(A[off + c] - B[off + c]);
            sumRGB += d;
            pixDiff += d;
            if (d > maxDiff) maxDiff = d;
        }
        sumRGBA += pixDiff;
        if (ch === 4) {
            const da = Math.abs(A[off + 3] - B[off + 3]);
            sumRGBA += da;
            if (da > maxDiff) maxDiff = da;
        }
        if (pixDiff > 8) diffPx++; // pixel "changed" if any RGB channel differs by more than ~3%
    }
    return {
        sameSize: true,
        width: a.width,
        height: a.height,
        meanRGB: sumRGB / (px * 3),
        meanRGBA: sumRGBA / (px * ch),
        maxChannelDiff: maxDiff,
        diffPixelFrac: diffPx / px,
    };
}

export function fileSha256(p) {
    const h = crypto.createHash('sha256');
    h.update(fs.readFileSync(p));
    return h.digest('hex').slice(0, 16);
}

export function decodeFile(p) {
    return decodePNG(fs.readFileSync(p));
}
