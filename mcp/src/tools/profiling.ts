import * as fs from "fs";
import * as path from "path";
import { z } from "zod";
import { editorCall } from "../editor-call.js";
import {
  defaultOutputDir,
  ensureServiceRunning,
  kDefaultServicePort,
  newestRgpSince,
  panelCaptureArgs,
  resolveRgpClis,
  runPanelCapture,
  snapshotRgpFiles,
} from "../profiling/rgp.js";
import { defineTool, type ToolDef, ToolError } from "../registry.js";
import { proxyTool } from "./proxy.js";

/** Image name the panel CLI is asked to capture. */
const kEditorProcessName = process.platform === "win32" ? "Editor.exe" : "Editor";

/** Default seconds of GPU work to record when the caller names no duration. */
const kDefaultCaptureDurationSec = 5;

/** What the vendor gate reads out of get_gpu_tooling. */
interface GpuTooling {
  vendor?: string;
  vendorId?: number;
  hardware?: string;
}

/**
 * Refuse on anything but AMD, naming the hardware actually detected.
 *
 * The gate runs before any CLI is resolved or spawned: a Radeon capture on a
 * non-Radeon GPU cannot succeed, so attempting it would only produce a slower,
 * less legible failure than saying so up front.
 */
async function requireAmdGpu(): Promise<GpuTooling> {
  const tooling = await editorCall("get_gpu_tooling") as GpuTooling;
  if (tooling?.vendor !== "AMD") {
    const hardware = tooling?.hardware ?? "unknown hardware";
    const vendor = tooling?.vendor ?? "Unknown";
    throw new ToolError(
      `rgp_capture needs an AMD Radeon GPU. This machine reports vendor ${vendor} — ` +
      `${hardware} (vendorId ${tooling?.vendorId ?? "unknown"}), so no Radeon GPU Profiler ` +
      `capture is possible here. Refusing rather than attempting it.\n` +
      `Required: an AMD Radeon GPU, the developer-mode Radeon driver, and the Radeon ` +
      `Developer Tool Suite (RadeonDeveloperServiceCLI + RadeonDeveloperPanelCLI).\n` +
      `UNVERIFIED-ON-AMD: this refusal is the only path of rgp_capture that has ever been ` +
      `executed. The AMD-side capture flow is untested.`);
  }
  return tooling;
}

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_render_stats",
    category: "profiling",
    description: "Get rendering performance statistics. renderServices.renderThreadCpu.ecsSystems[].maxMs is each ECS system's slowest update since tracking began; pass resetSystemMaxima:true to clear it after this read, so the next read reports the worst single frame in between (read, act, read).",
    schema: {
      resetSystemMaxima: z.coerce.boolean().optional().describe("Clear every ECS system's maxMs after this read (default false)"),
    },
    params: ({ resetSystemMaxima }) => ({ resetSystemMaxima: resetSystemMaxima ?? false }),
  }),

  proxyTool({
    name: "get_validation_stats",
    category: "profiling",
    description: "Exact per-VUID Vulkan validation-error stats: counts, first-occurrence message/objects/labels + frame stamps, suppressed and overflow counters. Immune to log-ring eviction and log thinning. Workflow: call with reset:true, run the phase under test, call again and assert the deltas are zero.",
    schema: {
      reset: z.coerce.boolean().optional().describe("Reset counters after reading (default false)"),
    },
    params: ({ reset }) => ({ reset: reset ?? false }),
  }),

  proxyTool({
    name: "get_gpu_tooling",
    category: "profiling",
    description: "What sits between the engine and the GPU right now: vendor + raw vendorId, hardware string, whether GPU debug labels are LIVE on this device (the entry points actually resolved — not merely that the build has label calls compiled in), whether the engine enabled the validation layer on this instance (request-derived; an externally injected layer reads false here and surfaces in attachedTools instead), and which capture/profiling tools (RenderDoc, Nsight, RGP) are attached via the runtime's tooling query. toolingInfoAvailable:false means the runtime could not be asked at all, which is distinct from an empty attachedTools (asked, nothing attached). Use as a capture/profiling preflight and to prove a measurement was not taken through a capture layer.",
    schema: {},
  }),

  defineTool({
    name: "rgp_capture",
    category: "profiling",
    actsOnHost: true,
    // The one-line help catalog shows only the first sentence, so the warning
    // has to be inside it to reach the discovery surface at all.
    description:
      "Capture a Radeon GPU Profiler trace of the running editor (UNVERIFIED ON AMD HARDWARE). " +
      "Requires an AMD Radeon GPU, the developer-mode driver, and the Radeon Developer Tool Suite " +
      "(RadeonDeveloperServiceCLI + RadeonDeveloperPanelCLI). Written and refusal-path-tested on " +
      "an NVIDIA machine; the AMD-side flow has not been executed. Verify on AMD before relying " +
      "on it.",
    schema: {
      outputDir: z.string().optional().describe("Directory to write the .rgp into (default: build/rgp-captures)"),
      // Positive: a zero or negative duration reaches execFile as an invalid
      // timeout and throws ERR_OUT_OF_RANGE, which the capture's catch-all
      // would report as a panel failure — a misattributed error.
      durationSec: z.coerce.number().int().positive().optional().describe("Seconds of GPU work to record (default: 5)"),
    },
    run: async ({ outputDir, durationSec }) => {
      // Vendor gate first — it is the cheapest refusal and the only one that
      // can fire on non-Radeon hardware.
      const tooling = await requireAmdGpu();

      const clis = resolveRgpClis();
      const service = await ensureServiceRunning(clis);
      const dir = outputDir ? path.resolve(outputDir) : defaultOutputDir();
      fs.mkdirSync(dir, { recursive: true });

      const duration = durationSec ?? kDefaultCaptureDurationSec;
      // Baseline for the output scan, taken before the trigger so a stale .rgp
      // already in the directory cannot be reported as this run's capture.
      const baseline = snapshotRgpFiles(dir);
      const args = panelCaptureArgs({
        processName: kEditorProcessName,
        outputDir: dir,
        durationSec: duration,
        servicePort: kDefaultServicePort,
      });
      // Give the panel the capture window plus headroom to attach and flush.
      const panel = await runPanelCapture(clis, args, (duration + 60) * 1000);
      const capture = newestRgpSince(dir, baseline);

      const logs = { panel: panel.logFile, service: service.logFile ?? null };
      if (!capture) {
        throw new ToolError(
          `No .rgp appeared in ${dir} after the capture (panel exit ${panel.exitCode}).\n` +
          `UNVERIFIED-ON-AMD: the panel invocation and the output location are both untested — ` +
          `check the logs and the panel's own --help for the correct flags.\n` +
          `Panel log: ${logs.panel}\nService log: ${logs.service ?? "(service was already running)"}\n` +
          `Invocation: ${clis.panelCli} ${args.join(" ")}`);
      }

      return {
        data: {
          capture: capture.filePath,
          sizeBytes: capture.sizeBytes,
          outputDir: dir,
          durationSec: duration,
          gpu: { vendor: tooling.vendor, hardware: tooling.hardware },
          clis: { serviceCli: clis.serviceCli, panelCli: clis.panelCli, resolvedVia: clis.source },
          service: { alreadyRunning: service.alreadyRunning, pid: service.pid ?? null },
          panelExitCode: panel.exitCode,
          invocation: [clis.panelCli, ...args],
          logs,
          unverified:
            "UNVERIFIED-ON-AMD: this flow has never been executed on AMD hardware. Confirm the " +
            "trace opens in Radeon GPU Profiler and that engine debug-utils labels appear as user " +
            "markers before trusting it.",
        },
      };
    },
  }),

  proxyTool({
    name: "get_terrain_stats",
    category: "profiling",
    description: "Get CBT terrain tessellation-health stats (occupancy, split/merge demand vs served, overflow) with a plain-language diagnosis of whether the triangulation is converged, saturated, or deadlocked. Also names the kernel arm the device took: heapArm is 'wide' (64-bit heap IDs) or 'narrow' (u32 heap IDs — the arm a device without 64-bit shader integer support falls back to), and subdivCeiling is the subdivision cap that arm's representation can carry. maxDepth is derived under that ceiling, so a narrow arm is the attribution for a terrain that refines less than its size would suggest. targetPixelError is the authored target (pixels at targetReferenceHeightPx rows); splitThresholdPx is the threshold the kernels applied on the last frame at renderHeightPx rows (null before the first frame), and the diagnosis ends with the same sentence.",
    schema: {},
  }),

  proxyTool({
    name: "get_ui_profile_history",
    category: "profiling",
    description: "Return the full UpdateProfileFrame ring buffer (last ~120 frames) for each UI window. Lets you pinpoint spike frames without burst-sampling — scan for max totalMs and inspect that frame's full counter set. Requires GE_UI_UPDATE_PROFILE=1 at launch.",
    schema: {
      maxFrames: z.coerce.number().int().optional().describe("Cap the number of most-recent frames returned per window (default 120)"),
    },
  }),

  proxyTool({
    name: "get_cpu_profiler",
    category: "profiling",
    description: "Read the CPU profiler's last completed frame, both views of it. 'tree' is the main-thread call tree (name/depth/totalMs/selfMs/calls per row); 'flat' is the per-scope aggregate for the same frame, each row carrying thread: 'main' | 'off-main' | 'mixed'. Read BOTH: the call tree holds main-thread scopes only, so every ECS system the scheduler dispatches to a job worker — terrain extraction, terrain modifiers, CBT update, TLAS update — exists as an 'off-main' row in flat and nowhere else. A scope missing from both really did not run. Values are one real frame, a single sample rather than an average, so read a few times before believing a number. Arming: a call arms the profiler (default enable:true) and enable:false disarms it while still returning the frame it paid for. Disarm before collecting performance numbers, because while armed every scope exit takes the profiler's mutex, off-main ones included; timings taken while it is armed are contaminated, and the first frame after arming is empty because no frame has completed yet.",
    schema: {
      enable: z.coerce.boolean().optional().describe("Arm the profiler (default true), or pass false to disarm it. Either way the response describes the last frame recorded before the call, so a disarming read still returns data. A disarm holds only while the editor's CPU Profiler panel is CLOSED — that panel re-arms the profiler every frame it is visible, and nothing arbitrates between the two; check wasEnabledOnEntry on the next read to see whether the disarm survived."),
      maxRows: z.coerce.number().int().optional().describe("Max rows returned in each of tree/flat (default 64)"),
    },
  }),

  proxyTool({
    name: "get_job_system",
    category: "profiling",
    description: "Read the engine job system right now: computeWorkers and blockingThreads; lanes.normal and lanes.background as {queued, running} (jobs waiting in the lane, compute workers running a job of that class); channels[] as {name, queued, running, cap, jobs, queueWaitMs} for every JobChannel (jobs waiting past the cap, jobs holding a slot, cumulative jobs that took a slot, cumulative time jobs waited for one); threads[] with one row per pool thread named as the OS names it ('Job Worker #3', 'Job Blocking #0'), running = 'Normal' | 'Background' | the channel's name | null when idle, and runningForMs for Background and channel jobs only (Normal jobs carry no start time); tasksExecuted (compute-lane dequeues plus channel jobs run) and census (the cumulative queue-topology counters). Gauges are relaxed reads of a running pool, so fields can describe slightly different moments; counters only grow, so read twice and subtract for a rate. Use it to see which thread holds a long job, whether a lane or a channel is backed up, and how many blocking threads exist.",
    schema: {},
  }),

  proxyTool({
    name: "get_gpu_profiler",
    category: "profiling",
    description: "Read per-pass CPU/GPU timings from the render graph for the last executed frame, including query resolveStats and any warnings. The response is the last frame only — there is no rolling history here; use get_monitors with a name for a frame-time ring buffer. Read timingSemantics before interpreting gpuSpanMs: 'pipeline-point' (Vulkan) means the value is the pass's own bracketed execution; 'encoder-span' (Metal — Apple GPUs sample counters only at command-encoder stage boundaries) means it is the summed busy span of the encoder(s) the pass created, with stage gaps and inter-encoder idle excluded, but still overlapping neighbouring passes' spans because the GPU runs encoders concurrently, and still charged in full to every pass that shared an encoder (spanShared true). Under encoder-span the payload deliberately carries no frame GPU total: never sum gpuSpanMs over all passes. Each measurement is counted on exactly one pass (spanCounted true), so to total a group of passes add only those. Use resolveStats.distinctSpanGpuMs (each measurement counted once, still an upper bound) for a ceiling, and get_monitors Time/GPUFramePeriod or get_render_stats gpuSync.frameGpuPeriodMs for the frame's real GPU period.",
    schema: {
      enabled: z.boolean().optional().describe("Arm or disarm per-pass profiling. Omit to leave it as it is — timings are only produced while it is armed."),
    },
  }),

  proxyTool({
    name: "toggle_shadow_debug",
    category: "profiling",
    description: "Toggle shadow debug visualization mode: cascade colors (tint fragments by cascade), shadow factor (grayscale shadow), PCSS branch classification (which early-out a PCSS fragment took), normal/contact masks, normal/contact-only lighting, or off. Omit mode to cycle.",
    schema: {
      mode: z.coerce.number().optional().describe("0=off, 1=cascade colors (red/green/blue/yellow tint), 2=shadow factor (grayscale), 3=PCSS branch (green=early-out lit, red=early-out shadowed, blue=full penumbra path), 4=normal shadow mask, 5=contact shadow mask, 6=contact only (lit), 7=normal only (lit)"),
    },
  }),

  proxyTool({
    name: "set_parallax_steps_view",
    category: "profiling",
    description: "Show or hide the Parallax steps view in every Scene View. Each pixel of a height-mapped (parallax) material is drawn in a palette colour for the height-map samples it took, its relief march and its self-shadow together, at the brightness of its lit colour: grey none (both under a pixel on screen), blue 1-4, cyan 5-8, green 9-12, yellow 13-16, orange 17-24, red 25-53 (53 is the most a desktop pixel takes). Other materials render normally. The colours survive exposure; the sky's aerial perspective tints the far distance blue over them and tonemapping shifts them. To read bands exactly, capture the view's scene colour target with capture_resource on a scene without sky and take the palette colour nearest each pixel's r, g, b over their sum. After a toggle each parallax material compiles its view variant, and its surfaces are missing until that compile lands (a few frames).",
    schema: {
      enable: z.boolean().optional().describe("true shows the view, false hides it. Omit to toggle."),
    },
  }),

  proxyTool({
    name: "toggle_shadow_thumbnails",
    category: "profiling",
    description: "Toggle shadow map cascade thumbnails overlay showing all 4 cascade depth maps as small tiles in the viewport corner.",
    schema: {
      enable: z.coerce.boolean().optional().describe("true to show, false to hide. Omit to toggle."),
    },
  }),
];
