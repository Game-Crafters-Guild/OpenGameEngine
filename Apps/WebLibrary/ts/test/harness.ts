// The browser services an engine needs, faked for node: animation frames and timers the
// test fires by hand, a visibility flag, a resize observer and the build-choice inputs.

import type { EngineOptions } from '../opengine.js';
import { createEngine, type EngineImpl, type ReleaseStamp } from '../src/engine.js';
import type { Host } from '../src/host.js';
import { StubAbi } from './stub-abi.js';

export class FakeHost implements Host {
    hidden = false;
    isolated = false;
    webGpu = true;
    dpr = 2;
    readonly logs: string[] = [];
    readonly frames = new Map<number, (timeMs: number) => void>();
    readonly timers = new Map<number, { callback: () => void; delayMs: number }>();
    private m_NextHandle = 1;
    private m_NowMs = 0;
    private readonly m_VisibilityListeners = new Set<() => void>();
    private readonly m_ResizeListeners = new Set<() => void>();

    requestAnimationFrame(callback: (timeMs: number) => void): number {
        const handle = this.m_NextHandle++;
        this.frames.set(handle, callback);
        return handle;
    }

    cancelAnimationFrame(handle: number): void { this.frames.delete(handle); }

    setTimeout(callback: () => void, delayMs: number): number {
        const handle = this.m_NextHandle++;
        this.timers.set(handle, { callback, delayMs });
        return handle;
    }

    clearTimeout(handle: number): void { this.timers.delete(handle); }
    nowMs(): number { return this.m_NowMs; }
    isHidden(): boolean { return this.hidden; }

    onVisibilityChange(callback: () => void): () => void {
        this.m_VisibilityListeners.add(callback);
        return () => this.m_VisibilityListeners.delete(callback);
    }

    observeResize(_element: HTMLElement, callback: () => void): () => void {
        this.m_ResizeListeners.add(callback);
        return () => this.m_ResizeListeners.delete(callback);
    }

    devicePixelRatio(): number { return this.dpr; }
    crossOriginIsolated(): boolean { return this.isolated; }
    hasWebGPU(): boolean { return this.webGpu; }
    resolveUrl(url: string): string { return new URL(url, 'https://page.test/demo/').href; }
    log(message: string): void { this.logs.push(message); }
    readonly errors: unknown[] = [];
    reportError(error: unknown): void { this.errors.push(error); }

    /** Fires every pending animation frame at `timeMs`. */
    fireFrames(timeMs: number): void {
        this.m_NowMs = timeMs;
        const pending = [...this.frames.values()];
        this.frames.clear();
        for (const callback of pending) callback(timeMs);
    }

    /** Fires every pending timer, advancing the clock by its delay. */
    fireTimers(): void {
        const pending = [...this.timers.values()];
        this.timers.clear();
        for (const { callback, delayMs } of pending) {
            this.m_NowMs += delayMs;
            callback();
        }
    }

    setHidden(hidden: boolean): void {
        this.hidden = hidden;
        for (const listener of this.m_VisibilityListeners) listener();
    }

    resizeCanvas(): void {
        for (const listener of this.m_ResizeListeners) listener();
    }
}

export function fakeCanvas(): HTMLCanvasElement {
    return { id: '', clientWidth: 800, clientHeight: 600 } as unknown as HTMLCanvasElement;
}

/** A started engine over a fresh stub. */
export async function startEngine(options: Partial<EngineOptions> = {}, stamp: ReleaseStamp = { version: '0.1.0', fingerprint: null }) {
    const abi = new StubAbi();
    const host = new FakeHost();
    const canvas = options.canvas ?? fakeCanvas();
    const engine: EngineImpl = await createEngine({ ...options, canvas }, host, async () => abi, stamp);
    return { abi, host, engine, canvas };
}

/** Loads `url` to completion, firing frames until the engine reports it done. */
export async function loadModel(engine: EngineImpl, host: FakeHost, url: string) {
    const loading = engine.scene.load(url);
    let done = false;
    loading.then(() => { done = true; }, () => { done = true; });
    for (let frame = 1; !done && frame < 100; ++frame) {
        await settle();
        host.fireFrames(frame * 16);
    }
    return within(loading, `scene.load('${url}')`);
}

/** `promise`, or a rejection naming `what` when it has not settled within `ms`. */
export function within<T>(promise: Promise<T>, what: string, ms = 500): Promise<T> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${what} never settled within ${ms} ms`)), ms);
    });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

/** Lets every queued promise continuation run. */
export function settle(): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, 0));
}
