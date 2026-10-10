// The browser services the engine's frame loop, resize tracking and build choice use,
// behind one interface so the facade runs under node against a stubbed ABI.

export interface Host {
    requestAnimationFrame(callback: (timeMs: number) => void): number;
    cancelAnimationFrame(handle: number): void;
    setTimeout(callback: () => void, delayMs: number): number;
    clearTimeout(handle: number): void;
    nowMs(): number;
    isHidden(): boolean;
    /** Calls `callback` on every visibility change; returns the unsubscribe. */
    onVisibilityChange(callback: () => void): () => void;
    /** Calls `callback` whenever `element`'s size changes; returns the unsubscribe. */
    observeResize(element: HTMLElement, callback: () => void): () => void;
    devicePixelRatio(): number;
    crossOriginIsolated(): boolean;
    hasWebGPU(): boolean;
    /** `url` resolved against the page's base URL. */
    resolveUrl(url: string): string;
    log(message: string): void;
    /** Reports an error thrown by page code the engine called, without stopping the engine. */
    reportError(error: unknown): void;
}

export function browserHost(): Host {
    return {
        requestAnimationFrame: (callback) => requestAnimationFrame(callback),
        cancelAnimationFrame: (handle) => cancelAnimationFrame(handle),
        setTimeout: (callback, delayMs) => window.setTimeout(callback, delayMs),
        clearTimeout: (handle) => window.clearTimeout(handle),
        nowMs: () => performance.now(),
        isHidden: () => document.visibilityState === 'hidden',
        onVisibilityChange(callback) {
            document.addEventListener('visibilitychange', callback);
            return () => document.removeEventListener('visibilitychange', callback);
        },
        observeResize(element, callback) {
            const observer = new ResizeObserver(() => callback());
            observer.observe(element);
            return () => observer.disconnect();
        },
        devicePixelRatio: () => window.devicePixelRatio || 1,
        crossOriginIsolated: () => globalThis.crossOriginIsolated === true,
        hasWebGPU: () => 'gpu' in navigator && navigator.gpu !== undefined,
        resolveUrl: (url) => new URL(url, document.baseURI).href,
        log: (message) => console.info(message),
        reportError: (error) => globalThis.reportError(error),
    };
}
