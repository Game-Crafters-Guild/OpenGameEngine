import type { OpenEngineErrorCode } from '../opengine.js';

/** Every error the library throws or rejects with. The message states the fix. */
export class OpenEngineError extends Error {
    readonly code: OpenEngineErrorCode;

    constructor(code: OpenEngineErrorCode, message: string) {
        super(message);
        this.name = 'OpenEngineError';
        this.code = code;
    }
}
