// Engine.create and the running engine: the build choice, the module's bring-up under the
// call contract, the release check, the default world, the frame loop with the hidden-tab
// rule, resize tracking and disposal.

import type { Engine as EngineApi, EngineOptions, EngineStatic, ThreadsOption } from '../opengine.js';
import type { Abi, CoreBindingModule, CoreBuild } from './abi.js';
import { Bridge } from './bridge.js';
import { kCoreWasmBytes, kPackageVersion, kReflectionFingerprint } from './build-stamp.js';
import { bindComponentTokens, Camera, Light, SkyEnvironment } from './components.js';
import { OpenEngineError } from './errors.js';
import { browserHost, type Host } from './host.js';
import { downloadTracker, type TrackDownload } from './progress.js';
import { checkRelease } from './reflection.js';
import { SceneImpl, type SceneHost } from './scene.js';
import { SkyViewImpl } from './sky-view.js';
import { SunViewImpl } from './sun-view.js';

/** The tick interval while the tab is hidden: rAF stops there, and the engine keeps running slowly. */
const kHiddenTickMs = 33;

/** A clear noon: kClearNoonSunIlluminanceLux (LightPhotometry.h). */
const kDefaultSunLux = 100_000;

export interface ReleaseStamp {
    version: string;
    fingerprint: string | null;
}

export type CoreLoader = (build: CoreBuild, canvas: HTMLCanvasElement, track: TrackDownload) => Promise<Abi>;

/** Which build `threads` selects, and the line that says why. */
export function selectBuild(threads: ThreadsOption, crossOriginIsolated: boolean): { build: CoreBuild; reason: string } {
    if (threads === 'single') return { build: 'st', reason: "threads: 'single' was requested" };
    if (threads === 'multi') {
        if (!crossOriginIsolated) {
            throw new OpenEngineError('InvalidArgument',
                "threads: 'multi' needs a cross-origin isolated page (COOP same-origin and COEP require-corp headers, " +
                "or coi-serviceworker.js served from a folder that holds the page and the engine module); use threads: 'auto' to fall back to the single-threaded build.");
        }
        return { build: 'mt', reason: "threads: 'multi' was requested" };
    }
    return crossOriginIsolated
        ? { build: 'mt', reason: 'the page is cross-origin isolated' }
        : { build: 'st', reason: 'the page is not cross-origin isolated (no COOP/COEP headers or service worker)' };
}

let g_CanvasCount = 0;

function canvasSelector(canvas: HTMLCanvasElement): string {
    if (!canvas.id) canvas.id = `opengine-canvas-${++g_CanvasCount}`;
    return `#${canvas.id}`;
}

export class EngineImpl implements EngineApi, SceneHost {
    readonly bridge: Bridge;
    readonly scene: SceneImpl;
    private readonly m_Host: Host;
    private readonly m_Canvas: HTMLCanvasElement;
    private readonly m_FrameCallbacks = new Set<(dt: number) => void>();
    private m_Running = false;
    private m_LastFrameMs: number | null = null;
    private m_PendingFrame: { kind: 'raf' | 'timer'; handle: number } | null = null;
    /** A load poll scheduled on its own because the engine is not running. */
    private m_PendingPoll: { kind: 'raf' | 'timer'; handle: number } | null = null;
    private readonly m_Unsubscribe: Array<() => void> = [];

    constructor(bridge: Bridge, host: Host, canvas: HTMLCanvasElement) {
        this.bridge = bridge;
        this.m_Host = host;
        this.m_Canvas = canvas;
        this.scene = new SceneImpl(this);
    }

    resolveUrl(url: string): string {
        return this.m_Host.resolveUrl(url);
    }

    /** Creates the camera, the sun and the sky the empty world starts with. */
    createDefaultWorld(): void {
        const camera = this.scene.create('Camera');
        camera.set(Camera, {});
        camera.transform.position.set(0, 1.5, -5);
        const sun = this.scene.create('Sun');
        sun.set(Light, { type: 'Directional', intensityUnit: 'Lux', intensity: kDefaultSunLux, castsShadows: true });
        sun.transform.rotation.set(50, -30, 0);
        const sky = this.scene.create('Sky');
        sky.set(SkyEnvironment, { sunLight: sun });
        this.scene.setDefaults({ camera, sun: new SunViewImpl(sun), sky: new SkyViewImpl(sky) });
    }

    /** Starts tracking the canvas size and the tab's visibility. */
    attach(): void {
        this.m_Unsubscribe.push(this.m_Host.observeResize(this.m_Canvas, () => this.resize()));
        this.m_Unsubscribe.push(this.m_Host.onVisibilityChange(() => this.reschedule()));
        this.resize();
    }

    run(): void {
        this.bridge.contract.checkAlive('engine.run');
        if (this.m_Running) return;
        this.m_Running = true;
        this.m_LastFrameMs = null;
        this.schedule();
    }

    pause(): void {
        if (!this.m_Running) return;
        this.m_Running = false;
        this.cancelPendingFrame();
        this.requestLoadPoll();
    }

    onFrame(callback: (dt: number) => void): () => void {
        this.m_FrameCallbacks.add(callback);
        return () => this.m_FrameCallbacks.delete(callback);
    }

    resize(): void {
        const code = this.bridge.abi.ge_resize(this.m_Canvas.clientWidth, this.m_Canvas.clientHeight, this.m_Host.devicePixelRatio());
        this.bridge.check(code, 'engine.resize');
    }

    async dispose(): Promise<void> {
        if (this.bridge.contract.disposed) return;
        this.pause();
        this.cancel(this.m_PendingPoll);
        this.m_PendingPoll = null;
        for (const unsubscribe of this.m_Unsubscribe.splice(0)) unsubscribe();
        // Every call throws Disposed from here: none can reach the module while ge_shutdown is
        // suspended.
        this.bridge.contract.markDisposed();
        this.scene.abandonLoads();
        await this.bridge.shutdown();
    }

    requestLoadPoll(): void {
        // A running engine polls after every tick, which finished the loads; an idle one
        // updates the asset manager and polls on a frame of its own (the 33 ms timer while hidden).
        if (this.m_Running || this.m_PendingPoll || this.bridge.contract.disposed || !this.scene.loading) return;
        this.m_PendingPoll = this.scheduleCallback(() => this.idlePoll());
    }

    private idlePoll(): void {
        this.m_PendingPoll = null;
        if (this.m_Running || this.bridge.contract.disposed) return;
        this.bridge.check(this.bridge.abi.ge_update_assets(), 'scene.load');
        this.scene.pollLoads();
    }

    /**
     * One frame: the page's callbacks, then the engine's tick, then the running load's poll.
     * A callback that throws is reported and the frame goes on; one that pauses or disposes
     * the engine ends the frame before the tick.
     */
    frame(timeMs: number): void {
        this.m_PendingFrame = null;
        if (!this.m_Running) return;
        this.m_PendingFrame = this.scheduleCallback((t) => this.frame(t));
        const dt = this.m_LastFrameMs === null ? 0 : (timeMs - this.m_LastFrameMs) / 1000;
        this.m_LastFrameMs = timeMs;
        for (const callback of [...this.m_FrameCallbacks]) {
            try {
                callback(dt);
            } catch (error) {
                this.m_Host.reportError(error);
            }
            if (!this.m_Running) return;
        }
        this.bridge.check(this.bridge.abi.ge_tick(), 'ge_tick');
        this.scene.pollLoads();
    }

    /** Animation frames while the tab is visible, the hidden-tab timer otherwise. */
    private scheduleCallback(callback: (timeMs: number) => void): { kind: 'raf' | 'timer'; handle: number } {
        if (this.m_Host.isHidden()) {
            return { kind: 'timer', handle: this.m_Host.setTimeout(() => callback(this.m_Host.nowMs()), kHiddenTickMs) };
        }
        return { kind: 'raf', handle: this.m_Host.requestAnimationFrame(callback) };
    }

    private schedule(): void {
        this.m_PendingFrame = this.scheduleCallback((timeMs) => this.frame(timeMs));
    }

    private reschedule(): void {
        if (!this.m_Running) return;
        this.cancelPendingFrame();
        this.schedule();
    }

    private cancelPendingFrame(): void {
        this.cancel(this.m_PendingFrame);
        this.m_PendingFrame = null;
    }

    private cancel(pending: { kind: 'raf' | 'timer'; handle: number } | null): void {
        if (!pending) return;
        if (pending.kind === 'raf') this.m_Host.cancelAnimationFrame(pending.handle);
        else this.m_Host.clearTimeout(pending.handle);
    }
}

/** Engine.create with its browser services, loader and release stamp supplied. */
export async function createEngine(options: EngineOptions, host: Host, loadCore: CoreLoader, stamp: ReleaseStamp): Promise<EngineImpl> {
    if (!host.hasWebGPU()) {
        throw new OpenEngineError('NoWebGPU', 'This browser has no WebGPU. Use a current Chrome or Edge, or Safari or Firefox with WebGPU enabled.');
    }
    const { build, reason } = selectBuild(options.threads ?? 'auto', host.crossOriginIsolated());
    host.log(`OpenEngine: using the ${build === 'mt' ? 'threaded' : 'single-threaded'} build because ${reason}.`);
    const bridge = new Bridge(await loadCore(build, options.canvas, downloadTracker(options.onProgress)));
    const selector = canvasSelector(options.canvas);
    const code = await bridge.contract.suspend('ge_create', () => bridge.abi.ge_create(bridge.writeString(selector), 0));
    bridge.check(code, 'Engine.create');
    const json = bridge.loadReflection();
    checkRelease(json, bridge.reflection.engineVersion, stamp);
    bindComponentTokens(bridge.reflection);
    const engine = new EngineImpl(bridge, host, options.canvas);
    engine.createDefaultWorld();
    engine.attach();
    return engine;
}

function defaultCoreUrl(): string {
    return new URL('./', import.meta.url).href;
}

function bindingLoader(coreUrl: string): CoreLoader {
    return async (build, canvas, track) => {
        const bindingUrl = new URL('opengine-core-binding.js', coreUrl).href;
        let binding: CoreBindingModule;
        try {
            binding = (await import(bindingUrl)) as CoreBindingModule;
        } catch (error) {
            throw new OpenEngineError('Engine', `Could not load the engine module from ${bindingUrl}: ${String(error)}. ` +
                'Serve opengine-core-binding.js and opengine-core.st/mt.js and .wasm from that folder, or pass coreUrl to Engine.create.');
        }
        return binding.loadCore({ build, coreUrl, canvas, wasmBytes: kCoreWasmBytes?.[build], track });
    };
}

export const Engine: EngineStatic = {
    create(options: EngineOptions): Promise<EngineApi> {
        const coreUrl = options.coreUrl ?? defaultCoreUrl();
        return createEngine(options, browserHost(), bindingLoader(coreUrl), { version: kPackageVersion, fingerprint: kReflectionFingerprint });
    },
};
