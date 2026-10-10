// Minimal IPC client for the editor debug server. Port-parameterized so a
// bench editor (GE_EDITOR_DEBUG_PORT) can coexist with a dev editor on 9999.
// Returns parsed JSON result. Always closes the socket and never hangs.

import net from 'node:net';

const HOST = '127.0.0.1';
const DEFAULT_TIMEOUT_MS = 30000;

let nextId = 1;

export function makeIpc(port) {
    return function ipc(method, params = {}, opts = {}) {
        const timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        const id = String(nextId++);
        return new Promise((resolve, reject) => {
            const c = net.createConnection({ host: HOST, port });
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
    };
}

export function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
