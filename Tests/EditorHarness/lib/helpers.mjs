// Helpers shared across scenarios.

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { ipc, takeScreenshot, sleep } from './ipc.mjs';
import { decodePNG, diffImages, fileSha256 } from './pngdiff.mjs';

// Resolve repo root relative to this file: Tests/EditorHarness/lib/helpers.mjs → ../../.. → repo root.
const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = path.resolve(HERE, '..', '..', '..');

// Editor reads assets from <Editor.exe>/Assets/. Auto-detect the active build dir
// by preference (non-unity daily-driver first, then unity, then any other
// existing build/* dir with an Editor.exe). Override via GE_EDITOR_ASSETS_ROOT.
function detectEditorAssetsRoot() {
    if (process.env.GE_EDITOR_ASSETS_ROOT) return process.env.GE_EDITOR_ASSETS_ROOT;
    const config = process.env.GE_CONFIG ?? 'DebugFast';
    const presets = [
        process.env.GE_PRESET,
        'vs2026-x64-local',
        'vs2026-x64-local-unity',
        'vs2026-arm64-local',
        'vs2022-x64-local',
        'vs2022-x64-local-unity',
    ].filter(Boolean);
    for (const preset of presets) {
        const candidate = path.join(REPO_ROOT, 'build', preset, 'bin', config, 'Apps', 'Editor', 'Assets');
        if (fs.existsSync(candidate)) return candidate;
    }
    // Last-resort fallback — return the preferred path even if it doesn't exist
    // yet so error messages point at a sensible location.
    return path.join(REPO_ROOT, 'build', 'vs2026-x64-local', 'bin', config, 'Apps', 'Editor', 'Assets');
}

export const EDITOR_ASSETS_ROOT = detectEditorAssetsRoot();
export const RESULTS_DIR = path.join(REPO_ROOT, 'Tests', 'EditorHarness', 'results');
export const BACKUPS_DIR = path.join(REPO_ROOT, 'Tests', 'EditorHarness', 'backups');

export function ensureDirs() {
    fs.mkdirSync(RESULTS_DIR, { recursive: true });
    fs.mkdirSync(BACKUPS_DIR, { recursive: true });
}

export function backupFile(absPath) {
    if (!fs.existsSync(absPath)) throw new Error('Backup source missing: ' + absPath);
    const id = crypto.createHash('sha1').update(absPath).digest('hex').slice(0, 10);
    const dest = path.join(BACKUPS_DIR, id + '_' + path.basename(absPath));
    fs.copyFileSync(absPath, dest);
    return { backup: dest, original: absPath };
}

// Idempotent: a scenario restores its own edits on the way out and the exit
// handler restores everything it tracked, so this runs twice for the same
// backup. Consuming the copy is what lets an empty backups/ mean "nothing was
// left half-edited".
export function restoreBackup(b) {
    if (!fs.existsSync(b.backup)) return;
    fs.copyFileSync(b.backup, b.original);
    fs.rmSync(b.backup);
}

export async function shootAndDecode(scenarioName, label, opts = {}) {
    const target = opts.target || 'window';
    const params = { target };
    if (opts.panelId) params.panelId = opts.panelId;
    if (opts.elementId) params.elementId = opts.elementId;
    if (opts.rect) Object.assign(params, opts.rect);
    if (opts.coords) params.coords = opts.coords;

    const r = await ipc('take_screenshot', params, { timeoutMs: 30000 });
    if (!r || r.error || !r.pngBase64) {
        throw new Error('Screenshot failed for ' + label + ': ' + JSON.stringify(r).slice(0, 300));
    }
    const buf = Buffer.from(r.pngBase64, 'base64');
    const outPath = path.join(RESULTS_DIR, scenarioName + '_' + label + '.png');
    fs.writeFileSync(outPath, buf);
    let decoded = null;
    try { decoded = decodePNG(buf); }
    catch (e) {
        // If we cannot decode (rare), still keep the file.
        return { savedTo: outPath, width: r.width, height: r.height, sha: fileSha256(outPath), decoded: null };
    }
    return {
        savedTo: outPath,
        width: r.width,
        height: r.height,
        sha: crypto.createHash('sha256').update(buf).digest('hex').slice(0, 16),
        decoded,
    };
}

export async function diffScreenshots(scenarioName, before, after) {
    if (!before.decoded || !after.decoded) return null;
    return diffImages(before.decoded, after.decoded);
}

export function logStep(label, msg) {
    console.log(`  [${label}] ${msg}`);
}

export async function pingEditor() {
    return ipc('get_editor_state', {}, { timeoutMs: 5000 });
}

export async function waitForReload(ms = 2500) {
    // Editor watches files with a debounce — wait for the reload to settle.
    await sleep(ms);
}

export { ipc, takeScreenshot, sleep };

// Bring the editor window to the foreground. Screenshots need a rendered frame,
// and a minimized or hidden window produces none — take_screenshot then fails
// naming that rather than returning a picture of the window. Uses the
// AttachThreadInput foreground trick.
export function ensureEditorFocused() {
    const ps = `
$sig = '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);' +
       '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);' +
       '[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();' +
       '[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);' +
       '[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);' +
       '[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();'
try { Add-Type -MemberDefinition $sig -Name W -Namespace ITF -ErrorAction Stop } catch {}
$p = Get-Process Editor -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($p) {
  $fg = [ITF.W]::GetForegroundWindow()
  $ft = [ITF.W]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
  $mt = [ITF.W]::GetCurrentThreadId()
  [ITF.W]::AttachThreadInput($mt, $ft, $true) | Out-Null
  [ITF.W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
  [ITF.W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
  [ITF.W]::AttachThreadInput($mt, $ft, $false) | Out-Null
}`;
    try {
        const cp = process.getBuiltinModule('node:child_process');
        const os = process.getBuiltinModule('node:os');
        const scriptPath = path.join(os.tmpdir(), 'ge-it-focus-editor.ps1');
        fs.writeFileSync(scriptPath, ps);
        const pshell = path.join(process.env.SystemRoot || 'C:\\Windows',
                                 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
        cp.execFileSync(pshell, ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', scriptPath],
                        { timeout: 15000 });
    }
    catch { /* focus is best-effort; the screenshot retry loop reports failures */ }
}

// Average RGB of a fractional region of a decoded screenshot.
export function avgRegion(decoded, x0f, y0f, x1f, y1f) {
    const { width: w, height: h, channels, pixels } = decoded;
    const x0 = Math.floor(w * x0f), x1 = Math.floor(w * x1f);
    const y0 = Math.floor(h * y0f), y1 = Math.floor(h * y1f);
    let r = 0, g = 0, b = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const o = (y * w + x) * channels;
            r += pixels[o]; g += pixels[o + 1]; b += pixels[o + 2]; n++;
        }
    return { r: r / n, g: g / n, b: b / n };
}
