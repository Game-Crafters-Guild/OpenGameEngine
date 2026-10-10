#!/usr/bin/env python3
"""Run a wasm build in headless Chrome and report what it actually drew.

    python3 Tools/Web/browser_gate.py --root build/wasm-debug/bin \
        --page WebPlayer.html --seconds 20 --screenshot /tmp/webplayer.png

Serves the build tree with the cross-origin-isolation headers a threaded build
needs, drives Chrome over CDP, and prints the console transcript plus a
verdict: WebGPU validation errors, and the centre-pixel colour of the captured
frame. Every transcript line is prefixed with its arrival time in seconds since
the run's first event, so the transcript can date a stall as well as show it.

The capture path is CDP Page.captureScreenshot and nothing else. Reading the
canvas back with drawImage into a 2D canvas returns all-zero pixels even for a
page that is demonstrably rendering ("Attempt to read from an uninitialized
SharedImage"), so that probe is a broken instrument, not a measurement.

No third-party packages: the CDP client below speaks enough of RFC 6455 to
issue commands and collect events.
"""

from __future__ import annotations

import argparse
import base64
import http.server
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

def FindChrome() -> str:
    """Chrome's path on this host. GE_CHROME overrides for an unusual install."""
    override = os.environ.get("GE_CHROME")
    if override:
        return override
    if sys.platform == "darwin":
        candidates = [Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")]
    elif sys.platform == "win32":
        candidates = [
            Path(os.environ.get("PROGRAMFILES", r"C:\Program Files"))
            / "Google" / "Chrome" / "Application" / "chrome.exe",
            Path(os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)"))
            / "Google" / "Chrome" / "Application" / "chrome.exe",
            Path(os.environ.get("LOCALAPPDATA", "."))
            / "Google" / "Chrome" / "Application" / "chrome.exe",
        ]
    else:
        candidates = [Path("/usr/bin/google-chrome"), Path("/usr/bin/chromium")]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    found = shutil.which("chrome") or shutil.which("google-chrome") or shutil.which("chromium")
    if found:
        return found
    raise SystemExit(
        "browser_gate: no Chrome on this host. Install Google Chrome, or set GE_CHROME "
        "to the browser's full path."
    )


# ANGLE backs WebGL, not WebGPU (which goes through Dawn), but naming a backend
# the host cannot provide makes Chrome fall back silently rather than report it.
# Each host therefore names its own.
kAngleBackend = {"darwin": "metal", "win32": "d3d11"}.get(sys.platform, "gl")


class WasmHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


class Cdp:
    """Minimal CDP client: one WebSocket, text frames, client-masked."""

    def __init__(self, url: str):
        _, _, rest = url.partition("://")
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.socket = socket.create_connection((host, int(port)))
        key = base64.b64encode(bytes(random.getrandbits(8) for _ in range(16))).decode()
        self.socket.sendall(
            (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
             f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        self._buffer = b""
        while b"\r\n\r\n" not in self._buffer:
            self._buffer += self.socket.recv(4096)
        self._buffer = self._buffer.split(b"\r\n\r\n", 1)[1]
        self._nextId = 1
        self.events: list[dict] = []

    def _Recv(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError("CDP socket closed")
            self._buffer += chunk
        out, self._buffer = self._buffer[:count], self._buffer[count:]
        return out

    def _ReadFrame(self) -> dict:
        while True:
            header = self._Recv(2)
            opcode = header[0] & 0x0F
            length = header[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._Recv(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._Recv(8))[0]
            payload = self._Recv(length)
            if opcode == 1:
                return json.loads(payload)
            if opcode == 8:
                raise RuntimeError("CDP socket closed by peer")

    def Send(self, method: str, _timeout: float | None = None, **params) -> dict:
        requestId = self._nextId
        message = json.dumps({"id": requestId, "method": method, "params": params})
        self._nextId += 1
        data = message.encode()
        header = bytearray([0x81])
        if len(data) < 126:
            header.append(0x80 | len(data))
        elif len(data) < (1 << 16):
            header.append(0x80 | 126)
            header += struct.pack(">H", len(data))
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", len(data))
        mask = bytes(random.getrandbits(8) for _ in range(4))
        header += mask
        self.socket.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        if _timeout is not None:
            self.socket.settimeout(_timeout)
        try:
            while True:
                frame = self._ReadFrame()
                if frame.get("id") == requestId:
                    return frame
                # A reply to an earlier request that timed out is dropped, not taken for this one.
                if "id" not in frame:
                    self.events.append(frame)
        finally:
            if _timeout is not None:
                self.socket.settimeout(None)

    def Pump(self, seconds: float) -> None:
        deadline = time.time() + seconds
        self.socket.settimeout(0.5)
        while time.time() < deadline:
            try:
                frame = self._ReadFrame()
            except (socket.timeout, TimeoutError):
                continue
            if "id" not in frame:
                self.events.append(frame)
        self.socket.settimeout(None)


def ConsoleText(event: dict) -> str:
    params = event.get("params", {})
    if event.get("method") == "Runtime.consoleAPICalled":
        return " ".join(str(a.get("value", a.get("description", ""))) for a in params.get("args", []))
    if event.get("method") == "Log.entryAdded":
        return params.get("entry", {}).get("text", "")
    if event.get("method") == "Runtime.exceptionThrown":
        details = params.get("exceptionDetails", {})
        frames = details.get("stackTrace", {}).get("callFrames", [])
        # exceptionDetails.text is the bare word "Uncaught"; the thrown value's
        # class and message live under .exception, and a wasm trap is only
        # distinguishable from a C++ throw or an abort() by that description.
        thrown = details.get("exception", {})
        what = thrown.get("description") or thrown.get("className") or ""
        if not what and "value" in thrown:
            what = repr(thrown["value"])
        return f"EXCEPTION {details.get('text', '')} :: {what}\n" + "\n".join(
            f"    {f.get('functionName') or '<anon>'} ({f.get('url','')}:{f.get('lineNumber')})"
            for f in frames)
    return ""


def ConsoleTime(event: dict) -> float | None:
    """CDP's own timestamp for the event, in milliseconds since the epoch."""
    params = event.get("params", {})
    if event.get("method") == "Log.entryAdded":
        return params.get("entry", {}).get("timestamp")
    return params.get("timestamp")


def StampedLine(text: str, seconds: float | None) -> str:
    """Prefix the first physical line with its arrival time; leave the rest.

    Continuation lines are the frames of a stack trace, and the classifier that
    reads them keys on their leading whitespace.
    """
    head, separator, rest = text.partition("\n")
    # Never blank-pad an unstamped line: a leading run of spaces is how a stack
    # frame is recognised, and a padded line would read as one.
    stamp = f"[{seconds:8.3f}] " if seconds is not None else "[   --.---] "
    return stamp + head + separator + rest


def CentrePixel(png: bytes) -> tuple[int, int, int] | None:
    """Decode the PNG far enough to read its centre pixel (stdlib only)."""
    import zlib

    if png[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, width, height, depth, colour = 8, 0, 0, 0, 0
    idat = b""
    while pos < len(png):
        length = struct.unpack(">I", png[pos:pos + 4])[0]
        kind = png[pos + 4:pos + 8]
        body = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if depth != 8 or colour not in (2, 6):
        return None
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    row = bytearray(stride)
    target = height // 2
    for y in range(height):
        filterType = raw[y * (stride + 1)]
        line = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        row = bytearray(line)
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upLeft = previous[x - channels] if x >= channels else 0
            if filterType == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filterType == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filterType == 3:
                row[x] = (row[x] + (left + up) // 2) & 0xFF
            elif filterType == 4:
                p = left + up - upLeft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upLeft)
                pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upLeft)
                row[x] = (row[x] + pred) & 0xFF
        if y == target:
            offset = (width // 2) * channels
            return (row[offset], row[offset + 1], row[offset + 2])
        previous = row
    return None


# Installed before the page's own scripts: records every WebAssembly memory the page makes,
# whether JavaScript constructs it (a shared memory) or a module exports it. Walking the heap
# for them afterwards (Runtime.queryObjects) does not finish on a page holding a large module.
kWasmMemoryRecorder = """
(() => {
    const memories = (globalThis.__geWasmMemories = []);
    const record = (memory) => { if (memory instanceof WebAssembly.Memory && !memories.includes(memory)) memories.push(memory); };
    const recordExports = (exports) => { for (const value of Object.values(exports ?? {})) record(value); };
    const NativeMemory = WebAssembly.Memory;
    WebAssembly.Memory = function Memory(descriptor) { const memory = new NativeMemory(descriptor); record(memory); return memory; };
    WebAssembly.Memory.prototype = NativeMemory.prototype;
    for (const name of ['instantiate', 'instantiateStreaming']) {
        const native = WebAssembly[name];
        WebAssembly[name] = async (...args) => {
            const result = await native.apply(WebAssembly, args);
            recordExports((result.instance ?? result).exports);
            return result;
        };
    }
})();
"""


def WasmMemoryBytes(cdp: Cdp) -> list[int] | None:
    """The buffer size of every WebAssembly memory the page made, largest first. A memory only
    grows, so this is the run's high-water mark."""
    sizes = cdp.Send("Runtime.evaluate", returnByValue=True, _timeout=30.0,
                     expression="(globalThis.__geWasmMemories ?? []).map((m) => m.buffer.byteLength)")
    value = sizes.get("result", {}).get("result", {}).get("value")
    return sorted(value, reverse=True) if isinstance(value, list) else None


# How much a captured frame varies along its rows, outside the model-viewer page's overlays: the
# largest luma range of any sampled row. The sky changes only from top to bottom and a failed frame
# is one colour, so a frame with a model in it is the one with detail along a row; a model in
# shadow can still be dark at the centre. The screenshot is decoded by the browser's image decoder
# in a 2D canvas (the WebGPU canvas itself cannot be read back).
kFrameDetail = """(async () => {
    const image = await createImageBitmap(await (await fetch('data:image/png;base64,PNG_BASE64')).blob());
    const canvas = new OffscreenCanvas(image.width, image.height);
    const context = canvas.getContext('2d');
    context.drawImage(image, 0, 0);
    // While the page's status stands over the canvas, the canvas has not drawn a frame.
    const overlay = document.querySelector('.canvas-status');
    if (overlay && !overlay.hidden) return 0;
    // The page's own overlays are not the frame: the panel (a right column, or a bottom strip on a
    // narrow page), the frame-rate readout, a model's credit (along the top or the bottom edge) and
    // the download bar along the top edge are cut out.
    const panel = document.querySelector('.panel')?.getBoundingClientRect();
    const besidePanel = panel && panel.left > image.width / 2;
    const right = besidePanel ? Math.floor(panel.left) : image.width;
    const shown = (element) => element && !element.hidden ? element.getBoundingClientRect() : null;
    const credit = shown(document.querySelector('.credit'));
    const creditOnTop = credit && credit.top < image.height / 2;
    const bottom = Math.min(panel && !besidePanel ? Math.floor(panel.top) : image.height,
        credit && !creditOnTop ? Math.floor(credit.top) : image.height);
    const below = (box) => box ? Math.ceil(box.bottom) + 1 : 0;
    const top = Math.max(below(shown(document.querySelector('.fps'))), below(shown(document.querySelector('progress.download'))),
        creditOnTop ? below(credit) : 0);
    const pixels = context.getImageData(0, 0, image.width, image.height).data;
    let detail = 0;
    for (let y = top; y < bottom; y += 8) {
        let low = 255, high = 0;
        for (let x = 0; x < right; x += 4) {
            const i = (y * image.width + x) * 4;
            const luma = 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
            low = Math.min(low, luma); high = Math.max(high, luma);
        }
        detail = Math.max(detail, high - low);
    }
    return Math.round(detail);
})()"""
# A row with a model in it spans at least this much luma. Measured with kFrameDetail: the gate's
# sky-only frames 20, a black frame 0, the viewer's model frames 159 to 244.
kMinFrameDetail = 40


def FrameDetail(cdp: Cdp, shot: str):
    """kFrameDetail of the base64 PNG `shot`, read in the current page; None when unreadable."""
    reply = cdp.Send("Runtime.evaluate", expression=kFrameDetail.replace("PNG_BASE64", shot),
                     returnByValue=True, awaitPromise=True, _timeout=60.0)
    return reply.get("result", {}).get("result", {}).get("value")


def ShowsModel(detail) -> bool:
    """Whether a frame of this FrameDetail has a model in it."""
    return isinstance(detail, (int, float)) and detail >= kMinFrameDetail


def RunSteps(cdp: Cdp, args: argparse.Namespace) -> list[int]:
    """Runs each --step and prints what the frame shows after it; returns the steps that threw
    or whose frame shows no model (ShowsModel)."""
    failed = []
    for index, expression in enumerate(args.step, 1):
        result = cdp.Send("Runtime.evaluate", expression=expression, awaitPromise=True,
                          returnByValue=True, _timeout=180.0)
        failure = result.get("result", {}).get("exceptionDetails")
        cdp.Pump(args.click_settle)
        shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
        png = base64.b64decode(shot)
        if args.screenshot:
            args.screenshot.with_name(f"{args.screenshot.stem}-step{index}.png").write_bytes(png)
        detail = FrameDetail(cdp, shot)
        if failure or not ShowsModel(detail):
            failed.append(index)
        print(f"browser_gate: step {index} {expression}: frame detail {detail}, centre pixel {CentrePixel(png)}"
              + (f"; threw {failure.get('exception', {}).get('description', failure.get('text'))}" if failure else ""))
    return failed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--page", default="WebPlayer.html")
    parser.add_argument("--port", type=int, default=8123)
    parser.add_argument("--cdp-port", type=int, default=9222,
                        help="Chrome remote-debugging port; give concurrent runs "
                             "distinct ports or they attach to each other's browser")
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--grep", action="append", default=[],
                        help="print only console lines containing one of these")
    parser.add_argument("--eval", dest="evaluate",
                        help="JS expression evaluated in the page after the run; "
                             "its value is printed (MEMFS inspection, counters)")
    parser.add_argument("--stack-frames", type=int, default=200,
                        help="CDP call-stack capture depth for exceptions")
    parser.add_argument("--chrome-flag", action="append", default=[],
                        help="extra Chrome command-line flag, repeatable "
                             "(e.g. --chrome-flag=--js-flags=--stack-size=4000)")
    parser.add_argument("--reloads", type=int, default=0,
                        help="reload the page this many extra times, pumping "
                             "--seconds each; a reload drops linear memory, so "
                             "anything that survives it came from storage")
    parser.add_argument("--click", action="append", default=[],
                        help="after the boot pump, click the canvas at this "
                             "'x,y' (page pixels), repeatable in order. The "
                             "click is DOM mouse events dispatched on the canvas, "
                             "the only input path that reaches a headless page; "
                             "CDP's Input domain does not")
    parser.add_argument("--step", action="append", default=[],
                        help="after the boot pump (and any --click), evaluate this JS "
                             "expression in the page and await it, repeatable in order; "
                             "after each, pump --click-settle seconds and capture the "
                             "frame (beside --screenshot as <name>-step<N>.png). A step "
                             "that throws, or whose frame shows no model (no row with "
                             "kMinFrameDetail of luma range), fails the run")
    parser.add_argument("--click-settle", type=float, default=5.0,
                        help="seconds pumped after each --click so the page "
                             "can redraw before the transcript and screenshot")
    parser.add_argument("--expect", action="append", default=[],
                        help="fail unless some console line contains this text, "
                             "repeatable: a page's own verdict line")
    parser.add_argument("--wasm-memory", action="store_true",
                        help="after the run, print the byte size of every WebAssembly "
                             "memory alive in the page. A memory only grows, so this is "
                             "the run's high-water mark")
    parser.add_argument("--user-data-dir", type=Path,
                        help="reuse this Chrome profile instead of a throwaway "
                             "one. Origin storage (OPFS, IndexedDB) lives in the "
                             "profile, so persistence across separate runs is "
                             "only observable with this set")
    args = parser.parse_args()

    os.chdir(args.root)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), WasmHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    # A throwaway profile is the right default — a gate should not inherit
    # state from the last run — but it also discards origin storage, which is
    # exactly what a persistence test needs to keep.
    keepProfile = args.user_data_dir is not None
    if keepProfile:
        profile = str(args.user_data_dir)
        os.makedirs(profile, exist_ok=True)
    else:
        profile = tempfile.mkdtemp(prefix="ge_browser_gate_")
    chrome = subprocess.Popen(
        [FindChrome(), "--headless=new", f"--remote-debugging-port={args.cdp_port}",
         f"--user-data-dir={profile}", "--enable-unsafe-swiftshader",
         "--enable-features=Vulkan", f"--use-angle={kAngleBackend}",
         "--window-size=1280,720", "--no-first-run", "--no-default-browser-check",
         *args.chrome_flag,
         "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        target = None
        for _ in range(60):
            try:
                with urllib.request.urlopen(
                        f"http://127.0.0.1:{args.cdp_port}/json/list") as response:
                    tabs = json.load(response)
                target = next((t for t in tabs if t.get("type") == "page"), None)
                if target:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        if target is None:
            print("browser_gate: Chrome never exposed a page target", file=sys.stderr)
            return 2

        cdp = Cdp(target["webSocketDebuggerUrl"])
        cdp.Send("Runtime.enable")
        # A stack-exhaustion trace is only diagnostic at full depth; CDP's
        # default cap silently truncates it to the innermost frames.
        cdp.Send("Runtime.setMaxCallStackSizeToCapture", size=args.stack_frames)
        cdp.Send("Log.enable")
        cdp.Send("Page.enable")
        if args.wasm_memory:
            cdp.Send("Page.addScriptToEvaluateOnNewDocument", source=kWasmMemoryRecorder)
        cdp.Send("Page.navigate", url=f"http://127.0.0.1:{args.port}/{args.page}")
        cdp.Pump(args.seconds)
        for reload in range(args.reloads):
            print(f"browser_gate: --- reload {reload + 1} of {args.reloads} ---")
            cdp.Send("Page.reload", ignoreCache=False)
            cdp.Pump(args.seconds)
        for click in args.click:
            x, y = (float(v) for v in click.split(","))
            print(f"browser_gate: --- click ({x:g}, {y:g}) ---")
            cdp.Send("Runtime.evaluate", returnByValue=True, expression=(
                "(() => { const c = document.getElementById('canvas');"
                " const r = c.getBoundingClientRect();"
                f" const cx = r.left + {x}, cy = r.top + {y};"
                " for (const t of ['mousemove', 'mousedown', 'mouseup', 'click'])"
                "   c.dispatchEvent(new MouseEvent(t, {clientX: cx, clientY: cy,"
                "     bubbles: true, cancelable: true, button: 0,"
                "     buttons: t === 'mousedown' ? 1 : 0}));"
                " return 'ok'; })()"))
            cdp.Pump(args.click_settle)
        failedSteps = RunSteps(cdp, args)

        # The transcript comes out before anything that needs the renderer to
        # answer. Both Runtime.evaluate and Page.captureScreenshot block on the
        # page's main thread, so a wedged page used to yield an empty report --
        # the one case where the console is most worth reading.
        # Every line carries the time it arrived, relative to the first event of
        # the run. Without it a transcript cannot answer the question that
        # separates "slow" from "stopped": a frame loop that dies mid-window
        # leaves a canvas showing its last good frame and a console whose final
        # heartbeat looks like any other.
        records = [(ConsoleTime(e), ConsoleText(e)) for e in cdp.events]
        records = [(t, line) for t, line in records if line]
        origin = next((t for t, _ in records if t is not None), None)
        lines = [line for _, line in records]
        for timestamp, line in records:
            if not args.grep or any(g in line for g in args.grep):
                elapsed = None
                if timestamp is not None and origin is not None:
                    elapsed = (timestamp - origin) / 1000.0
                print(StampedLine(line, elapsed))
        sys.stdout.flush()

        if args.evaluate:
            evaluated = cdp.Send("Runtime.evaluate", expression=args.evaluate,
                                 returnByValue=True, awaitPromise=True)
            print(json.dumps(evaluated.get("result", {}), indent=1)[:200000])

        if args.wasm_memory:
            print(f"browser_gate: wasm memory bytes {WasmMemoryBytes(cdp)}")

        png = b""
        try:
            shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)
            png = base64.b64decode(shot["result"]["data"])
            if args.screenshot:
                args.screenshot.write_bytes(png)
        except (socket.timeout, TimeoutError):
            print("browser_gate: Page.captureScreenshot did not answer within 30s — the page's "
                  "main thread is blocked. No screenshot; the transcript above is the evidence.")

        # Dawn reports through the engine's uncaptured-error hook ("WebGPU error
        # (2): ...") as well as its own console text, and a first failure cascades
        # into "is invalid due to a previous error" for the rest of the frame.
        # Matching only the word "validation" counted zero while 165 real errors
        # were on the console — match what the engine and Dawn actually print.
        kErrorMarkers = ("WebGPU error", "is invalid due to a previous error",
                         "Validation", "validation error")
        validation = [line for line in lines
                      if any(marker in line for marker in kErrorMarkers)]
        centre = CentrePixel(png)
        print(f"\nbrowser_gate: {len(lines)} console lines, "
              f"{len(validation)} validation errors, centre pixel {centre}")
        if args.screenshot:
            print(f"browser_gate: screenshot -> {args.screenshot}")
        missing = [text for text in args.expect if not any(text in line for line in lines)]
        for text in missing:
            print(f"browser_gate: no console line contains the expected '{text}'")
        return 1 if validation or missing or failedSteps else 0
    finally:
        chrome.terminate()
        # Chrome flushes origin storage on exit; a run that reused a profile has
        # to wait for that before the next run can read it back.
        try:
            chrome.wait(timeout=15)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        if not keepProfile:
            shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
