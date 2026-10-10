#!/usr/bin/env python3
"""The web package's origin rules in headless Chrome: the CDN form and cross-origin loads.

    python Tools/Web/web_package_gate.py --module <package dir> --model <file.glb> \\
        --out <evidence dir>

`--module` is a package build_web_package.py wrote. Each case serves a generated page from a
local origin, isolated (COOP same-origin, COEP require-corp: the threaded build) or not (the
single-threaded build), and records whether the engine started, whether the model loaded, the
console, the centre pixel and a screenshot. The cases and the rule each exercises:

  cdn-*     the facade imported from a second origin that answers with CORS headers, as a CDN
            does. At mt the engine module's pthread workers start from the glue's URL, which
            must share the page's origin: with the glue copied beside the page (coreUrl) the
            engine starts, without it it does not; at st there are no workers and the CDN form
            needs no copy.
  load-*    scene.load of a model on another origin. The page reads the file's bytes, which a
            browser allows only with Access-Control-Allow-Origin, at either build: the host
            with CORS headers loads, a host with none and a host with only
            Cross-Origin-Resource-Policy fail with the library's message naming CORS.
  parts-*   scene.load of the chess set (A Beautiful Game, from the Khronos sample assets) at
            either build: every node that draws one of its shared meshes is an entity under the
            model, 49 of them (the board and the 32 pieces, each pawn in two parts).
  viewer-*  the shipped model viewer page (examples/model-viewer) at mt: the default model, the
            Fox, shows; its Animation section lists the clips the engine names and plays the first,
            its clip time advancing; with its run clip picked two frames a moment apart differ by
            more than two frames of the paused fox do, and the clip time read from the engine
            advances while playing and holds while paused; a model without clips (the Avocado)
            leaves the section disabled with its note; the Fox's credit is on screen over the
            canvas; at the default size the panel fits the viewport with its Pause button
            uncovered, and on a phone the open strip takes at most half the page with Pause in its
            view. The landing (a cold first load), every pick, the narrow and phone steps are
            framed in the free area's middle at the page's fit (kModelFramed, its numbers in each
            record); the visitor's zoom survives F and the panel's toggle; a turntable revolution
            keeps the Fox inside the free area and a re-frame at a turn lands where one at yaw 0
            does; switching the turntable on and off again keeps the orbit and the Fox's place on
            screen; the page's own turntable, run with the clip paused, turns the Fox about its box's
            centre, its live box the recorded one turned; short phone pages keep the orbit finite;
            an elevation pair (set by a drag on the canvas) and speed 0 are recorded. On a cold
            ~20 Mbit/s link the status counts the engine's download as a percentage, says the
            engine is starting, then reports a model's download (the 0.16 MB
            Fox on a 400 kbit/s link) as a percentage when its size is known or a sweeping bar
            otherwise. The bar along the top edge advances between two samples (on a phone-sized
            page too, panel closed); each listed sample model loads through the picker (the model
            changes, no validation error); a local .glb opens through the file input (set over CDP,
            no desktop input); F shows the frame-rate readout; the slider sets the time of day; in the
            GI preview (?gi=1), at a pinned exposure, turning global illumination on lifts the local
            model's lower flank above the noise floor, darkens neither its contact band nor a
            crease, and with the floor switched off lights it as a page whose floor was never
            created does.
  instancing-*  the instancing page, served as the viewer is: without ?turntable=1 it has no
            turning switch, its copies stay at rest and its panel does not mention turning; with
            the flag, the slider's top, 100,000 copies, read back as entities that carry a
            MeshRenderer, draws with no validation error.

Exit 0 when every case matches its expectation. Builds on browser_gate.py's Chrome driver.
"""

from __future__ import annotations

import argparse
import base64
import functools
import http.server
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import browser_gate  # noqa: E402
import build_web_package  # noqa: E402

kIsolationHeaders = {"Cross-Origin-Opener-Policy": "same-origin",
                     "Cross-Origin-Embedder-Policy": "require-corp"}
kCorsHeaders = {"Access-Control-Allow-Origin": "*"}
kCorpHeaders = {"Cross-Origin-Resource-Policy": "cross-origin"}
# What the gate copies beside a page for the same-origin bootstrap of the CDN form, with the
# engine pack (whole, or split as on the site).
kCoreFiles = ("opengine-core-binding.js", "opengine-core.st.js", "opengine-core.st.wasm",
              "opengine-core.mt.js", "opengine-core.mt.wasm")
kErrorMarkers = ("WebGPU error", "is invalid due to a previous error", "Validation",
                 "validation error")
# A cooked-only runtime that has no program for a material it was asked to draw says so on the
# console and draws nothing for it, with no validation error.
kMissingProgramMarker = "no cooked program"
# The LOD-crossfade warm-up keys every material registration asks for, which the cook does not
# produce yet (#3139): warnings for variants nothing draws.
kCrossfadeWarmupDefine = "GE_LOD_CROSSFADE"


def MissingPrograms(lines: list[str]) -> list[str]:
    """The console lines naming a material variant the pack lacks, the crossfade warm-ups aside."""
    return [line for line in lines
            if kMissingProgramMarker in line and kCrossfadeWarmupDefine not in line]

# The page each case loads: the facade from FACADE, the engine on the canvas, the model from
# MODEL; the outcome lands in window.__gate and on the console.
kPageTemplate = """<!doctype html>
<html><head><meta charset="utf-8"><title>web package gate</title>
<style>html, body {{ margin: 0; height: 100%; background: #000; }}
canvas {{ display: block; width: 100%; height: 100%; }}</style></head>
<body><canvas></canvas>
<script type="module">
const report = (outcome) => {{ window.__gate = outcome; console.log('GATE ' + JSON.stringify(outcome)); }};
const why = (error) => String((error && error.message) || error);
// The entities under a model loaded into a fresh world: the factory creates them right after the
// model root, so their ids follow its id with no recycled slot between them.
const CountParts = (scene, model) => {{
    let parts = 0;
    for (let id = model.id + 1; ; ++id) {{
        try {{ if (scene.entityFor(id).transform.parent !== model) break; }} catch {{ break; }}
        ++parts;
    }}
    return parts;
}};
try {{
    const {{ Engine, OrbitControls }} = await import({facade});
    const canvas = document.querySelector('canvas');
    const engine = await Engine.create({{ canvas, threads: {threads}{coreUrl} }});
    let loaded = false, error = null, parts = null;
    try {{
        const model = await engine.scene.load({model});
        parts = CountParts(engine.scene, model);
        const {{ center, size }} = model.bounds;
        const controls = new OrbitControls(engine.scene.camera, canvas);
        controls.target.copy(center);
        controls.distance = 2 * Math.max(size.x, size.y, size.z);
        loaded = true;
    }} catch (loadError) {{ error = why(loadError); }}
    engine.run();
    report({{ created: true, isolated: crossOriginIsolated, loaded, error, parts }});
}} catch (createError) {{
    report({{ created: false, isolated: crossOriginIsolated, loaded: false, error: why(createError) }});
}}
</script></body></html>
"""


# The chess set: 49 nodes draw its 15 meshes (each pawn, rook, knight and bishop mesh several
# times), so a model that keeps only the first node of each mesh shows 12 of its 32 pieces.
kChessUrl = ("https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/"
             "ABeautifulGame/glTF-Binary/ABeautifulGame.glb")
kChessParts = 49


@dataclass
class Case:
    name: str
    rule: str
    isolated: bool
    facadeOrigin: str          # "page" or "cdn"
    bootstrap: bool            # the glue copied beside the page, passed as coreUrl
    modelOrigin: str           # "page", "cors", "none", "corp" or "chess" (kChessUrl)
    expectCreated: bool
    expectLoaded: bool
    expectError: str | None = None   # a fragment the page's error message must contain
    expectParts: int | None = None   # the entities under the loaded model


def Cases() -> list[Case]:
    workerRule = "mt starts its workers from the glue's URL, which must share the page's origin"
    corsRule = "another origin's file loads only with Access-Control-Allow-Origin, at either build"
    partsRule = "every node that draws a shared mesh is an entity, at its node's transform"
    cases = [
        Case("cdn-mt-bootstrap", workerRule, True, "cdn", True, "page", True, True),
        Case("cdn-mt-no-bootstrap", workerRule, True, "cdn", False, "page", False, False,
             "Failed to construct 'Worker'"),
        Case("cdn-st", workerRule, False, "cdn", False, "page", True, True),
    ]
    for build, isolated in (("st", False), ("mt", True)):
        cases += [
            Case(f"load-{build}-cors", corsRule, isolated, "page", False, "cors", True, True),
            Case(f"load-{build}-no-headers", corsRule, isolated, "page", False, "none", True, False, "CORS"),
            Case(f"load-{build}-corp-only", corsRule, isolated, "page", False, "corp", True, False, "CORS"),
            Case(f"parts-{build}", partsRule, isolated, "page", False, "chess", True, True,
                 expectParts=kChessParts),
        ]
    return cases


class HeaderHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = browser_gate.WasmHandler.extensions_map

    def __init__(self, *args, headers: dict[str, str], **kwargs):
        self.extraHeaders = headers
        super().__init__(*args, **kwargs)

    def end_headers(self):
        for key, value in self.extraHeaders.items():
            self.send_header(key, value)
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


class QuietServer(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # A page that navigates away resets its open connections; that is not a server fault.
        if not isinstance(sys.exc_info()[1], ConnectionError):
            super().handle_error(request, client_address)


def Serve(root: Path, port: int, headers: dict[str, str]) -> http.server.ThreadingHTTPServer:
    handler = functools.partial(HeaderHandler, directory=str(root), headers=headers)
    server = QuietServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


class Origins:
    """The page origins (plain and isolated), the CDN and the three asset hosts."""

    def __init__(self, basePort: int):
        names = ("page", "isolated", "cdn", "cors", "none", "corp")
        self.ports = {name: basePort + index for index, name in enumerate(names)}

    def Url(self, name: str) -> str:
        return f"http://127.0.0.1:{self.ports[name]}"


def WritePage(case: Case, root: Path, origins: Origins, modelName: str) -> str:
    facade = (f"{origins.Url('cdn')}/opengine.mjs" if case.facadeOrigin == "cdn"
              else "./package/opengine.mjs")
    coreUrl = ", coreUrl: new URL('./core/', location.href).href" if case.bootstrap else ""
    if case.modelOrigin == "chess":
        model = kChessUrl
    elif case.modelOrigin == "page":
        model = f"./{modelName}"
    else:
        model = f"{origins.Url(case.modelOrigin)}/{modelName}"
    page = kPageTemplate.format(facade=json.dumps(facade),
                                threads=json.dumps("multi" if case.isolated else "single"),
                                coreUrl=coreUrl, model=json.dumps(model))
    (root / f"{case.name}.html").write_text(page, encoding="utf-8")
    return f"{origins.Url('isolated' if case.isolated else 'page')}/{case.name}.html"


def StartChrome(cdpPort: int, profile: str) -> subprocess.Popen:
    return subprocess.Popen(
        [browser_gate.FindChrome(), "--headless=new", f"--remote-debugging-port={cdpPort}",
         f"--user-data-dir={profile}", "--enable-unsafe-swiftshader", "--enable-features=Vulkan",
         f"--use-angle={browser_gate.kAngleBackend}", "--window-size=1280,720", "--no-first-run",
         "--no-default-browser-check", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def ConnectCdp(cdpPort: int) -> browser_gate.Cdp:
    for _ in range(60):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{cdpPort}/json/list") as response:
                target = next((t for t in json.load(response) if t.get("type") == "page"), None)
            if target:
                cdp = browser_gate.Cdp(target["webSocketDebuggerUrl"])
                for domain in ("Runtime.enable", "Log.enable", "Page.enable"):
                    cdp.Send(domain)
                return cdp
        except OSError:
            pass
        time.sleep(0.5)
    raise RuntimeError("Chrome never exposed a page target")


def RunCase(cdp: browser_gate.Cdp, case: Case, url: str, seconds: float, out: Path) -> dict:
    cdp.events.clear()
    cdp.Send("Page.navigate", url=url)
    cdp.Pump(seconds)
    lines = [line for line in map(browser_gate.ConsoleText, cdp.events) if line]
    evaluated = cdp.Send("Runtime.evaluate", expression="window.__gate ?? null", returnByValue=True)
    outcome = evaluated.get("result", {}).get("result", {}).get("value") or {}
    png = base64.b64decode(cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)
                           ["result"]["data"])
    (out / f"{case.name}.png").write_bytes(png)
    (out / f"{case.name}.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    validation = [line for line in lines if any(marker in line for marker in kErrorMarkers)]
    missing = MissingPrograms(lines)
    centre = browser_gate.CentrePixel(png)

    failures = []
    if outcome.get("created") is not case.expectCreated:
        failures.append(f"engine created {outcome.get('created')}, expected {case.expectCreated}")
    if outcome.get("loaded") is not case.expectLoaded:
        failures.append(f"model loaded {outcome.get('loaded')}, expected {case.expectLoaded}")
    if case.expectParts is not None and outcome.get("parts") != case.expectParts:
        failures.append(f"{outcome.get('parts')} entities under the model, expected {case.expectParts}")
    if case.expectError and case.expectError not in (outcome.get("error") or ""):
        failures.append(f"the error does not contain \"{case.expectError}\"")
    if case.expectCreated:
        if validation:
            failures.append(f"{len(validation)} WebGPU validation errors")
        if missing:
            failures.append(f"{len(missing)} materials without a cooked program")
        if not centre or max(centre) == 0:
            failures.append(f"centre pixel {centre} is black")
    return {"case": case.name, "rule": case.rule, "url": url, "outcome": outcome,
            "validationErrors": len(validation), "centrePixel": centre,
            "build": next((line for line in lines if line.startswith("OpenEngine: using")), None),
            "pass": not failures, "failures": failures}


kViewerPage = "package/examples/model-viewer/index.html"
# The exposure compensation, in stops, the viewer's steps run at: the page's default, which the
# landing step checks on the engine's Camera; every pose records the compensation it was taken at.
kViewerExposureEv = 2
kExposureNow = "globalThis.viewer?.engine.scene.camera.get({ name: 'Camera' })?.exposureCompensation"
# The model the viewer opens on, as its status and its list name it.
kDefaultModel = "Fox"
# The default model's clips in the model's order, the clip the motion step plays, the listed
# model without clips, and the note the page shows for it.
kFoxClips = ("Survey", "Walk", "Run")
kMotionClip = "Run"
kStillModel = "Avocado"
kNoAnimations = "This model has no animations."
# The Animation section as the page shows it: enabled or disabled, its note when it is on screen,
# its clips (checked against the clips the engine names for the model on screen) and the panel's
# title.
kAnimationState = """(() => {
    const section = document.querySelector('.panel [name=animation]');
    const note = document.querySelector('.panel .no-animation');
    if (!section || !note) return 'no Animation section';
    const listed = [...section.querySelectorAll('[name=clip] option')].map((option) => option.value).join(', ');
    let engine = '';
    try { engine = globalThis.viewer.model().animation.clips.join(', '); } catch { /* a model without clips */ }
    return [section.disabled ? 'disabled' : 'enabled', note.checkVisibility() ? note.textContent : 'no note',
            listed === engine ? `clips ${listed || 'none'}` : `clips ${listed || 'none'}, the engine's ${engine || 'none'}`,
            document.querySelector('.panel summary').textContent.trim()].join(' | ');
})()"""
# The open panel, and the overlays over the canvas's edge (the shown model's credit, and the
# frame-rate readout while it shows): a wide page's column fits the viewport without scrolling and
# its Pause button is not covered; a narrow page's strip takes at most half the page, shows its
# scroll bar when it scrolls and has the Pause button in its view; each overlay is whole inside the viewport, clear of the panel, of the
# other overlay and of nothing else. 'clear', or what is wrong. NEEDS_CREDIT true makes a missing
# credit a problem; false checks the panel and whatever overlays show.
kPanelFits = """((needsCredit) => {
    const panel = document.querySelector('.panel'); const pause = panel.querySelector('[name=pause]');
    if (!panel.open) return 'panel closed';
    const covered = (element) => {
        const box = element.getBoundingClientRect();
        return !element.contains(document.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2));
    };
    const meets = (a, b) => a.right > b.left && a.left < b.right && a.bottom > b.top && a.top < b.bottom;
    const card = panel.getBoundingClientRect();
    const problems = [];
    // A wide page's column fits without scrolling with its Pause button in view. A narrow page's
    // strip takes at most half the page and scrolls, its scroll bar showing when it does, with the
    // Animation section's Pause button inside the strip's view.
    const strip = card.left < innerWidth / 2;
    if (!strip && panel.scrollHeight > panel.clientHeight + 1) problems.push(`the panel scrolls (${panel.scrollHeight} px of content in ${panel.clientHeight})`);
    if (!strip && covered(pause)) problems.push('the Pause button is covered');
    if (strip && card.height > innerHeight / 2 + 1) problems.push(`the strip is ${Math.round(card.height)} px of a ${innerHeight} px page`);
    if (strip && panel.scrollHeight > panel.clientHeight + 1 && panel.offsetWidth - panel.clientWidth < 4) problems.push('the strip scrolls with no scroll bar showing');
    if (strip && (pause.getBoundingClientRect().bottom > card.bottom || covered(pause))) problems.push("the Pause button is below the strip's view");
    const overlays = [['credit', document.querySelector('.credit')], ['frame-rate readout', document.querySelector('.fps')]]
        .filter(([, element]) => !element.hidden && element.textContent);
    if (needsCredit && !overlays.some(([name]) => name === 'credit')) problems.push('no credit on screen');
    for (const [name, element] of overlays) {
        const box = element.getBoundingClientRect();
        if (box.left < 0 || box.top < 0 || box.right > innerWidth || box.bottom > innerHeight)
            problems.push(`the ${name} spans ${[box.left, box.top, box.right, box.bottom].map(Math.round)} in ${innerWidth}x${innerHeight}`);
        if (meets(box, card)) problems.push(`the ${name} overlaps the panel`);
        if (covered(element)) problems.push(`the ${name} is covered`);
    }
    if (overlays.length === 2 && meets(overlays[0][1].getBoundingClientRect(), overlays[1][1].getBoundingClientRect()))
        problems.push('the credit and the frame-rate readout overlap');
    return problems.length ? problems.join('; ') : 'clear';
})(NEEDS_CREDIT)"""
kPanelAndCredit = kPanelFits.replace("NEEDS_CREDIT", "true")
kPanelFitsAnyModel = kPanelFits.replace("NEEDS_CREDIT", "false")
# The closest-zoom step: the page's minimum orbit distance on a model whose centre is empty air
# around a stand (the Flight helmet), where a distance inside the model shows only sky.
kClosestZoomModel = "Flight helmet"
kClosestZoom = """(() => { const c = globalThis.viewer.controls; c.distance = c.minDistance; c.update(); })()"""
# The build each way of serving the viewer must end up on, as the facade's log line names it.
kExpectedBuild = {"mt": "using the threaded build", "st": "using the single-threaded build",
                  "pages": "using the threaded build"}
kLocalModelName = "LocalModel.glb"
# A .glb the engine refuses: its bytes are no glTF container.
kBrokenModelName = "Broken.glb"
# A .gltf that names its buffer in a file beside it, which a page cannot hand the engine.
kExternalGltfName = "External.gltf"
kStatus = "document.querySelector('.panel output[name=status]').value"


# The page exceptions and CDP error replies strict Evaluates met since the last step ended (the
# calls a step makes and the calls between steps alike): ViewerStep and AnimationMotion fail on
# what it holds and empty it, so an expression that throws, or does not parse, cannot pass as an
# undefined value.
kPageExceptions: list[str] = []


def Evaluate(cdp: browser_gate.Cdp, expression: str, strict: bool = True):
    """The value of `expression` in the page; a promise is awaited. A `strict` evaluation that
    throws is recorded in kPageExceptions; WaitFor's polls are not strict (a page still loading
    may throw until it is ready)."""
    reply = cdp.Send("Runtime.evaluate", expression=expression, returnByValue=True, awaitPromise=True,
                     _timeout=60.0)
    if strict and "error" in reply:
        kPageExceptions.append(f"CDP refused the evaluation ({reply['error'].get('message')}) of: "
                               f"{' '.join(expression.split())[:160]}")
    result = reply.get("result", {})
    if strict and "exceptionDetails" in result:
        details = result["exceptionDetails"]
        text = details.get("exception", {}).get("description") or details.get("text") or "an exception"
        kPageExceptions.append(f"{text.splitlines()[0]} in: {' '.join(expression.split())[:160]}")
    return result.get("result", {}).get("value")


def WaitFor(cdp: browser_gate.Cdp, expression: str, expect: str, seconds: float):
    """Pumps the page until `expression` equals `expect` or `seconds` pass; its last value."""
    deadline = time.time() + seconds
    value = Evaluate(cdp, expression, strict=False)
    while value != expect and time.time() < deadline:
        cdp.Pump(1.0)
        value = Evaluate(cdp, expression, strict=False)
    return value


# Holds the pose still for a capture: the turntable off. With it off before a model loads, the
# model stays at its authored orientation (RunViewer reloads the default model once it is off).
kHoldPose = """(() => {
    const turn = document.querySelector('.panel [name=turn]');
    if (turn && turn.checked) { turn.checked = false; turn.dispatchEvent(new Event('change')); }
})()"""
# The pose a capture was taken at: the orbit (target, distance, camera position and rotation in
# degrees), the model's rotation, the time of day, the exposure compensation, the viewport and the device pixel ratio.
kReadPose = """(() => {
    const v = globalThis.viewer; const c = v?.engine.scene.camera.transform; const m = v?.model();
    const xyz = (p) => p && [p.x, p.y, p.z].map((n) => Math.round(n * 1000) / 1000);
    return { target: xyz(v?.controls.target), distance: v && Math.round(v.controls.distance * 1000) / 1000,
             cameraPosition: xyz(c?.position), cameraRotation: xyz(c?.rotation),
             modelRotation: xyz(m?.transform.rotation), modelQuaternion: m && [m.transform.quaternion.x, m.transform.quaternion.y, m.transform.quaternion.z, m.transform.quaternion.w].map((n) => Math.round(n * 1e4) / 1e4),
             timeOfDay: document.querySelector('.panel output[name=clock]')?.value, exposureCompensation: EXPOSURE,
             viewport: [innerWidth, innerHeight], devicePixelRatio };
})()""".replace("EXPOSURE", kExposureNow)


# Where each CDP session's current page was navigated, as an index into its events.
kNavigationMark: dict[int, int] = {}


def PageBuild(cdp: browser_gate.Cdp) -> str | None:
    """The build the current page last reported in its log since it was navigated; None when it
    reported none. The last: on a first visit at pages the page can start the single-threaded
    build before the service worker reloads it isolated."""
    lines = map(browser_gate.ConsoleText, cdp.events[kNavigationMark.get(id(cdp), 0):])
    builds = [line for line in lines if line.startswith("OpenEngine: using")]
    return builds[-1] if builds else None


def ViewerStep(cdp: browser_gate.Cdp, name: str, rule: str, act, check: str, expect: str,
               seconds: float, out: Path, pose: str = kReadPose, drawsModel: bool = True,
               record: str | None = None) -> dict:
    """Runs `act`, waits for the page expression `check` to equal `expect`, holds the pose and
    records the step with it (`pose`, a page expression, reads it). `drawsModel` False skips the
    model-drawn check, for a page whose open panel covers the canvas (a phone). `record`, a page
    expression, is read once the check settles and kept in the step's record (the numbers behind a
    check, pass or fail)."""
    first = len(cdp.events)
    act()
    # The build this step's page reported: the facade logs it once per page load, so the step
    # reads it from its page's log since that page's navigation (kNavigationMark).
    value = WaitFor(cdp, check, expect, seconds)
    recorded = Evaluate(cdp, record) if record else None
    Evaluate(cdp, kHoldPose)
    cdp.Pump(2.0)
    lines = [line for line in map(browser_gate.ConsoleText, cdp.events[first:]) if line]
    shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
    png = base64.b64decode(shot)
    (out / f"{name}.png").write_bytes(png)
    (out / f"{name}.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    validation = [line for line in lines if any(marker in line for marker in kErrorMarkers)]
    missing = MissingPrograms(lines)
    detail = browser_gate.FrameDetail(cdp, shot)
    failures = []
    if value != expect:
        failures.append(f"page shows {value!r}, expected {expect!r}")
    if drawsModel and not browser_gate.ShowsModel(detail):
        failures.append(f"the frame left of the panel is uniform (detail {detail}): no model drawn")
    if validation:
        failures.append(f"{len(validation)} WebGPU validation errors")
    if missing:
        failures.append(f"{len(missing)} materials without a cooked program")
    recordedPose = Evaluate(cdp, pose)
    failures += [f"the page threw: {exception}" for exception in kPageExceptions]
    del kPageExceptions[:]
    return {"case": name, "rule": rule, "outcome": value, "validationErrors": len(validation),
            "centrePixel": browser_gate.CentrePixel(png),
            "build": PageBuild(cdp),
            "frameDetail": detail, "pose": recordedPose, **({"record": recorded} if record else {}),
            "pass": not failures, "failures": failures}


# The GI step's regions on screen, in CSS pixels (x0, y0, x1, y1), as fractions of the model's
# projected radius (the bounding sphere's image) about the model's box centre on screen (GiRegions
# fills in kCentrePixel). Sized for the gate's local model (the Duck) at the held pose:
#   lowerFlank   the flank under the target, which the camera looks down at;
#   contactBand  the belly where the model meets the floor;
#   crease       the fold under the head.
kGiRegions = """(async () => {
    const { Camera } = await import('@openengine/web');
    const v = globalThis.viewer; const { size } = v.model().bounds;
    const radius = 0.5 * Math.hypot(size.x, size.y, size.z);
    const halfFov = ((v.engine.scene.camera.get(Camera)?.fovY ?? 60) * Math.PI) / 360;
    const canvas = document.querySelector('canvas');
    const d = (canvas.clientHeight / 2) * Math.tan(Math.asin(Math.min(1, radius / v.controls.distance))) / Math.tan(halfFov);
    const [x, y] = CENTRE_PIXEL;
    const box = (x0, y0, x1, y1) => [x + x0 * d, y + y0 * d, x + x1 * d, y + y1 * d].map(Math.round);
    return { lowerFlank: box(-0.25, 0.25, 0.25, 0.5), contactBand: box(-0.25, 0.5, 0.35, 0.62),
             crease: box(-0.24, -0.03, 0.11, 0.07) };
})()"""
# The mean Rec. 709 luma (0..255) of the screenshot PNG_BASE64 inside BOX. The bytes are decoded in
# place: an isolated page's service worker refuses a fetch of a data: URL.
kRegionLuma = """(async () => {
    const bytes = Uint8Array.from(atob('PNG_BASE64'), (c) => c.charCodeAt(0));
    const image = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const [x0, y0, x1, y1] = BOX;
    const context = new OffscreenCanvas(image.width, image.height).getContext('2d');
    context.drawImage(image, 0, 0);
    const { data } = context.getImageData(x0, y0, x1 - x0, y1 - y0);
    let sum = 0;
    for (let i = 0; i < data.length; i += 4) sum += 0.2126 * data[i] + 0.7152 * data[i + 1] + 0.0722 * data[i + 2];
    return sum / (data.length / 4);
})()"""
# Switches a panel checkbox (floor, gi) as a click would.
kSetCheck = """(() => { const box = document.querySelector('.panel [name=NAME]');
    if (box.checked !== CHECKED) { box.checked = CHECKED; box.dispatchEvent(new Event('change')); } })()"""
# The probe field converges in a few seconds of frames; the step waits this long after a change.
kGiSettleSeconds = 15.0
# Frames between the two captures of one state: their difference is that state's noise.
kGiNoiseFrames = 60
# GI must lift the lower flank by at least this many luma levels, and by three noise floors.
kGiMinLumaDelta = 2.0
# Two arms that must agree (the floor switched off against a floor never created; GI on against
# off where GI must not darken) may differ by this many luma levels, or three noise floors.
kGiLumaTolerance = 1.0
# The GI cases' exposure: the camera's own default manual EV100, so no arm moves the meter.
kGiExposureEv = 15.4
kPinExposure = f"""(async () => {{ const {{ Camera }} = await import('@openengine/web');
    globalThis.viewer.engine.scene.camera.set(Camera, {{ exposureControl: 'Manual', manualExposureEV: {kGiExposureEv} }});
    return 'ok'; }})()"""
# Counts the page's frames from here on (globalThis.__giFrames).
kCountFrames = """(() => { globalThis.__giFrames = 0;
    globalThis.viewer.engine.onFrame(() => { ++globalThis.__giFrames; }); return 'ok'; })()"""
# Runs before the page's own scripts: the floor switch starts off, so the floor is never created.
kFloorNeverCreated = """document.addEventListener('DOMContentLoaded', () => {
    const box = document.querySelector('.panel [name=floor]'); if (box) box.checked = false; });"""
# How many canvas pixels' Rec. 709 luma differs between the screenshots A and B by more than each
# step of the ladder (1, 4, 8 and 16 levels).
kPixelLadder = """(async () => {
    const decode = async (b64) => { const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
        const image = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
        const context = new OffscreenCanvas(image.width, image.height).getContext('2d'); context.drawImage(image, 0, 0);
        return context.getImageData(0, 0, image.width, image.height).data; };
    const [a, b] = [await decode('A_BASE64'), await decode('B_BASE64')];
    const counts = { 1: 0, 4: 0, 8: 0, 16: 0 };
    for (let i = 0; i < a.length; i += 4) {
        const la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
        const lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
        for (const step of [1, 4, 8, 16]) if (Math.abs(la - lb) > step) ++counts[step];
    }
    return counts;
})()"""


def GiRegions() -> str:
    """kGiRegions about the model's box centre on screen (kCentrePixel): the page frames the model
    in the area the panel leaves free, not in the canvas's middle."""
    return kGiRegions.replace("CENTRE_PIXEL", kCentrePixel)


def SetCheck(cdp: browser_gate.Cdp, name: str, checked: bool) -> None:
    Evaluate(cdp, kSetCheck.replace("NAME", name).replace("CHECKED", "true" if checked else "false"))


def RegionLuma(cdp: browser_gate.Cdp, shot: str, box: list[int]) -> float:
    return Evaluate(cdp, kRegionLuma.replace("BOX", json.dumps(box)).replace("PNG_BASE64", shot))


def OpenGiPreview(cdp: browser_gate.Cdp, url: str, localModel: Path, seconds: float) -> str:
    """Opens the viewer with its GI preview (?gi=1), the turntable off, the local model shown at the
    pinned exposure, and the page's frames counted. Returns the status the page ended on."""
    Navigate(cdp, "Page.navigate", url=f"{url}?gi=1")
    WaitFor(cdp, "typeof globalThis.viewer", "object", seconds)
    Evaluate(cdp, kHoldPose)
    WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)
    SetLocalFile(cdp, localModel)
    status = WaitFor(cdp, kStatus, f"Showing {kLocalModelName}", seconds)
    Evaluate(cdp, kPinExposure)
    Evaluate(cdp, kCountFrames)
    return status


def CaptureGi(cdp: browser_gate.Cdp, label: str, shots: dict, frames: dict) -> None:
    shots[label] = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
    frames[label] = Evaluate(cdp, "globalThis.__giFrames")


def CaptureGiPair(cdp: browser_gate.Cdp, label: str, shots: dict, frames: dict) -> None:
    """Two captures of one state kGiNoiseFrames frames apart: `label`-1 and `label`-2."""
    CaptureGi(cdp, f"{label}-1", shots, frames)
    target = (frames[f"{label}-1"] or 0) + kGiNoiseFrames
    while (Evaluate(cdp, "globalThis.__giFrames") or 0) < target:
        cdp.Pump(0.1)
    CaptureGi(cdp, f"{label}-2", shots, frames)


def ReopenViewer(cdp: browser_gate.Cdp, url: str, seconds: float) -> None:
    """Back to the shipped page (no ?gi=1) on its default model, the turntable off."""
    Navigate(cdp, "Page.navigate", url=url)
    WaitFor(cdp, "typeof globalThis.viewer", "object", seconds)
    Evaluate(cdp, kHoldPose)
    WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)


def GiStep(cdp: browser_gate.Cdp, url: str, localModel: Path, seconds: float, out: Path) -> dict:
    """The GI preview (?gi=1) at a pinned exposure, the turntable off, on the gate's local model.
    The control: GI on in a page whose floor was never created. The page: GI off, GI on with the
    floor, then the floor switched off. Each state is captured twice kGiNoiseFrames frames apart;
    a region's noise floor is the largest difference within a pair. GI on must lift the lower
    flank above it, must not darken the contact band or the crease, and with the floor switched
    off must light every region as the page whose floor was never created does."""
    name = "viewer-gi"
    first = len(cdp.events)
    shots: dict[str, str] = {}
    frames: dict[str, int] = {}
    script = cdp.Send("Page.addScriptToEvaluateOnNewDocument", source=kFloorNeverCreated)["result"]["identifier"]
    statuses = [OpenGiPreview(cdp, url, localModel, seconds)]
    cdp.Send("Page.removeScriptToEvaluateOnNewDocument", identifier=script)
    controlRegions = Evaluate(cdp, GiRegions())
    SetCheck(cdp, "gi", True)
    cdp.Pump(kGiSettleSeconds)
    CaptureGiPair(cdp, "never-floor-on", shots, frames)
    statuses.append(OpenGiPreview(cdp, url, localModel, seconds))
    regions = Evaluate(cdp, GiRegions())
    SetCheck(cdp, "gi", False)
    cdp.Pump(3.0)
    CaptureGiPair(cdp, "off", shots, frames)
    SetCheck(cdp, "gi", True)
    cdp.Pump(kGiSettleSeconds)
    CaptureGiPair(cdp, "on", shots, frames)
    pose = Evaluate(cdp, kReadPose)
    SetCheck(cdp, "floor", False)
    cdp.Pump(kGiSettleSeconds)
    CaptureGiPair(cdp, "floor-off", shots, frames)
    build = PageBuild(cdp)
    for label, shot in shots.items():
        (out / f"{name}-{label}.png").write_bytes(base64.b64decode(shot))
    lines = [line for line in map(browser_gate.ConsoleText, cdp.events[first:]) if line]
    (out / f"{name}.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    validation = [line for line in lines if any(marker in line for marker in kErrorMarkers)]
    missing = MissingPrograms(lines)
    failures = [f"the local model was not shown before a GI capture (the page read {status!r})"
                for status in statuses if status != f"Showing {kLocalModelName}"]
    if not isinstance(regions, dict) or regions != controlRegions:
        failures.append(f"the regions differ between the control and the page: {controlRegions!r} vs {regions!r}")
    regions = regions if isinstance(regions, dict) else {}
    luma = {region: {label: RegionLuma(cdp, shot, box) for label, shot in shots.items()}
            for region, box in regions.items()}
    ladder = Evaluate(cdp, kPixelLadder.replace("A_BASE64", shots["floor-off-2"])
                      .replace("B_BASE64", shots["never-floor-on-2"]))
    summary = []
    for region, values in luma.items():
        if not all(isinstance(value, (int, float)) for value in values.values()):
            failures.append(f"the {region}'s luma could not be read: {values!r}")
            continue
        noise = max(abs(values[f"{state}-2"] - values[f"{state}-1"]) for state in ("off", "on", "floor-off", "never-floor-on"))
        tolerance = max(kGiLumaTolerance, 3.0 * noise)
        lift = values["on-2"] - values["off-2"]
        floorOff = values["floor-off-2"] - values["never-floor-on-2"]
        if region == "lowerFlank" and not lift > max(kGiMinLumaDelta, 3.0 * noise):
            failures.append(f"GI moved the lower flank by {lift:+.2f} luma levels; the noise floor is {noise:.2f}")
        if region != "lowerFlank" and lift < -tolerance:
            failures.append(f"GI darkened the {region} by {lift:+.2f} luma levels (noise floor {noise:.2f})")
        if abs(floorOff) > tolerance:
            failures.append(f"with the floor switched off the {region} is {floorOff:+.2f} luma levels from the page whose "
                            f"floor was never created (noise floor {noise:.2f})")
        summary.append(f"{region}: off {values['off-2']:.2f}, on {values['on-2']:.2f} ({lift:+.2f}); floor off "
                       f"{values['floor-off-2']:.2f}, never created {values['never-floor-on-2']:.2f}; noise {noise:.2f}")
    if validation:
        failures.append(f"{len(validation)} WebGPU validation errors")
    if missing:
        failures.append(f"{len(missing)} materials without a cooked program")
    ReopenViewer(cdp, url, seconds)
    return {"case": name,
            "rule": f"at EV100 {kGiExposureEv}, GI lifts the lower flank above the noise floor, does not darken the "
                    "contact band or the crease, and a switched-off floor lights the model as a floor never created does",
            "outcome": "; ".join(summary) + f"; floor off vs never created, canvas pixels over 1/4/8/16 levels: {ladder}",
            "validationErrors": len(validation), "regions": regions, "luma": luma, "frames": frames,
            "pixelLadder": ladder, "exposureEv100": kGiExposureEv,
            "build": build, "pose": pose, "pass": not failures, "failures": failures}


# A link of about 20 Mbit/s with 40 ms of latency; and no throttling.
kThrottled = {"offline": False, "latency": 40, "downloadThroughput": 20e6 / 8, "uploadThroughput": 5e6 / 8}
kUnthrottled = {"offline": False, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1}
# The status and the download bar while a download counts its bytes.
kProgressSample = """(() => {
    const bar = document.querySelector('progress.download');
    return { status: document.querySelector('.panel output[name=status]')?.value ?? null,
             shown: !!bar && !bar.hidden, value: bar ? bar.value : null, max: bar ? bar.max : null,
             position: bar ? bar.position : null,
             sweep: bar ? getComputedStyle(bar).backgroundPosition : null };
})()"""
# The two samples of a download and the sky screenshot, taken in that order once the status
# counts it, fit inside a download of about 3 s: a 9 MB sample model at the throttle, or the
# 0.16 MB default model on kSlowLink.
kSampleGapSeconds = 1.0
kSlowLink = {"offline": False, "latency": 40, "downloadThroughput": 400e3 / 8, "uploadThroughput": 400e3 / 8}


# A frame with the sky drawn and no model: browser_gate.FrameDetail reads a black frame at 0, the gate's
# sky-only frames at 20 and the viewer's sky before its first model at 32.
kMinSkyDetail = 10


def ProgressSamples(cdp: browser_gate.Cdp, label: str, seconds: float, between=None) -> dict:
    """Samples a visible download twice: known totals advance numerically; unknown totals
    show an indeterminate bar whose accent sweeps. A compressed model's decoded size can be
    unknown until its last byte arrives, even when the response has Content-Length."""
    counting = re.compile(rf"^Loading {re.escape(label)} \d+%$")

    def IsDownloading(sample: dict) -> bool:
        # The page's markup names the engine's download before any progress report, with the
        # bar hidden and its position -1: only a shown bar is a download.
        if not sample.get("shown"):
            return False
        status = sample.get("status") or ""
        position = sample.get("position")
        if status == f"Loading {label}...":
            return position == -1
        return bool(counting.match(status) and isinstance(position, (int, float)) and 0 <= position <= 1)

    deadline = time.time() + seconds
    while not IsDownloading(Evaluate(cdp, kProgressSample) or {}) and time.time() < deadline:
        cdp.Pump(0.25)
    first = Evaluate(cdp, kProgressSample) or {}
    cdp.Pump(kSampleGapSeconds)
    second = Evaluate(cdp, kProgressSample) or {}
    record, failures = between() if between else ({}, [])
    samples = [first, second]
    failures += [f"the status {sample.get('status')!r} and bar position {sample.get('position')} "
                 f"do not show a download of {label}" for sample in samples if not IsDownloading(sample)]
    failures += ["the download bar is hidden" for sample in samples if not sample.get("shown")]
    if all(IsDownloading(sample) for sample in samples):
        if all(sample.get("position") == -1 for sample in samples):
            if not first.get("sweep") or first["sweep"] == second.get("sweep"):
                failures.append(f"the indeterminate download bar did not sweep in {kSampleGapSeconds} s")
        elif all(sample.get("position") != -1 for sample in samples):
            if not (second.get("value") or 0) > (first.get("value") or 0):
                failures.append(f"the download bar did not advance in {kSampleGapSeconds} s "
                                f"({first.get('value')} then {second.get('value')} of {second.get('max')})")
    return {"label": label, "samples": samples, **record, "failures": failures}


def SkyBeforeModel(cdp: browser_gate.Cdp, path: Path):
    """A `between` for ProgressSamples: the frame while the model downloads shows the sky, and the
    model's bar is not yet full."""
    def Check():
        shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
        # The bar read right after the frame: not full then means the frame came before the model.
        bar = Evaluate(cdp, kProgressSample) or {}
        path.write_bytes(base64.b64decode(shot))
        detail = browser_gate.FrameDetail(cdp, shot)
        failures = []
        if not isinstance(detail, (int, float)) or detail < kMinSkyDetail:
            failures.append(f"the frame is blank while the model downloads (detail {detail}): no sky drawn")
        if bar.get("max") is None or not (bar.get("value") or 0) < bar["max"]:
            failures.append(f"the model's bar was full ({bar.get('value')} of {bar.get('max')}) when the sky frame was taken")
        return {"skyFrame": path.name, "skyDetail": detail, "skyBar": bar}, failures
    return Check


def OverlaysWhileLoading(cdp: browser_gate.Cdp, path: Path):
    """A `between` for ProgressSamples: while the next model downloads, the model on screen keeps
    its credit, whole and clear of the panel, and the download bar is on screen with it."""
    def Check():
        shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
        overlays = Evaluate(cdp, kPanelAndCredit)
        bar = Evaluate(cdp, kProgressSample) or {}
        path.write_bytes(base64.b64decode(shot))
        failures = [] if overlays == "clear" else [f"while the next model downloads: {overlays}"]
        if not bar.get("shown"):
            failures.append("the download bar is hidden in the mid-download frame")
        return {"midFrame": path.name, "midOverlays": overlays, "midBar": bar}, failures
    return Check


kStarting = "Starting the engine..."


def StartingAfterDownload(cdp: browser_gate.Cdp, nextLabel: str, seconds: float):
    """A `between` for the engine's ProgressSamples: once its bytes are in, the status says the
    engine is starting, with the bar running without a value (position -1), before the download
    of `nextLabel` counts. The wait fails when the status stops changing for `seconds`, not when
    the download is long: the engine's bytes take their size over the throttled link's rate."""
    def Check():
        deadline = time.time() + seconds
        seen = []
        while time.time() < deadline:
            sample = Evaluate(cdp, kProgressSample) or {}
            status = sample.get("status") or ""
            if not seen or seen[-1]["status"] != status:
                seen.append(sample)
                deadline = time.time() + seconds
            if status == kStarting:
                failures = [] if sample.get("position") == -1 else [
                    f"the bar holds a value ({sample.get('value')} of {sample.get('max')}) while the engine starts"]
                return {"starting": sample}, failures
            if status.startswith((f"Loading {nextLabel}", "Showing")):
                break
            cdp.Pump(0.1)
        return {"statusesSeen": seen}, [f"the status never said {kStarting!r} between the engine's download and "
                                        f"{nextLabel}'s (it read {[sample.get('status') for sample in seen]})"]
    return Check


# The download bar's box on screen and what covers its middle, with the panel's state and its title.
kBarOnScreen = """(() => {
    const bar = document.querySelector('progress.download'); const box = bar.getBoundingClientRect();
    const middle = document.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2);
    return { box: [box.left, box.top, box.width, box.height].map(Math.round), viewport: [innerWidth, innerHeight],
             onTop: middle === bar, panelOpen: document.querySelector('.panel').open,
             summary: document.querySelector('.panel summary').textContent.trim() };
})()"""


def BarOnScreen(cdp: browser_gate.Cdp, path: Path):
    """A `between` for ProgressSamples: the bar is inside the viewport, spans it, is not covered,
    and the closed panel's title names and counts the download."""
    def Check():
        shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
        geometry = Evaluate(cdp, kBarOnScreen) or {}
        path.write_bytes(base64.b64decode(shot))
        left, top, width, height = geometry.get("box") or (0, 0, 0, 0)
        viewWidth, viewHeight = geometry.get("viewport") or (0, 0)
        failures = []
        if not (top >= 0 and top + height <= viewHeight and height >= 3 and left <= 0 and width >= viewWidth):
            failures.append(f"the download bar is not across the viewport: box {geometry.get('box')} in {geometry.get('viewport')}")
        if not geometry.get("onTop"):
            failures.append("something covers the download bar")
        if not re.search(r"· Loading the engine \d+%$", geometry.get("summary") or ""):
            failures.append(f"the panel's title {geometry.get('summary')!r} does not name and count the engine's download")
        return {"barFrame": path.name, "barGeometry": geometry}, failures
    return Check


def ServiceWorkers(cdp: browser_gate.Cdp) -> list[browser_gate.Cdp]:
    """The browser's service workers, each on a CDP connection of its own: a service worker
    fetches for the page it controls (the coi worker at pages), and the page's network conditions
    do not reach those requests."""
    host, port = cdp.socket.getpeername()[:2]
    with urllib.request.urlopen(f"http://{host}:{port}/json/list") as reply:
        targets = json.load(reply)
    return [browser_gate.Cdp(target["webSocketDebuggerUrl"]) for target in targets
            if target.get("type") == "service_worker"]


def SetLink(connections: list[browser_gate.Cdp], link: dict) -> None:
    for connection in connections:
        connection.Send("Network.enable")
        connection.Send("Network.setCacheDisabled", cacheDisabled=link is not kUnthrottled)
        connection.Send("Network.emulateNetworkConditions", **link)


def ThrottledStep(cdp: browser_gate.Cdp, name: str, rule: str, act, downloads, shown: str,
                  seconds: float, out: Path) -> dict:
    """Runs `act` on a cold ~20 Mbit/s link, samples each of `downloads` ((label, between, link)
    triples, in the order the page downloads them) twice, each on its link, lifts the throttle
    and records the step once the page shows `shown`."""
    sampled = []

    def Throttled():
        workers = ServiceWorkers(cdp)
        SetLink([cdp, *workers], kThrottled)
        act()
        current = kThrottled
        for label, between, link in downloads:
            if link is not current:
                SetLink([cdp, *workers], link)
                current = link
            sampled.append(ProgressSamples(cdp, label, seconds, between))
        SetLink([cdp, *workers], kUnthrottled)
        for worker in workers:
            worker.socket.close()
    result = ViewerStep(cdp, name, rule, Throttled, kStatus, shown, seconds, out)
    result["progress"] = sampled
    result["failures"] += [failure for download in sampled for failure in download["failures"]]
    result["pass"] = not result["failures"]
    return result


def PickSample(cdp: browser_gate.Cdp, index: int) -> None:
    Evaluate(cdp, "(() => { const list = document.querySelector('.panel [name=model]');"
                  f" list.selectedIndex = {index}; list.dispatchEvent(new Event('change')); }})()")


def SetLocalFile(cdp: browser_gate.Cdp, path: Path) -> None:
    root = cdp.Send("DOM.getDocument")["result"]["root"]["nodeId"]
    node = cdp.Send("DOM.querySelector", nodeId=root, selector=".panel input[type=file]")
    cdp.Send("DOM.setFileInputFiles", nodeId=node["result"]["nodeId"], files=[str(path)])


def PressF(cdp: browser_gate.Cdp) -> None:
    """A key event delivered to the page over CDP, not to the desktop."""
    cdp.Send("Input.dispatchKeyEvent", type="keyDown", code="KeyF", key="f", text="f",
             windowsVirtualKeyCode=70)
    cdp.Send("Input.dispatchKeyEvent", type="keyUp", code="KeyF", key="f", windowsVirtualKeyCode=70)


# How the model sits in the canvas area the panel and the overlays leave free, seen through the
# camera that draws it (its position and rotation; it looks along its +Z). Two shapes are
# projected: the model's own box (its eight corners) and the hull the page fits, which holds the
# model at every turn about world up (the circle its box sweeps, at the box's bottom and top,
# kHullPoints points each, about the box's centre, which the page's turntable turns the model
# about). In MODE 'fit' (a model at rest, as the page framed it): the model's box as it now stands
# lands with its middle within kFramedCentreTolerance px of the free area's middle; in MODE 'turning'
# (the turntable on) the hull's middle, which stays put as the model turns, does. In both the
# model's box lies inside the hull (kBoxInHull), the hull lies inside the free
# area and clear of the panel and every overlay, and the hull is tight: on its limiting side it
# reaches at least kMinTightness of the margin-shrunk half area (the page's fit stops there). In
# MODE 'turned' (a model turned in place after the fit): the model's box lies inside the free area
# and clear of the panel and the overlays. The free area is the page's rule: the panel's column or
# strip (a narrow page's closed title strip too), and each shown overlay (the readout only where
# the overlays column runs across a narrow page's top) taking its top or its bottom by the half it
# sits in. Every measure is written to globalThis.framing, pass or fail. 'framed', or what is wrong.
kFramedCentreTolerance = 8
# The Fox's turns in place for the turned steps: the hull's widest and the model end-on.
kTurnedYaws = (45, 90, 135, 180, 225, 270, 315)
kMinTightness = 0.95
kHullPoints = 16
kFitMargin = 1.15
# The page's gap between an overlay (or a closed title strip) and the free area, in CSS pixels
# (examples/shared/page-status.js kOverlayGap).
kOverlayGap = 8
# Whether a projected box lies inside the projected hull: the hull's sixteen points are a polygon
# inscribed in the circle the box sweeps, so a box corner between two of them may stand out by up
# to 1 - cos(pi / kHullPoints) of the hull's half width; one pixel more for rounding.
kBoxInHull = """((box, hull) => {
    const slack = 1 + ((hull.right - hull.left) / 2) * (1 - Math.cos(Math.PI / HULL_POINTS));
    return box.left >= hull.left - slack && box.right <= hull.right + slack
        && box.top >= hull.top - slack && box.bottom <= hull.bottom + slack;
})""".replace("HULL_POINTS", str(kHullPoints))
kModelFramedIn = """(async (mode) => {
    const { Camera } = await import('@openengine/web');
    const v = globalThis.viewer; const model = v?.model();
    if (!model) return 'no model';
    // The box the page fits: the model's box as shown (before any turn), which the page keeps (a
    // skinned model's live bounds move with its clip). The model's own box now is that box turned
    // about its centre by the model's turn since it was shown (its rotation against the shape's rest).
    const shown = v.shape?.() ?? model.bounds; const { center, size } = shown;
    const r = shown.rest ?? { x: 0, y: 0, z: 0, w: 1 }, m = model.transform.quaternion;
    const yaw = (2 * Math.atan2(-m.w * r.y + m.x * r.z + m.y * r.w - m.z * r.x, m.w * r.w + m.x * r.x + m.y * r.y + m.z * r.z) * 180) / Math.PI;
    const loaded = { center, size };
    const camera = v.engine.scene.camera.transform; const eye = camera.position; const q = camera.quaternion;
    const tanHalf = Math.tan(((v.engine.scene.camera.get(Camera)?.fovY ?? 60) * Math.PI) / 360);
    const forward = [2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)];
    const across = Math.hypot(forward[2], forward[0]) || 1;
    const right = [forward[2] / across, 0, -forward[0] / across];
    const up = [forward[1] * right[2] - forward[2] * right[1], forward[2] * right[0] - forward[0] * right[2],
                forward[0] * right[1] - forward[1] * right[0]];
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const canvas = document.querySelector('canvas'); const width = canvas.clientWidth, height = canvas.clientHeight;
    const project = (p) => {
        const rel = [p[0] - eye.x, p[1] - eye.y, p[2] - eye.z]; const depth = dot(rel, forward);
        return [width / 2 + (dot(rel, right) / (depth * tanHalf)) * (height / 2), height / 2 - (dot(rel, up) / (depth * tanHalf)) * (height / 2)];
    };
    const boxOf = (points) => {
        const projected = points.map(project);
        return { left: Math.min(...projected.map((c) => c[0])), right: Math.max(...projected.map((c) => c[0])),
                 top: Math.min(...projected.map((c) => c[1])), bottom: Math.max(...projected.map((c) => c[1])) };
    };
    const corners = []; const a = (yaw * Math.PI) / 180;
    for (const sx of [-0.5, 0.5]) for (const sy of [-0.5, 0.5]) for (const sz of [-0.5, 0.5]) {
        const x = sx * size.x, z = sz * size.z;
        corners.push([center.x + x * Math.cos(a) + z * Math.sin(a), center.y + sy * size.y, center.z - x * Math.sin(a) + z * Math.cos(a)]);
    }
    const sweep = 0.5 * Math.hypot(loaded.size.x, loaded.size.z); const hullPoints = [];
    for (let i = 0; i < HULL_POINTS; ++i) {
        const angle = (2 * Math.PI * i) / HULL_POINTS;
        for (const sy of [-0.5, 0.5])
            hullPoints.push([loaded.center.x + sweep * Math.cos(angle), loaded.center.y + sy * loaded.size.y,
                             loaded.center.z + sweep * Math.sin(angle)]);
    }
    const modelBox = boxOf(corners), hull = boxOf(hullPoints);
    let freeTop = 0, freeBottom = height, freeRight = width;
    const obstacles = [];
    const details = document.querySelector('.panel'); const card = details.getBoundingClientRect();
    if (details.open) {
        if (card.left > width / 2) freeRight = card.left; else freeBottom = card.top;
        obstacles.push(['the panel', card]);
    } else if (card.top > height / 2 && card.left < width / 2) {
        freeBottom = card.top - OVERLAY_GAP;
        obstacles.push(['the panel', card]);
    }
    const acrossTop = document.querySelector('.overlays')?.getBoundingClientRect().bottom < height / 2;
    for (const [name, selector] of [['the credit', '.credit'], ['the frame-rate readout', '.fps']]) {
        const element = document.querySelector(selector);
        if (!element || element.hidden) continue;
        const rect = element.getBoundingClientRect();
        obstacles.push([name, rect]);
        if (selector === '.fps' && !acrossTop) continue;   // a wide page's corner readout: an obstacle only
        if (rect.bottom < height / 2) freeTop = Math.max(freeTop, rect.bottom + OVERLAY_GAP);
        else freeBottom = Math.min(freeBottom, rect.top - OVERLAY_GAP);
    }
    const middle = [freeRight / 2, (freeTop + freeBottom) / 2];
    const freeWidth = freeRight, freeHeight = freeBottom - freeTop;
    const measure = (box) => ({
        centreOffset: [Math.round(((box.left + box.right) / 2 - middle[0]) * 10) / 10, Math.round(((box.top + box.bottom) / 2 - middle[1]) * 10) / 10],
        fill: [Math.round(((box.right - box.left) / freeWidth) * 1000) / 1000, Math.round(((box.bottom - box.top) / freeHeight) * 1000) / 1000],
        box: [box.left, box.top, box.right, box.bottom].map(Math.round) });
    const halfWidth = freeWidth / (2 * FIT_MARGIN), halfHeight = freeHeight / (2 * FIT_MARGIN);
    const tightness = Math.max((middle[0] - hull.left) / halfWidth, (hull.right - middle[0]) / halfWidth,
                               (middle[1] - hull.top) / halfHeight, (hull.bottom - middle[1]) / halfHeight);
    globalThis.framing = { mode, free: [0, freeTop, freeRight, freeBottom].map(Math.round), model: measure(modelBox),
                           hull: { ...measure(hull), tightness: Math.round(tightness * 1000) / 1000 } };
    const problems = [];
    const inside = (box) => box.left >= -1 && box.right <= freeRight + 1 && box.top >= freeTop - 1 && box.bottom <= freeBottom + 1;
    const clear = (box, what) => {
        for (const [name, rect] of obstacles) {
            const overlapX = Math.min(box.right, rect.right) - Math.max(box.left, rect.left);
            const overlapY = Math.min(box.bottom, rect.bottom) - Math.max(box.top, rect.top);
            if (overlapX > 1 && overlapY > 1) problems.push(`${what} reaches ${name}`);
        }
    };
    if (mode === 'fit' || mode === 'turning') {
        const [centred, name] = mode === 'fit' ? [globalThis.framing.model, "the model's box"] : [globalThis.framing.hull, 'the hull'];
        const off = Math.hypot(...centred.centreOffset);
        if (off > TOLERANCE) problems.push(`${name}'s centre is ${Math.round(off)} px from the free area's middle`);
        if (!inside(hull)) problems.push('the hull leaves the free area');
        if (!BOX_IN_HULL(modelBox, hull)) problems.push("the model's box leaves the hull it sweeps");
        clear(hull, 'the hull');
        if (tightness < MIN_TIGHTNESS) problems.push(`the hull reaches ${Math.round(tightness * 100)}% of its margin: not the fit`);
    } else {
        if (!inside(modelBox)) problems.push("the model's box leaves the free area");
        clear(modelBox, "the model's box");
    }
    return problems.length ? problems.join('; ') : 'framed';
})(MODE)""".replace("TOLERANCE", str(kFramedCentreTolerance)).replace("MIN_TIGHTNESS", str(kMinTightness)) \
    .replace("HULL_POINTS", str(kHullPoints)).replace("FIT_MARGIN", str(kFitMargin)) \
    .replace("OVERLAY_GAP", str(kOverlayGap)).replace("BOX_IN_HULL", kBoxInHull)
kModelFramed = kModelFramedIn.replace("MODE", "'fit'")
kModelTurned = kModelFramedIn.replace("MODE", "'turned'")
kModelTurning = kModelFramedIn.replace("MODE", "'turning'")
# The narrow viewports the page is checked at: a small tablet and a phone, in CSS pixels.
# Every model as the page shows it: kModelReach metres on its box's longest side
# (examples/shared/page-status.js), standing as loaded (its rotation a turn about world up only,
# the turntable's), with the orbit at its starting elevation: kModelPitch for the models the page's
# sample table gives one (model-viewer/panel.js), the page's own start for the rest; and the model
# at its starting turn (kModelYaw, 0 for the rest), the turntable held from the landing on.
kModelReach = 2
kModelPitch = {"Damaged helmet": -20}
kModelYaw = {"Damaged helmet": 35}
kPitchTolerance = 0.05
kShownStandard = """(() => {
    const v = globalThis.viewer, s = v.shape(), q = v.model().transform.quaternion;
    const problems = [], reach = Math.max(s.size.x, s.size.y, s.size.z), pitch = PITCH_NOW;
    if (Math.abs(reach - REACH) > 1e-3 * REACH) problems.push(`the model's box is ${reach.toFixed(3)} m on its longest side, not REACH`);
    if (Math.abs(q.x) > 1e-4 || Math.abs(q.z) > 1e-4) problems.push(`the model is tilted (quaternion x ${q.x.toFixed(4)}, z ${q.z.toFixed(4)})`);
    if (Math.abs(pitch - PITCH) > TOLERANCE) problems.push(`the orbit's elevation is ${pitch.toFixed(2)} degrees, not PITCH`);
    const r = s.rest ?? { x: 0, y: 0, z: 0, w: 1 }, yaw = (2 * Math.atan2(-q.w * r.y + q.x * r.z + q.y * r.w - q.z * r.x, q.w * r.w + q.x * r.x + q.y * r.y + q.z * r.z) * 180) / Math.PI;
    if (Math.abs(yaw - YAW) > TOLERANCE) problems.push(`the model's turn is ${yaw.toFixed(2)} degrees, not YAW`);
    return problems.length ? problems.join('; ') : 'standard';
})()"""


def ShownStandard(pitch: float, yaw: float = 0) -> str:
    """kShownStandard for a model whose starting elevation is `pitch` and starting turn `yaw` degrees."""
    return (kShownStandard.replace("PITCH_NOW", kPitchNow).replace("REACH", str(kModelReach))
            .replace("TOLERANCE", str(kPitchTolerance)).replace("PITCH", f"{pitch:.6f}").replace("YAW", f"{yaw:.6f}"))


kNarrowViewports = ((500, 725), (390, 844))


# Set on the page being left, so that a read after a navigation can tell the new document from it.
kOldDocument = "globalThis.__gateLeavingDocument"
# How long a navigation may take to bring up its new document.
kNavigationSeconds = 30.0


def Navigate(cdp: browser_gate.Cdp, method: str, **params) -> None:
    """Page.navigate or Page.reload, returning once the new document answers: the page being left
    is marked first, so a status read right after this never comes from it."""
    Evaluate(cdp, f"{kOldDocument} = true", strict=False)
    kNavigationMark[id(cdp)] = len(cdp.events)
    cdp.Send(method, **params)
    deadline = time.time() + kNavigationSeconds
    while Evaluate(cdp, f"{kOldDocument} === true", strict=False) is not False and time.time() < deadline:
        cdp.Pump(0.1)


def SetViewport(cdp: browser_gate.Cdp, width: int, height: int) -> None:
    """Emulates a `width` x `height` CSS-pixel viewport and reloads the page into it."""
    cdp.Send("Emulation.setDeviceMetricsOverride", width=width, height=height, deviceScaleFactor=1,
             mobile=False)
    Navigate(cdp, "Page.reload", ignoreCache=False)


def SetTimeOfDay(cdp: browser_gate.Cdp, hours: int) -> None:
    Evaluate(cdp, "(() => { const slider = document.querySelector('.panel [name=time]');"
                  f" slider.value = '{hours}'; slider.dispatchEvent(new Event('input')); }})()")


def RunViewer(cdp: browser_gate.Cdp, origins: Origins, localModel: Path, seconds: float,
              out: Path, build: str, siteUrl: str | None = None) -> list[dict]:
    """The viewer's steps at `build`:
      mt     the isolated origin (COOP and COEP headers);
      st     the plain origin with the coi service worker blocked, so the page stays unisolated;
      pages  the plain origin with the service worker allowed, as GitHub Pages serves the site: the
             worker isolates the page after one reload and its scope must cover the engine module,
             whose folder the threaded build starts its workers from."""
    if build == "st":
        cdp.Send("Network.enable")
        cdp.Send("Network.setBlockedURLs", urls=["*coi-serviceworker.js"])
    url = (f"{siteUrl.rstrip('/')}/{kViewerPage}" if siteUrl
           else f"{origins.Url('isolated' if build == 'mt' else 'page')}/{kViewerPage}")
    def OpenWithTurntableOff():
        Navigate(cdp, "Page.navigate", url=url)
        WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)
        Evaluate(cdp, kRestAtYawZero)
    # The landing: the page's first load, cold, captured without a second load of the model.
    results = [ViewerStep(cdp, "viewer-default",
                          "the page opens on its default model, framed in the middle of the area the panel leaves free, at the steps' exposure compensation",
                          OpenWithTurntableOff, f"(async () => {kStatus} + ' | ' + (await {kModelFramed}) + ' | EV ' + {kExposureNow})()",
                          f"Showing {kDefaultModel} | framed | EV {kViewerExposureEv}", seconds, out, pose=kAnimationPose,
                          record="globalThis.framing")]
    labels = Evaluate(cdp, "[...document.querySelectorAll('.panel [name=model] option')].map(o => o.dataset.name)") or []
    startPitch = Evaluate(cdp, kPitchNow)   # the page's own starting elevation, which the landing model keeps
    results.append(PlaysOnLoad(cdp, ViewerStep(
        cdp, "viewer-animation-clips",
        "the Fox's Animation section lists the clips the engine names and plays the first, its clip time "
        "advancing; the panel's title names it",
        lambda: None, kAnimationState,
        f"enabled | no note | clips {', '.join(kFoxClips)} | Model viewer · {kFoxClips[0]}",
        seconds, out, pose=kAnimationPose)))
    results.append(ViewerStep(cdp, "viewer-credit",
                              "the open panel fits without scrolling, its Pause button uncovered, and the Fox's "
                              "credit is whole over the canvas, clear of the panel",
                              lambda: None, kPanelAndCredit, "clear", seconds, out, pose=kAnimationPose))
    results.append(ViewerStep(cdp, "viewer-zoom-kept",
                              "the visitor's zoom survives F (the readout on and off) and the panel closing and "
                              "reopening",
                              lambda: ZoomKept(cdp), kZoomKept, "kept", seconds, out,
                              record="(({ target, ...numbers }) => numbers)(globalThis.zoomKept)"))
    # The Fox turned in place on the fit (the turntable held): the hull holds it at every turn.
    for yaw in kTurnedYaws:
        results.append(ViewerStep(cdp, f"viewer-turned-{yaw}",
                                  f"the Fox turned {yaw} degrees by the turntable stays inside the free area, clear of "
                                  "the panel and the credit (a revolution in 45-degree steps; the record tracks its box)",
                                  lambda yaw=yaw: Evaluate(cdp, kTurnTo.replace("YAW", str(yaw))),
                                  kModelTurned, "framed", seconds, out, pose=kAnimationPose, record="globalThis.framing"))
    results.append(ViewerStep(cdp, "viewer-reframe-turned",
                              "a re-frame with the Fox turned 45 degrees (the panel closed and reopened) centres the "
                              "turned model's box, the hull it sweeps at the fit (the record keeps the orbit at yaw 0 and 45)",
                              lambda: ReframeTurned(cdp), kModelFramed, "framed", seconds, out,
                              pose=kAnimationPose, record="globalThis.reframeTurned"))
    Evaluate(cdp, kTurnTo.replace("YAW", "0"))
    results += ElevationPair(cdp, seconds, out)
    # Speed 0: the clip holds its pose; the panel and the closed title show what plays.
    results.append(ViewerStep(cdp, "viewer-speed-zero",
                              "speed 0 holds the clip's pose; the speed reads 0.00x",
                              lambda: Evaluate(cdp, "(() => { const s = document.querySelector('.panel [name=speed]'); "
                                                    "s.value = '0'; s.dispatchEvent(new Event('input')); })()"),
                              "document.querySelector('.panel [name=rate]').value", "0.00×", seconds, out,
                              pose=kAnimationPose, record=kAnimationState))
    Evaluate(cdp, "(() => { const s = document.querySelector('.panel [name=speed]'); "
                  "s.value = '1'; s.dispatchEvent(new Event('input')); })()")
    results.append(AnimationMotion(cdp, seconds, out))
    results.append(ViewerStep(cdp, "viewer-turntable-toggle",
                              "switching the turntable on and off again starts and stops the turn where the model "
                              "stands: the orbit holds and the Fox's centre stays within a pixel on screen",
                              lambda: TurntableToggle(cdp), kTurntableToggled, "kept", seconds, out,
                              pose=kAnimationPose, record="globalThis.turntableToggle"))
    results.append(ViewerStep(cdp, "viewer-turntable-live",
                              f"from a cold load, with the clip paused, the page's own turntable turns the Fox at least "
                              f"{kTurnDegrees} degrees about its box's centre: the live box keeps the recorded box's centre and "
                              "is the recorded box turned by the model's yaw (the box the framing checks project)",
                              lambda: TurntableLive(cdp, url, seconds), kTurnedAboutCentre, "about its centre", seconds, out,
                              pose=kAnimationPose, record="globalThis.turntableLive"))
    results.append(ViewerStep(cdp, "viewer-animation-none",
                              "a model without clips leaves the Animation section disabled, its note in place of "
                              "the clip list, and the open panel still fits without scrolling",
                              lambda: PickSample(cdp, labels.index(kStillModel)),
                              f"({kStatus} === 'Showing {kStillModel}') ? {kAnimationState} + ' | ' + {kPanelFitsAnyModel} : {kStatus}",
                              f"disabled | {kNoAnimations} | clips none | Model viewer | clear", seconds, out,
                              pose=kAnimationPose))

    # After the first visit, so that at pages the service worker's reload is behind us.
    def OpenCold():
        Navigate(cdp, "Page.navigate", url=url)
    results.append(ThrottledStep(cdp, "viewer-progress-engine",
                                 "on a slow link the status counts the engine's download, says the engine is starting, "
                                 "then counts the first model's, each bar advancing, and the sky draws while the model downloads",
                                 OpenCold, [("the engine", StartingAfterDownload(cdp, labels[0], seconds), kThrottled),
                                            (labels[0], SkyBeforeModel(cdp, out / "viewer-progress-engine-sky.png"), kSlowLink)],
                                 f"Showing {labels[0]}", seconds, out))
    results.append(ThrottledStep(cdp, "viewer-progress-model",
                                 "on a slow link a picked model has an advancing percentage or a sweeping bar while its size is unknown",
                                 lambda: PickSample(cdp, 1),
                                 [(labels[1], OverlaysWhileLoading(cdp, out / "viewer-progress-model-mid.png"), kThrottled)],
                                 f"Showing {labels[1]}", seconds, out))
    for index, label in enumerate(labels[1:], start=1):
        results.append(ViewerStep(cdp, f"viewer-pick-{index}",
                                  "a listed sample model loads by URL and replaces the shown one, framed in the middle of "
                                  "the area the panel leaves free, at the standard size, upright, the orbit at its starting "
                                  "elevation; the open panel fits without scrolling",
                                  lambda index=index: PickSample(cdp, index),
                                  f"(async () => {kStatus} + ' | ' + {kPanelFitsAnyModel} + ' | ' + (await {kModelFramed}) + ' | ' + "
                                  f"{ShownStandard(kModelPitch.get(label, startPitch), kModelYaw.get(label, 0))})()",
                                  f"Showing {label} | clear | framed | standard", seconds, out,
                                  record=f"({{ framing: globalThis.framing, pitch: {kPitchNow} }})"))
    shownAndListed = f"{kStatus} + ' | ' + document.querySelector('.panel [name=model]').selectedOptions[0].text"
    results.append(ViewerStep(cdp, "viewer-local-file",
                              "a local .glb opens through the file input as an object URL, and the list names it",
                              lambda: SetLocalFile(cdp, localModel), shownAndListed,
                              f"Showing {kLocalModelName} | Your file: {kLocalModelName}", seconds, out))

    def ReopenSameFile():
        Evaluate(cdp, "document.querySelector('.panel output[name=status]').value = 'reopening'")
        SetLocalFile(cdp, localModel)
    results.append(ViewerStep(cdp, "viewer-local-file-again", "choosing the same file again loads it again",
                              ReopenSameFile, shownAndListed,
                              f"Showing {kLocalModelName} | Your file: {kLocalModelName}", seconds, out))
    results.append(GiStep(cdp, url, localModel, seconds, out))
    results.append(ViewerStep(cdp, "viewer-fps-key", "F shows the frame-rate readout",
                              lambda: PressF(cdp),
                              "(() => { const r = document.querySelector('.fps');"
                              " return !r.hidden && / fps · worst [0-9.]+ ms$/.test(r.textContent)"
                              " ? 'shown' : r.textContent; })()", "shown", 10.0, out))
    for end in ("min", "max"):
        hours = int(float(Evaluate(cdp, f"document.querySelector('.panel [name=time]').{end}")))
        results.append(ViewerStep(cdp, f"viewer-time-{end}",
                                  "the slider's end is a lit hour: the model shows at it",
                                  lambda hours=hours: SetTimeOfDay(cdp, hours),
                                  "document.querySelector('.panel output[name=clock]').value",
                                  f"{hours}:00", 10.0, out))
    SetTimeOfDay(cdp, 15)
    flight = labels.index(kClosestZoomModel)
    results.append(ViewerStep(cdp, "viewer-closest-zoom",
                              "at the closest zoom the model still fills the frame",
                              lambda: (PickSample(cdp, flight), WaitFor(cdp, kStatus, f"Showing {kClosestZoomModel}", seconds),
                                       Evaluate(cdp, kClosestZoom)),
                              kStatus, f"Showing {kClosestZoomModel}", seconds, out))
    for width, height in kNarrowViewports:
        results.append(ViewerStep(cdp, f"viewer-narrow-{width}x{height}",
                                  "on a narrow page the model is framed in the middle of the free area, clear of the overlays",
                                  lambda width=width, height=height: (
                                      SetViewport(cdp, width, height),
                                      WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds),
                                      Evaluate(cdp, kRestAtYawZero)),
                                  kModelFramed, "framed", seconds, out, record="globalThis.framing"))
    # At the phone's viewport, panel opened: the page re-frames the model above it.
    phone = f"{kNarrowViewports[-1][0]}x{kNarrowViewports[-1][1]}"
    results.append(ViewerStep(cdp, f"viewer-credit-{phone}",
                              "on a phone-sized page the opened strip takes at most half the page and scrolls with its "
                              "scroll bar showing, the Animation section's Pause button inside it, the Fox's credit "
                              "whole over the canvas, clear of the strip, and the model re-framed above the strip",
                              lambda: Evaluate(cdp, "document.querySelector('.panel').open = true"),
                              f"(async () => {kPanelAndCredit} + ' | ' + (await {kModelFramed}))()", "clear | framed", seconds, out,
                              pose=kAnimationPose, record="globalThis.framing"))
    results.append(ViewerStep(cdp, f"viewer-fps-{phone}",
                              "on a phone-sized page the frame-rate readout shows under the credit, clear of it and of "
                              "the open panel, and the model is re-framed clear of both",
                              lambda: (PressF(cdp), WaitFor(cdp, "!document.querySelector('.fps').hidden && "
                                                                 "/ fps/.test(document.querySelector('.fps').textContent)", True, 10.0)),
                              f"(async () => {kPanelAndCredit} + ' | ' + (await {kModelFramed}))()", "clear | framed", seconds, out,
                              pose=kAnimationPose, record="globalThis.framing"))
    PressF(cdp)
    # Still at the last narrow viewport; the next navigation starts the panel closed.
    results.append(ThrottledStep(cdp, f"viewer-progress-narrow-{kNarrowViewports[-1][0]}x{kNarrowViewports[-1][1]}",
                                 "on a phone-sized page, panel closed, the download bar is on screen and the panel's title names and counts the download",
                                 OpenCold, [("the engine", BarOnScreen(cdp, out / "viewer-progress-narrow-bar.png"), kThrottled)],
                                 f"Showing {labels[0]}", seconds, out))
    short = f"{kShortViewport[0]}x{kShortViewport[1]}"
    for opened in ("open", "closed"):
        results.append(ViewerStep(cdp, f"viewer-short-{short}-{opened}",
                                  f"on a short narrow page (a phone in landscape), panel {opened}, the orbit is finite "
                                  "and the model framed in the room the strip and the credit leave",
                                  lambda opened=opened: ShortPage(cdp, seconds, opened == "open"),
                                  f"(async () => {kShortPageFinite} + ' | ' + (await {kModelFramed}))()", "finite | framed",
                                  seconds, out, drawsModel=False, pose=kAnimationPose,
                                  record="({ orbit: globalThis.shortPage, framing: globalThis.framing, panel: "
                                         "(({ left, top, right, bottom }) => [left, top, right, bottom].map(Math.round))"
                                         "(document.querySelector('.panel').getBoundingClientRect()) })"))
    noRoom = f"{kNoRoomViewport[0]}x{kNoRoomViewport[1]}"
    results.append(ViewerStep(cdp, f"viewer-short-{noRoom}-open",
                              "on a page too short for any room beside the open strip and the credit, the page frames "
                              "in the whole canvas and the orbit stays finite",
                              lambda: ShortPage(cdp, seconds, True, kNoRoomViewport), kShortPageFinite, "finite",
                              seconds, out, drawsModel=False, pose=kAnimationPose,
                              record="({ orbit: globalThis.shortPage, panel: "
                                     "(({ left, top, right, bottom }) => [left, top, right, bottom].map(Math.round))"
                                     "(document.querySelector('.panel').getBoundingClientRect()) })"))
    cdp.Send("Emulation.clearDeviceMetricsOverride")
    Navigate(cdp, "Page.reload", ignoreCache=False)
    WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)
    Evaluate(cdp, kHoldPose)

    # A refused file leaves its reason in the status until the next action that succeeds.
    refusedGltf = localModel.with_name(kExternalGltfName)
    results.append(ViewerStep(cdp, "viewer-refusal-then-time",
                              "a refusal's message gives way to the model's name on the next action",
                              lambda: (SetLocalFile(cdp, refusedGltf),
                                       WaitFor(cdp, f"({kStatus} || '').includes('refers to files beside it')", True, seconds),
                                       SetTimeOfDay(cdp, 12)),
                              kStatus, f"Showing {kDefaultModel}", seconds, out))

    broken = localModel.with_name(kBrokenModelName)
    refusedAndListed = (f"(({kStatus} || '').startsWith('Could not load {kBrokenModelName}') ? 'refused' : {kStatus})"
                        " + ' | ' + document.querySelector('.panel [name=model]').selectedOptions[0].text")
    results.append(ViewerStep(cdp, "viewer-local-file-broken",
                              "a file that does not load says so, and the list goes back to the model on screen",
                              lambda: (PickSample(cdp, 0), WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds),
                                       SetLocalFile(cdp, broken)),
                              refusedAndListed, f"refused | {kDefaultModel}", seconds, out))
    results.append(ViewerStep(cdp, "viewer-after-broken", "the model picked after a file that did not load loads",
                              lambda: PickSample(cdp, 1), kStatus, f"Showing {labels[1]}", seconds, out))
    return results


kInstancingPage = "package/examples/instancing/index.html"
# The instancing page shows its turning switch only with this query flag.
kTurntableFlag = "?turntable=1"
kInstanceCount = 100_000
# The copies the engine holds: the page's entities that carry a MeshRenderer, asked of the engine.
kCountInstances = """(async () => {
    const { MeshRenderer } = await import('@openengine/web');
    return globalThis.demo?.instances.filter((entity) => entity.has(MeshRenderer)).length ?? null;
})()"""
kSetInstanceCount = """(() => { const slider = document.querySelector('.panel [name=count]');
    slider.value = 'COUNT'; slider.dispatchEvent(new Event('input')); })()"""
# Without the flag: whether the turning switch is on the page, whether the copies turn (the page's
# own state), and whether the panel's text mentions turning.
kTurntableAbsent = """(() => ({ control: !!document.querySelector('.panel [name=turn]'),
    turning: globalThis.demo?.turning ?? null, mentions: /turn/i.test(document.querySelector('.panel').textContent) }))()"""
# The pose of the instancing page: the copies on the page, whether the turning switch is on the
# page and whether the copies turn (the page's own state), the camera's position and rotation
# (degrees), the orbit, the viewport.
kDemoPose = """(() => {
    const d = globalThis.demo; const c = d?.engine.scene.camera.transform;
    const xyz = (p) => p && [p.x, p.y, p.z].map((n) => Math.round(n * 1000) / 1000);
    return { count: d?.instances.length ?? null, control: !!document.querySelector('.panel [name=turn]'),
             turning: d?.turning ?? null,
             cameraPosition: xyz(c?.position), cameraRotation: xyz(c?.rotation),
             target: xyz(d?.controls.target), distance: d && Math.round(d.controls.distance * 1000) / 1000,
             viewport: [innerWidth, innerHeight], devicePixelRatio };
})()"""
# The pose of an animation capture: the clip and speed the panel set, the panel's title, the clip
# time of the model or the first part under it that carries one (AnimatorRef, reflected; a model
# with one skinned mesh carries it on its root) or why it could not be read, and kReadPose's orbit,
# camera, model rotation, time of day, exposure compensation and viewport.
kAnimationPose = """(() => {
    const v = globalThis.viewer; const c = v?.engine.scene.camera.transform; const m = v?.model();
    const xyz = (p) => p && [p.x, p.y, p.z].map((n) => Math.round(n * 1000) / 1000);
    let time = null, timeError = null;
    try {
        for (const queue = [m]; queue.length && time === null;) {
            const part = queue.shift();
            time = part.get({ name: 'AnimatorRef' })?.time ?? null;
            queue.push(...part.transform.children);
        }
    } catch (error) { timeError = String(error?.message ?? error); }
    return { clip: document.querySelector('.panel [name=clip]').value,
             speed: Number(document.querySelector('.panel [name=speed]').value), clipTime: time, timeError,
             title: document.querySelector('.panel summary').textContent.trim(),
             target: xyz(v?.controls.target), distance: v && Math.round(v.controls.distance * 1000) / 1000,
             cameraPosition: xyz(c?.position), cameraRotation: xyz(c?.rotation), modelRotation: xyz(m?.transform.rotation),
             timeOfDay: document.querySelector('.panel output[name=clock]')?.value, exposureCompensation: EXPOSURE,
             viewport: [innerWidth, innerHeight], devicePixelRatio };
})()""".replace("EXPOSURE", kExposureNow)
# The share of the frame (left of the panel, every fourth pixel of every fourth row) whose luma
# differs by more than 16 between two screenshots, in percent.
kFrameChange = """(async () => {
    const decode = async (png) => {
        const image = await createImageBitmap(await (await fetch('data:image/png;base64,' + png)).blob());
        const context = new OffscreenCanvas(image.width, image.height).getContext('2d');
        context.drawImage(image, 0, 0);
        return context.getImageData(0, 0, image.width, image.height);
    };
    const [a, b] = [await decode('PNG_A'), await decode('PNG_B')];
    const right = Math.floor(document.querySelector('.panel').getBoundingClientRect().left);
    const luma = (d, i) => 0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2];
    let changed = 0, sampled = 0;
    for (let y = 0; y < a.height; y += 4) for (let x = 0; x < right; x += 4) {
        const i = (y * a.width + x) * 4;
        ++sampled;
        if (Math.abs(luma(a.data, i) - luma(b.data, i)) > 16) ++changed;
    }
    return Math.round((10000 * changed) / sampled) / 100;
})()"""
# The motion rule: the playing pair changes at least this share of the frame, and this many
# times the paused pair's change (TAA's jitter moves edges in a still frame too).
kMinMotionPercent = 0.1
kMotionOverStill = 3
# The length of kMotionClip in Fox.glb (its samplers' last key, seconds): the playing pair is a
# quarter of it apart, so the pair cannot straddle a whole loop.
kMotionClipSeconds = 1.1583


def OpenDemo(cdp: browser_gate.Cdp, origins: Origins, page: str, build: str, status: str,
             seconds: float) -> None:
    """Navigates to a demo page served as the viewer's `build` is, and waits for its status."""
    if build == "st":
        # Unisolated: the coi service worker would isolate the page after one reload.
        cdp.Send("Network.enable")
        cdp.Send("Network.setBlockedURLs", urls=["*coi-serviceworker.js"])
    Navigate(cdp, "Page.navigate", url=f"{origins.Url('isolated' if build == 'mt' else 'page')}/{page}")
    WaitFor(cdp, f"({kStatus} || '').startsWith({json.dumps(status)})", True, seconds)


def Capture(cdp: browser_gate.Cdp, name: str, out: Path) -> str:
    shot = cdp.Send("Page.captureScreenshot", format="png", _timeout=30.0)["result"]["data"]
    (out / f"{name}.png").write_bytes(base64.b64decode(shot))
    return shot


def PlaysOnLoad(cdp: browser_gate.Cdp, result: dict) -> dict:
    """Adds to a step's `result` the engine's clip time read twice half a second apart: the model
    plays its first clip on load only when the time moves."""
    times = [Evaluate(cdp, kAnimationPose)["clipTime"]]
    cdp.Pump(0.5)
    times.append(Evaluate(cdp, kAnimationPose)["clipTime"])
    result["clipTimes"] = times
    if None in times or times[0] == times[1]:
        result["failures"].append(f"the clip time did not advance after load: {times}")
        result["pass"] = False
    return result


# The visitor's zoom across the page's re-frames, as ZoomKept records it: the orbit distance after
# zooming in, after F twice (the readout on and off), and after closing and reopening the panel;
# and the model's centre on screen, projected through the camera, before the zoom and after the
# toggle (the page keeps it in place at any zoom), within kCentreTolerance CSS pixels.
kCentreTolerance = 2
kZoomKept = """(() => {
    const z = globalThis.zoomKept;
    if (!z) return 'not recorded';
    const same = (a, b) => Math.abs(a - b) <= 1e-4 * Math.abs(b);
    const problems = [];
    if (!same(z.afterF, z.zoomed)) problems.push(`F moved the distance from ${z.zoomed} to ${z.afterF}`);
    if (!same(z.afterToggle, z.zoomed)) problems.push(`closing and reopening the panel moved it from ${z.zoomed} to ${z.afterToggle}`);
    const moved = Math.hypot(z.centreAfter[0] - z.centreBefore[0], z.centreAfter[1] - z.centreBefore[1]);
    if (!(moved <= TOLERANCE)) problems.push(`the model's centre moved ${Math.round(moved)} px on screen`);
    return problems.length ? problems.join('; ') : 'kept';
})()""".replace("TOLERANCE", str(kCentreTolerance))
# The model's centre on screen, projected through the camera that draws it: [x, y] in CSS pixels.
kCentrePixel = """(() => {
    const v = globalThis.viewer; const { center } = v.shape?.() ?? v.model().bounds;   // the box the page centres
    const camera = v.engine.scene.camera.transform; const eye = camera.position; const q = camera.quaternion;
    const tanHalf = Math.tan(((v.engine.scene.camera.get({ name: 'Camera' })?.fovY ?? 60) * Math.PI) / 360);
    const forward = [2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)];
    const across = Math.hypot(forward[2], forward[0]) || 1;
    const right = [forward[2] / across, 0, -forward[0] / across];
    const up = [forward[1] * right[2] - forward[2] * right[1], forward[2] * right[0] - forward[0] * right[2],
                forward[0] * right[1] - forward[1] * right[0]];
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const rel = [center.x - eye.x, center.y - eye.y, center.z - eye.z]; const depth = dot(rel, forward);
    const canvas = document.querySelector('canvas'); const half = canvas.clientHeight / 2;
    return [canvas.clientWidth / 2 + (dot(rel, right) / (depth * tanHalf)) * half, half - (dot(rel, up) / (depth * tanHalf)) * half];
})()"""


kYawZero = "globalThis.viewer.model().transform.rotation.set(0, 0, 0)"
# The page's re-frame at the visitor's zoom: the panel closed and reopened (or opened and closed
# again), each toggle event awaited.
kReframeByPanel = """(async () => {
    const panel = document.querySelector('.panel');
    for (const open of [!panel.open, panel.open]) {
        const toggled = new Promise((resolve) => panel.addEventListener('toggle', resolve, { once: true }));
        panel.open = open;
        await toggled;
    }
})()"""
# The turntable's turn undone and the turntable held, then the model re-framed at rest (the switch
# itself keeps the orbit where it is).
kRestAtYawZero = f"(async () => {{ {kYawZero}; {kHoldPose}; await {kReframeByPanel}; }})()"
# Turns the shown model to YAW degrees the way the page's turntable does, from where it stood at
# yaw 0 (recorded on the first call): its position about its box's centre, then its rotation.
kTurnTo = """(() => {
    const m = globalThis.viewer.model(); const p = m.transform.position;
    if (!globalThis.turnStart || globalThis.turnStart.model !== m.id) {
        const c = globalThis.viewer.shape().center;
        globalThis.turnStart = { model: m.id, position: { x: p.x, y: p.y, z: p.z }, centre: { x: c.x, z: c.z } };
    }
    const { position, centre } = globalThis.turnStart; const a = (YAW * Math.PI) / 180;
    const x = position.x - centre.x, z = position.z - centre.z;
    p.set(centre.x + x * Math.cos(a) + z * Math.sin(a), position.y, centre.z - x * Math.sin(a) + z * Math.cos(a));
    m.transform.rotation.set(0, 0, 0);
    m.transform.rotateWorldY(YAW);   // the turntable's own turn (+Z toward +X), as turnedAbout moves the position
    globalThis.turnStart.yaw = YAW;
})()"""
kOrbitNow = "(() => { const c = globalThis.viewer.controls; return { distance: c.distance, target: [c.target.x, c.target.y, c.target.z] }; })()"
# A small phone in landscape, under the narrow breakpoint; and a page so short that the strip and
# the credit leave no room at all, where the page frames in the whole canvas.
kShortViewport = (568, 320)
kNoRoomViewport = (568, 120)
kShortPageFinite = """(() => {
    const s = globalThis.shortPage;
    if (!s) return 'not recorded';
    const finite = Number.isFinite(s.distance) && s.target.every(Number.isFinite);
    return finite ? 'finite' : `the orbit is ${JSON.stringify(s)}`;
})()"""


# The orbit's pitch, degrees (negative looks down): the page's start (about 9 degrees up) and the
# 17.4 degrees it started at before.
kElevations = (("17", -17.4), ("9", None))
# The orbit's pitch read from the camera and the target (OrbitControls: the camera sits at
# target - look * distance, look.y = sin(pitch)).
kPitchNow = """(() => { const c = globalThis.viewer.controls, p = globalThis.viewer.engine.scene.camera.transform.position;
    return (Math.asin(Math.max(-1, Math.min(1, (c.target.y - p.y) / c.distance))) * 180) / Math.PI; })()"""
# A point on the canvas left of the panel, where a drag orbits; null when something covers it.
kDragPoint = """(() => { const canvas = document.querySelector('canvas'), r = canvas.getBoundingClientRect();
    const x = r.left + r.width / 4, y = r.top + r.height / 2;
    return document.elementFromPoint(x, y) === canvas ? { x, y, height: canvas.clientHeight } : null; })()"""
# Seconds for the orbit's damping to settle after a drag (0.9 per frame at 60 frames a second).
kSettleSeconds = 1.5


def DragPitchTo(cdp: browser_gate.Cdp, pitch: float) -> None:
    """Drags the orbit vertically over CDP (the page's own pointer input, not the desktop's) until
    its pitch is `pitch` degrees: OrbitControls turns 360 degrees per canvas height."""
    point = Evaluate(cdp, kDragPoint)
    if point is None:
        kPageExceptions.append("no canvas point free of the panel and the overlays to drag the orbit from")
        return
    dy = -(pitch - Evaluate(cdp, kPitchNow)) * point["height"] / 360
    x, y = point["x"], point["y"]
    cdp.Send("Input.dispatchMouseEvent", type="mousePressed", x=x, y=y, button="left", buttons=1, clickCount=1)
    cdp.Send("Input.dispatchMouseEvent", type="mouseMoved", x=x, y=y + dy, button="left", buttons=1)
    cdp.Send("Input.dispatchMouseEvent", type="mouseReleased", x=x, y=y + dy, button="left", buttons=0, clickCount=1)
    cdp.Pump(kSettleSeconds)


def ElevationPair(cdp: browser_gate.Cdp, seconds: float, out: Path) -> list[dict]:
    """The Fox at the landing pose at 17.4 degrees up and at the page's start, each framed by the
    page's rule (the panel closed and reopened), with Survey paused at one tick for both. The pitch
    is set by a vertical drag on the canvas, as a visitor orbits; the page's start pitch is read
    before the first drag."""
    Evaluate(cdp, "(() => { const list = document.querySelector('.panel [name=clip]'); list.value = 'Survey'; "
                  "list.dispatchEvent(new Event('change')); })()")
    cdp.Pump(1.0)
    Evaluate(cdp, "document.querySelector('.panel [name=pause]').click()")
    startPitch = Evaluate(cdp, kPitchNow)
    results = []
    for name, pitch in kElevations:
        def Act(pitch=pitch):
            DragPitchTo(cdp, startPitch if pitch is None else pitch)
            for opened in ("false", "true"):
                Evaluate(cdp, f"document.querySelector('.panel').open = {opened}")
                cdp.Pump(0.5)
        results.append(ViewerStep(cdp, f"viewer-elevation-{name}",
                                  f"the Fox at {name} degrees up, framed by the page's rule, Survey held at one tick",
                                  Act, kModelFramed, "framed", seconds, out, pose=kAnimationPose,
                                  record=f"({{ framing: globalThis.framing, pitch: {kPitchNow} }})"))
    Evaluate(cdp, "document.querySelector('.panel [name=pause]').click()")
    return results


# The page's own turntable, sampled while it runs: the model's live box (viewer.model().bounds) every
# kTurnSampleSeconds until it has turned kTurnDegrees (or kTurnLimitSeconds pass), with the
# model's yaw from its quaternion.
kTurnDegrees = 60
kTurnSampleSeconds = 0.5
kTurnLimitSeconds = 20
kTurntableSamples = f"""(async () => {{
    const m = globalThis.viewer.model(), s = globalThis.viewer.shape();
    const yawOf = () => {{ const q = m.transform.quaternion; return (2 * Math.atan2(q.y, q.w) * 180) / Math.PI; }};
    const sample = (yaw) => {{ const b = m.bounds; return {{ yaw, centre: [b.center.x, b.center.y, b.center.z], size: [b.size.x, b.size.y, b.size.z] }}; }};
    let last = yawOf(), turned = 0;
    const samples = [sample(last)], started = performance.now();
    while (turned < {kTurnDegrees} && performance.now() - started < {kTurnLimitSeconds * 1000}) {{
        await new Promise((resolve) => setTimeout(resolve, {kTurnSampleSeconds * 1000}));
        const yaw = yawOf();
        turned += ((((yaw - last) % 360) + 540) % 360) - 180;
        last = yaw;
        samples.push(sample(yaw));
    }}
    globalThis.turntableLive = {{ shape: {{ centre: [s.center.x, s.center.y, s.center.z], size: [s.size.x, s.size.y, s.size.z] }}, turned, samples }};
}})()"""
# The turntable turns the model about its box's centre: every sample's live centre is the recorded
# box's (viewer.shape(), the box the framing checks project), and the live box is the recorded box
# turned by the sample's yaw (the extent along +Y unchanged, along X and Z |cos| and |sin| mixed),
# each within kTurnTolerance of the box's largest side. Writes the worst differences into the record.
kTurnTolerance = 1e-3
kTurnedAboutCentre = f"""(() => {{
    const r = globalThis.turntableLive;
    if (!r) return 'not recorded';
    const [cx, cy, cz] = r.shape.centre, [sx, sy, sz] = r.shape.size;
    const tolerance = {kTurnTolerance} * Math.max(sx, sy, sz);
    let centre = 0, box = 0;
    for (const {{ yaw, centre: c, size }} of r.samples) {{
        const a = (yaw * Math.PI) / 180, cos = Math.abs(Math.cos(a)), sin = Math.abs(Math.sin(a));
        const expected = [cos * sx + sin * sz, sy, sin * sx + cos * sz];
        centre = Math.max(centre, Math.hypot(c[0] - cx, c[1] - cy, c[2] - cz));
        box = Math.max(box, ...size.map((value, i) => Math.abs(value - expected[i])));
    }}
    r.worst = {{ centre, box, tolerance }};
    const problems = [];
    if (r.turned < {kTurnDegrees}) problems.push(`the turntable turned ${{r.turned.toFixed(1)}} degrees in {kTurnLimitSeconds} s, under {kTurnDegrees}`);
    if (centre > tolerance) problems.push(`the live box centre leaves the recorded centre by ${{centre.toFixed(3)}} (tolerance ${{tolerance.toFixed(3)}})`);
    if (r.framing !== 'framed') problems.push(`while it turns: ${{r.framing}}`);
    if (box > tolerance) problems.push(`the live box differs from the recorded box turned by its yaw by ${{box.toFixed(3)}} (tolerance ${{tolerance.toFixed(3)}})`);
    return problems.length ? problems.join('; ') : 'about its centre';
}})()"""


# The turntable switch from rest, as TurntableToggle records it: the orbit (target and distance) and
# the model's centre on screen at rest, kToggleTurnSeconds after the switch is turned on and after it
# is turned off again; and the model's yaw at each, read twice kToggleHoldSeconds apart once off.
# The switch starts and stops the turn where the model stands: the orbit holds (relative 1e-6), the
# centre stays within kToggleTolerance CSS pixels, the model turns while the switch is on and holds
# its turn once it is off.
kToggleTurnSeconds = 1.5
kToggleHoldSeconds = 0.5
kToggleTolerance = 1
kToggleRecord = f"""(() => {{
    const v = globalThis.viewer, m = v.model().transform.quaternion, r = v.shape().rest, t = v.controls.target;
    const yaw = (2 * Math.atan2(-m.w * r.y + m.x * r.z + m.y * r.w - m.z * r.x, m.w * r.w + m.x * r.x + m.y * r.y + m.z * r.z) * 180) / Math.PI;
    return {{ target: [t.x, t.y, t.z], distance: v.controls.distance, centre: {kCentrePixel}, yaw }};
}})()"""
kTurntableToggled = f"""(() => {{
    const r = globalThis.turntableToggle;
    if (!r) return 'not recorded';
    const problems = [];
    const same = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(b));
    for (const name of ['on', 'off']) {{
        const s = r[name];
        if (!same(s.distance, r.rest.distance) || !s.target.every((value, i) => same(value, r.rest.target[i])))
            problems.push(`switching the turntable ${{name}} moved the orbit from ${{JSON.stringify(r.rest)}} to ${{JSON.stringify(s)}}`);
        const moved = Math.hypot(s.centre[0] - r.rest.centre[0], s.centre[1] - r.rest.centre[1]);
        if (!(moved <= {kToggleTolerance})) problems.push(`switching the turntable ${{name}} moved the model's centre ${{moved.toFixed(1)}} px on screen`);
    }}
    if (!(Math.abs(r.on.yaw - r.rest.yaw) >= 1)) problems.push(`the model did not turn while the switch was on (yaw ${{r.rest.yaw.toFixed(2)}} to ${{r.on.yaw.toFixed(2)}})`);
    if (Math.abs(r.held.yaw - r.off.yaw) > 1e-3) problems.push(`the model kept turning after the switch was off (yaw ${{r.off.yaw.toFixed(2)}} to ${{r.held.yaw.toFixed(2)}})`);
    return problems.length ? problems.join('; ') : 'kept';
}})()"""


def TurntableToggle(cdp: browser_gate.Cdp) -> None:
    """From the model at rest: switches the turntable on, then off again, recording the pose at rest,
    after each switch and kToggleHoldSeconds after the last in globalThis.turntableToggle."""
    record = {"rest": Evaluate(cdp, kToggleRecord)}
    for name, checked, seconds in (("on", "true", kToggleTurnSeconds), ("off", "false", 0.0)):
        Evaluate(cdp, "(() => { const turn = document.querySelector('.panel [name=turn]'); "
                      f"turn.checked = {checked}; turn.dispatchEvent(new Event('change')); }})()")
        cdp.Pump(seconds)
        record[name] = Evaluate(cdp, kToggleRecord)
    cdp.Pump(kToggleHoldSeconds)
    record["held"] = Evaluate(cdp, kToggleRecord)
    Evaluate(cdp, f"globalThis.turntableToggle = {json.dumps(record)}")


def TurntableLive(cdp: browser_gate.Cdp, url: str, seconds: float) -> None:
    """From a cold load of `url`: pauses the first clip, leaves the page's turntable on and samples
    the model's live box while it turns (kTurntableSamples), then resumes the clip."""
    Navigate(cdp, "Page.navigate", url=url)
    WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)
    Evaluate(cdp, "document.querySelector('.panel [name=pause]').click()")
    Evaluate(cdp, kTurntableSamples)
    Evaluate(cdp, f"(async () => {{ globalThis.turntableLive.framing = await {kModelTurning}; }})()")
    Evaluate(cdp, "document.querySelector('.panel [name=pause]').click()")


def ReframeTurned(cdp: browser_gate.Cdp) -> None:
    """Records the orbit after a re-frame (the panel closed and reopened) at yaw 0, then after one
    with the model turned 45 degrees, in globalThis.reframeTurned."""
    orbits = {}
    for name, yaw in (("straight", 0), ("turned", 45)):
        Evaluate(cdp, kTurnTo.replace("YAW", str(yaw)))
        for opened in ("false", "true"):
            Evaluate(cdp, f"document.querySelector('.panel').open = {opened}")
            cdp.Pump(0.5)
        orbits[name] = Evaluate(cdp, kOrbitNow)
    Evaluate(cdp, f"globalThis.reframeTurned = {json.dumps(orbits)}")


def ShortPage(cdp: browser_gate.Cdp, seconds: float, opened: bool, viewport=kShortViewport) -> None:
    """At `viewport`, from a cold load (the turntable held, yaw 0): opens the panel, or leaves it
    closed, and records the orbit in globalThis.shortPage."""
    SetViewport(cdp, *viewport)
    WaitFor(cdp, kStatus, f"Showing {kDefaultModel}", seconds)
    Evaluate(cdp, kRestAtYawZero)
    Evaluate(cdp, f"document.querySelector('.panel').open = {'true' if opened else 'false'}")
    cdp.Pump(0.5)
    orbit = Evaluate(cdp, kOrbitNow)
    # JSON has no NaN: a value that is not finite is written as null and reads as not finite.
    Evaluate(cdp, f"globalThis.shortPage = {json.dumps(orbit).replace('NaN', 'null').replace('Infinity', 'null')}")


def ZoomKept(cdp: browser_gate.Cdp) -> None:
    """Zooms the orbit in to 0.6 of its distance, presses F (the readout on), records the distance,
    presses F again, closes and reopens the panel and records it again, with the model's centre on
    screen before the zoom and after the toggle, in globalThis.zoomKept; then restores the fit it started from, so the later steps
    frame as before."""
    distance = "globalThis.viewer.controls.distance"
    centreBefore = Evaluate(cdp, kCentrePixel)
    Evaluate(cdp, f"(() => {{ const t = globalThis.viewer.controls.target; {distance} *= 0.6; "
                  f"globalThis.zoomKept = {{ zoomed: {distance}, target: {{ x: t.x, y: t.y, z: t.z }} }}; }})()")
    PressF(cdp)
    cdp.Pump(0.5)
    Evaluate(cdp, f"globalThis.zoomKept.afterF = {distance}")
    PressF(cdp)
    for opened in ("false", "true"):
        Evaluate(cdp, f"document.querySelector('.panel').open = {opened}")
        cdp.Pump(0.5)
    Evaluate(cdp, f"globalThis.zoomKept.afterToggle = {distance}")
    centreAfter = Evaluate(cdp, kCentrePixel)
    Evaluate(cdp, f"Object.assign(globalThis.zoomKept, {{ centreBefore: {json.dumps(centreBefore)}, "
                  f"centreAfter: {json.dumps(centreAfter)} }})")
    # The later steps' framing: the fit as the page made it before the zoom.
    Evaluate(cdp, f"(() => {{ {distance} = globalThis.zoomKept.zoomed / 0.6; "
                  "globalThis.viewer.controls.target.copy(globalThis.zoomKept.target); })()")
    Evaluate(cdp, f"globalThis.zoomKept.restored = {distance}")


def AnimationMotion(cdp: browser_gate.Cdp, seconds: float, out: Path) -> dict:
    """The default model with kMotionClip picked in the panel: two frames of it playing a moment
    apart, then two paused, each with its pose, clip time and the panel's title."""
    first = len(cdp.events)
    Evaluate(cdp, kHoldPose)
    Evaluate(cdp, "(() => { const list = document.querySelector('.panel [name=clip]');"
                  f" list.value = '{kMotionClip}'; list.dispatchEvent(new Event('change')); }})()")
    cdp.Pump(3.0)
    frames = {}
    for state in ("playing", "paused"):
        if state == "paused":
            Evaluate(cdp, "document.querySelector('.panel [name=pause]').click()")
            cdp.Pump(1.0)
        pair = []
        for index in (1, 2):
            if index == 2:
                cdp.Pump(kMotionClipSeconds / 4)
            name = f"viewer-animation-{state}-{index}"
            shot = Capture(cdp, name, out)
            pose = Evaluate(cdp, kAnimationPose)
            pose["wallSeconds"] = round(time.monotonic(), 3)
            pose["clipSeconds"] = kMotionClipSeconds
            pair.append({"name": name, "shot": shot, "pose": pose})
        for frame in pair:
            frame["frameDetail"] = browser_gate.FrameDetail(cdp, frame["shot"])
        change = Evaluate(cdp, kFrameChange.replace("PNG_A", pair[0]["shot"]).replace("PNG_B", pair[1]["shot"]))
        frames[state] = {"change": change, "frames": [{k: v for k, v in f.items() if k != "shot"} for f in pair]}
    lines = [line for line in map(browser_gate.ConsoleText, cdp.events[first:]) if line]
    (out / "viewer-animation-motion.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    validation = [line for line in lines if any(marker in line for marker in kErrorMarkers)]
    playing, paused = frames["playing"], frames["paused"]
    times = {state: [f["pose"]["clipTime"] for f in frames[state]["frames"]] for state in frames}
    titles = {"playing": f"Model viewer · {kMotionClip}", "paused": f"Model viewer · {kMotionClip}, paused"}
    failures = []
    for state in frames:
        for frame in frames[state]["frames"]:
            if not browser_gate.ShowsModel(frame["frameDetail"]):
                failures.append(f"{frame['name']} is uniform (detail {frame['frameDetail']}): no model drawn")
            if frame["pose"]["title"] != titles[state]:
                failures.append(f"{frame['name']}: the panel's title reads {frame['pose']['title']!r}, expected {titles[state]!r}")
    moving, still = playing["change"], paused["change"]
    if not (isinstance(moving, (int, float)) and moving >= kMinMotionPercent and moving >= kMotionOverStill * (still or 0)):
        failures.append(f"the playing pair changes {moving}% of the frame, the paused pair {still}%: "
                        f"expected at least {kMinMotionPercent}% and {kMotionOverStill} times the paused pair")
    if None in times["playing"] or times["playing"][0] == times["playing"][1]:
        failures.append(f"the clip time did not advance while playing: {times['playing']}")
    walls = [f["pose"]["wallSeconds"] for f in playing["frames"]]
    if walls[1] - walls[0] >= kMotionClipSeconds:
        failures.append(f"the playing pair is {walls[1] - walls[0]:.3f} s apart, a whole {kMotionClipSeconds} s loop or more")
    if None in times["paused"] or times["paused"][0] != times["paused"][1]:
        failures.append(f"the clip time moved while paused: {times['paused']}")
    if validation:
        failures.append(f"{len(validation)} WebGPU validation errors")
    failures += [f"the page threw: {exception}" for exception in kPageExceptions]
    del kPageExceptions[:]
    return {"case": "viewer-animation-motion",
            "rule": f"the default model's {kMotionClip} clip moves it; pausing holds it; the panel's title says which",
            "outcome": {"playing": playing, "paused": paused}, "validationErrors": len(validation),
            "build": PageBuild(cdp), "pass": not failures, "failures": failures}


def RunDemos(cdp: browser_gate.Cdp, origins: Origins, seconds: float, out: Path, build: str) -> list[dict]:
    """The instancing page's steps, served as the viewer's `build` is."""
    count = f"{kInstanceCount:,}"
    withoutFlag = ViewerStep(
        cdp, "instancing-no-turntable",
        "without ?turntable=1 the page has no turning switch, its copies stay at rest and its panel does not mention turning",
        lambda: OpenDemo(cdp, origins, kInstancingPage, build, "1,000 entities", seconds),
        kTurntableAbsent, {"control": False, "turning": False, "mentions": False}, seconds, out, pose=kDemoPose)
    instancing = ViewerStep(
        cdp, f"instancing-{kInstanceCount}", "the slider's top places 100,000 copies, each an entity with a MeshRenderer",
        lambda: (OpenDemo(cdp, origins, kInstancingPage + kTurntableFlag, build, "1,000 entities", seconds),
                 Evaluate(cdp, kSetInstanceCount.replace("COUNT", str(kInstanceCount))),
                 WaitFor(cdp, kStatus + ".startsWith('" + count + " entities')", True, seconds)),
        kCountInstances, kInstanceCount, seconds, out, pose=kDemoPose)
    return [withoutFlag, instancing]


def PackageVersion(package: Path, siteUrl: str | None) -> str:
    """The version of the package the viewer's steps ran: the published site's with --site-url,
    else the local package's."""
    if siteUrl:
        with urllib.request.urlopen(f"{siteUrl.rstrip('/')}/package/package.json", timeout=30) as reply:
            return json.load(reply)["version"]
    return json.loads((package / "package.json").read_text(encoding="utf-8"))["version"]


def StageRoots(package: Path, model: Path, work: Path) -> tuple[Path, Path]:
    """The page root (pages, the package, the bootstrap copy, the model) and the asset root."""
    pageRoot, assetRoot = work / "page", work / "assets"
    shutil.copytree(package, pageRoot / "package")
    (pageRoot / "core").mkdir()
    for name in (*kCoreFiles, *build_web_package.PackFiles(package)):
        shutil.copy2(package / name, pageRoot / "core" / name)
    shutil.copy2(model, pageRoot / model.name)
    # A site package's examples load the models the site hosts from models/ beside the package.
    if (package.parent / "models").is_dir():
        shutil.copytree(package.parent / "models", pageRoot / "models")
    assetRoot.mkdir()
    shutil.copy2(model, assetRoot / model.name)
    return pageRoot, assetRoot


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--module", type=Path, required=True,
                        help="a package directory build_web_package.py wrote")
    parser.add_argument("--model", type=Path, required=True, help="a .glb the pages load")
    parser.add_argument("--out", type=Path, required=True,
                        help="evidence directory: screenshots, transcripts, results.json")
    parser.add_argument("--seconds", type=float, default=20.0, help="seconds pumped per case")
    parser.add_argument("--base-port", type=int, default=8410,
                        help="first of six consecutive local ports the origins use")
    parser.add_argument("--cdp-port", type=int, default=9341)
    parser.add_argument("--case", action="append", default=[], help="run only this origin case")
    parser.add_argument("--expect-fail", action="append", default=[], metavar="CASE=FRAGMENT:REASON",
                        help="a case expected to fail on the modules under test in one known way: "
                             "every one of its failures must contain FRAGMENT, and REASON names the "
                             "fix it needs; it is reported as expected when it fails that way, and "
                             "fails the run when it passes")
    parser.add_argument("--viewer-seconds", type=float, default=90.0,
                        help="seconds a model viewer step may take (the models come over the network)")
    parser.add_argument("--no-viewer", action="store_true", help="skip the model viewer's steps")
    parser.add_argument("--viewer-only", action="store_true", help="run only the model viewer's steps")
    parser.add_argument("--no-demos", action="store_true", help="skip the instancing page's steps")
    parser.add_argument("--demos-only", action="store_true",
                        help="run only the instancing page's steps, served as --viewer-build says")
    parser.add_argument("--site-url",
                        help="run the viewer's steps against a published site instead of the local "
                             "origins (the URL of the site's root, the folder holding package/); "
                             "use with --viewer-only and the --viewer-build the host serves")
    parser.add_argument("--viewer-build", choices=tuple(kExpectedBuild), default="mt",
                        help="how the viewer's page is served: mt (isolating headers), st (no "
                             "isolation), pages (no headers, the coi service worker isolates)")
    args = parser.parse_args()
    expectedFailures = {}
    for entry in args.expect_fail:
        case, _, rest = entry.partition("=")
        fragment, _, reason = rest.partition(":")
        if not (case and fragment and reason):
            parser.error(f"--expect-fail {entry!r} is not CASE=FRAGMENT:REASON")
        expectedFailures[case] = (fragment, reason)

    pagesOnly = args.viewer_only or args.demos_only
    cases = [] if pagesOnly else [case for case in Cases() if not args.case or case.name in args.case]
    if not cases and not pagesOnly:
        parser.error(f"no case named {args.case}")
    args.out.mkdir(parents=True, exist_ok=True)
    origins = Origins(args.base_port)
    work = Path(tempfile.mkdtemp(prefix="ge_package_gate_"))
    servers = []
    chrome = None
    try:
        pageRoot, assetRoot = StageRoots(args.module, args.model, work)
        servers = [Serve(pageRoot, origins.ports["page"], {}),
                   Serve(pageRoot, origins.ports["isolated"], kIsolationHeaders),
                   Serve(args.module, origins.ports["cdn"], {**kCorsHeaders, **kCorpHeaders}),
                   Serve(assetRoot, origins.ports["cors"], kCorsHeaders),
                   Serve(assetRoot, origins.ports["none"], {}),
                   Serve(assetRoot, origins.ports["corp"], kCorpHeaders)]
        chrome = StartChrome(args.cdp_port, str(work / "profile"))
        print(f"web_package_gate: Chrome pid {chrome.pid}")
        cdp = ConnectCdp(args.cdp_port)
        results = []
        for case in cases:
            url = WritePage(case, pageRoot, origins, args.model.name)
            # The chess set comes over the network, as the viewer's samples do.
            seconds = args.viewer_seconds if case.modelOrigin == "chess" else args.seconds
            result = RunCase(cdp, case, url, seconds, args.out)
            results.append(result)
            verdict = "pass" if result["pass"] else "FAIL " + "; ".join(result["failures"])
            print(f"{case.name:22} {verdict}\n    rule: {case.rule}\n    {result['outcome']}")
        pageResults = []
        if not args.no_viewer and not args.demos_only:
            localModel = work / kLocalModelName
            shutil.copy2(args.model, localModel)
            (work / kBrokenModelName).write_bytes(b"glTF but not a model")
            (work / kExternalGltfName).write_text(
                '{"asset": {"version": "2.0"}, "buffers": [{"uri": "External.bin", "byteLength": 4}]}')
            pageResults += RunViewer(cdp, origins, localModel, args.viewer_seconds, args.out,
                                     args.viewer_build, args.site_url)
        if not args.no_demos and not args.viewer_only and not args.site_url:
            pageResults += RunDemos(cdp, origins, args.viewer_seconds, args.out, args.viewer_build)
        if pageResults:
            version = PackageVersion(args.module, args.site_url)
            expectBuild = kExpectedBuild[args.viewer_build]
            for result in pageResults:
                result["packageVersion"] = version
                if expectBuild not in (result["build"] or ""):
                    result["failures"].append(f"the page ran {result['build']!r}, expected the {expectBuild}")
                    result["pass"] = False
                results.append(result)
                verdict = "pass" if result["pass"] else "FAIL " + "; ".join(result["failures"])
                print(f"{result['case']:22} {verdict}\n    rule: {result['rule']}\n    {result['outcome']}")
        # A case the run expects to fail (a module without a fix the case pins) counts as expected
        # only when every one of its failures is the known one; a pass, or any other failure beside
        # it, fails the run.
        for result in results:
            expectation = expectedFailures.get(result["case"])
            if expectation is None:
                continue
            fragment, reason = expectation
            result["expectedFailure"] = {"fragment": fragment, "reason": reason}
            if result["pass"]:
                print(f"web_package_gate: {result['case']} passes; drop its --expect-fail ({reason})")
                result["pass"] = False
            elif all(fragment in failure for failure in result["failures"]):
                print(f"web_package_gate: {result['case']} fails as expected ({fragment}): {reason}")
                result["pass"] = True
            else:
                other = [failure for failure in result["failures"] if fragment not in failure]
                print(f"web_package_gate: {result['case']} fails otherwise than expected: {other}")
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n",
                                               encoding="utf-8")
        passed = sum(result["pass"] for result in results)
        print(f"web_package_gate: {passed}/{len(results)} cases as expected -> {args.out}")
        return 0 if passed == len(results) else 1
    finally:
        if chrome:
            chrome.terminate()
            try:
                chrome.wait(timeout=15)
            except subprocess.TimeoutExpired:
                chrome.kill()
        for server in servers:
            server.shutdown()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
