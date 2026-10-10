// Minimal IPC client for the editor debug server on 127.0.0.1.
// Returns parsed JSON result. Always closes the socket and never hangs.
//
// Port defaults to 9999 and follows GE_EDITOR_DEBUG_PORT, the same variable the
// editor reads to pick its listen port — point both at the same value to run
// against an isolated editor instead of the user's session on 9999.

import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';

const HOST = '127.0.0.1';
export const PORT = Number(process.env.GE_EDITOR_DEBUG_PORT) || 9999;
const DEFAULT_TIMEOUT_MS = 30000;

let nextId = 1;

export function ipc(method, params = {}, opts = {}) {
    const timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const id = String(nextId++);
    return new Promise((resolve, reject) => {
        const c = net.createConnection({ host: HOST, port: PORT });
        let buf = '';
        let done = false;
        const finish = (err, val) => {
            if (done) return;
            done = true;
            try { c.destroy(); } catch {}
            clearTimeout(timer);
            if (err) reject(err); else resolve(val);
        };
        const timer = setTimeout(() => finish(new Error(`IPC timeout ${method}`)), timeoutMs);
        c.on('connect', () => {
            c.write(JSON.stringify({ id, method, params }) + '\n');
        });
        c.on('data', (d) => {
            buf += d.toString();
            const lines = buf.split('\n');
            buf = lines.pop();
            for (const line of lines) {
                if (!line.trim()) continue;
                let resp;
                try { resp = JSON.parse(line); } catch (e) { return finish(new Error(`Bad JSON from server: ${line}`)); }
                if (resp.ok === false || resp.error) {
                    return finish(null, { error: resp.error || resp.result?.error || 'unknown error', raw: resp });
                }
                return finish(null, resp.result ?? resp);
            }
        });
        c.on('error', (e) => finish(new Error(`IPC connection error: ${e.code || e.message}`)));
        c.on('close', () => finish(new Error('IPC closed without response')));
    });
}

// Save the base64 png from a screenshot response to a path on disk.
// Returns { width, height, savedTo, sha256, bytes }.
export function saveScreenshot(resp, outPath) {
    if (!resp || !resp.pngBase64) throw new Error('No pngBase64 in response: ' + JSON.stringify(resp));
    const buf = Buffer.from(resp.pngBase64, 'base64');
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
    fs.writeFileSync(outPath, buf);
    const crypto = require('node:crypto');
    return {
        width: resp.width,
        height: resp.height,
        savedTo: outPath,
        bytes: buf.length,
    };
}

export async function takeScreenshot(target, opts = {}) {
    const params = { target, ...opts };
    return ipc('take_screenshot', params, { timeoutMs: 30000 });
}

export function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
