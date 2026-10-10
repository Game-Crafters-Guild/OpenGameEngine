// The ASYNCIFY call contract (design section 3.1): while ge_create is suspended no other
// export may run, and nothing runs after dispose. Every ABI call the
// facade makes goes through the guarded Abi this file builds, so a violation is a thrown
// error naming the rule instead of a reentry into an unwound wasm stack.

import type { Abi } from './abi.js';
import { OpenEngineError } from './errors.js';

type SuspendingCall = 'ge_create' | 'ge_shutdown';

export class CallContract {
    private m_Suspended: SuspendingCall | null = null;
    private m_Disposed = false;

    /** The suspending call in progress, or null. */
    get suspended(): SuspendingCall | null {
        return this.m_Suspended;
    }

    get disposed(): boolean {
        return this.m_Disposed;
    }

    markDisposed(): void {
        this.m_Disposed = true;
    }

    /** Runs a suspending call; no synchronous call is allowed until it settles. */
    async suspend<T>(call: SuspendingCall, run: () => Promise<T>): Promise<T> {
        this.check(call);
        this.m_Suspended = call;
        try {
            return await run();
        } finally {
            this.m_Suspended = null;
        }
    }

    /** Throws when the engine was disposed. */
    checkAlive(call: string): void {
        if (this.m_Disposed) throw this.disposedError(call);
    }

    disposedError(call: string): OpenEngineError {
        return new OpenEngineError('Disposed', `${call} was called after engine.dispose(). Create a new engine with Engine.create.`);
    }

    /** Throws when `call` may not run now. */
    check(call: string): void {
        this.checkAlive(call);
        if (this.m_Suspended === null) return;
        throw new OpenEngineError('CallOrder',
            `${call} was called while ${this.m_Suspended} is suspended: no engine call may run until Engine.create resolves; await it first.`);
    }
}

const kSuspendingCalls = new Set<string>(['ge_create', 'ge_shutdown']);

/** Wraps every synchronous `ge_` export of `abi` with the contract's check. */
export function guardAbi(abi: Abi, contract: CallContract): Abi {
    const wrapped = new Map<string, unknown>();
    return new Proxy(abi, {
        get(target, key) {
            const value: unknown = Reflect.get(target, key);
            if (typeof key !== 'string' || typeof value !== 'function') return value;
            if (!key.startsWith('ge_') || kSuspendingCalls.has(key)) return value.bind(target);
            let guarded = wrapped.get(key);
            if (guarded === undefined) {
                guarded = (...args: unknown[]) => {
                    contract.check(key);
                    return (value as (...a: unknown[]) => unknown).apply(target, args);
                };
                wrapped.set(key, guarded);
            }
            return guarded;
        },
    });
}
