// Download progress for Engine.create's and scene.load's onProgress: counts each response's
// bytes as its body is read and reports a phase's files together (the rule for which bytes is
// LoadProgress's, in opengine.d.ts).

import type { LoadPhase, LoadProgress } from '../opengine.js';
import { OpenEngineError } from './errors.js';

/** A file of a phase: its response, and its decompressed size when the page knows it beforehand. */
export interface TrackedFile {
    response: Response;
    decodedBytes?: number;
}

/** Wraps a phase's responses so reading them reports the phase's progress; returns the wrapped responses. */
export type TrackDownload = (phase: LoadPhase, files: TrackedFile[]) => Response[];

/**
 * A tracker that reports to `onProgress`, or one that hands the responses back unwrapped when
 * there is no callback.
 */
export function downloadTracker(onProgress: ((progress: LoadProgress) => void) | undefined): TrackDownload {
    if (!onProgress) return (_phase, files) => files.map((file) => file.response);
    return (phase, files) => trackDownload(phase, files, onProgress);
}

/**
 * The file name the engine picks a model's loader by, as its module reads it from a URL: the
 * last path segment, or a blob: URL's fragment; null when it has no extension.
 */
function fileNameOf(url: string): string | null {
    const name = url.startsWith('blob:') ? url.slice(url.indexOf('#') + 1 || url.length) : new URL(url).pathname.split('/').pop() ?? '';
    return /\.[^.]+$/.test(name) ? name : null;
}

/**
 * Downloads the model at `url`, reporting it in the 'model' phase, and resolves to a blob: URL
 * of its bytes that carries its file name after '#', which the engine loads in its place. Null
 * when the URL names no file the engine can pick a loader by: the engine's own fetch reports
 * that. A failed request rejects as the engine's own fetch would.
 */
export async function downloadModel(url: string, onProgress: (progress: LoadProgress) => void): Promise<string | null> {
    const name = fileNameOf(url);
    if (!name) return null;
    const failure = (why: string) => new OpenEngineError('Engine', `scene.load('${url}') failed: could not fetch '${url}': ${why}`);
    let response: Response;
    try {
        response = await fetch(url);
    } catch {
        throw failure('the request failed (a file on another origin loads only when its host sends CORS headers).');
    }
    if (!response.ok) throw failure(`HTTP ${response.status}.`);
    const [counted] = trackDownload('model', [{ response }], onProgress);
    let bytes: Blob;
    try {
        bytes = await counted!.blob();
    } catch {
        throw failure('the download broke off; load it again.');
    }
    return `${URL.createObjectURL(bytes)}#${name}`;
}

/** How one file of a phase counts: its bytes so far, its total, and whether that total is known. */
interface FileCount {
    /** Decompressed bytes read so far. */
    decoded: number;
    /** The decompressed size, when known beforehand. */
    decodedBytes: number | undefined;
    /** Content-Length, or 0 without one. */
    length: number;
    /** The response says it is compressed (Content-Encoding other than identity). */
    encoded: boolean;
    /** The body read past Content-Length: compressed, though the header was not visible. */
    overran: boolean;
    done: boolean;
}

/**
 * The file's progress, or null while its total is unknown. A body reads decompressed: with
 * Content-Length and the decompressed size both known the bytes read are scaled to the network's;
 * with Content-Length alone they count as they are while the body is not compressed; without
 * Content-Length they count against the decompressed size. A compressed file of unknown
 * decompressed size has no total until it has arrived.
 */
function progressOf(count: FileCount): { loaded: number; total: number } | null {
    const { decoded, decodedBytes, length } = count;
    if (count.done) return { loaded: length && decodedBytes ? length : decoded, total: length && decodedBytes ? length : decoded };
    if (length && decodedBytes) return { loaded: Math.min(length, Math.round((decoded * length) / decodedBytes)), total: length };
    if (length && !count.encoded && !count.overran) return { loaded: decoded, total: length };
    if (!length && decodedBytes) return { loaded: Math.min(decodedBytes, decoded), total: decodedBytes };
    return null;
}

/**
 * Reports a phase's files together: `loaded` and `total` add up, and the phase has no total
 * (0) while one of its files has none.
 */
function trackDownload(phase: LoadPhase, files: TrackedFile[], onProgress: (progress: LoadProgress) => void): Response[] {
    const counts: FileCount[] = files.map(({ response, decodedBytes }) => {
        const length = Number(response.headers.get('Content-Length'));
        const encoding = response.headers.get('Content-Encoding');
        return { decoded: 0, decodedBytes, length: length > 0 ? length : 0, encoded: !!encoding && encoding !== 'identity', overran: false, done: false };
    });
    let reported = -1;
    const report = () => {
        const each = counts.map((count) => progressOf(count) ?? { loaded: count.decoded, total: 0 });
        const loaded = each.reduce((sum, file) => sum + file.loaded, 0);
        const known = each.every((file) => file.total > 0);
        const done = counts.every((count) => count.done);
        if (loaded <= reported && !done && reported >= 0) return;
        reported = loaded;
        onProgress({ phase, loaded, total: known || done ? each.reduce((sum, file) => sum + file.total, 0) : 0 });
    };
    const wrapped = files.map(({ response }, index) => {
        const count = counts[index]!;
        if (!response.body) {
            count.done = true;
            return response;
        }
        const counter = new TransformStream<Uint8Array, Uint8Array>({
            transform(chunk, controller) {
                count.decoded += chunk.byteLength;
                if (count.length && count.decoded > count.length) count.overran = true;
                report();
                controller.enqueue(chunk);
            },
            flush() {
                count.done = true;
                if (counts.every((each) => each.done)) report();
            },
        });
        return new Response(response.body.pipeThrough(counter), { status: response.status, statusText: response.statusText, headers: response.headers });
    });
    report();
    return wrapped;
}
