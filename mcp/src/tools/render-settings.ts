import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

// Runtime render-quality knobs, for same-session A/B measurement.
//
// Two families with different persistence, which the descriptions call out
// because the difference decides whether a run is reproducible after a restart:
// set_msaa / set_aa_mode / set_taa_render_scale / set_hdr_output write the
// project settings file and re-apply from it, while set_lod, the shadow knobs
// and set_dynamic_resolution are live-only and revert on restart.
//
// Booleans are z.boolean(), never z.coerce.boolean(): coercion maps the string
// "false" to true, which would silently enable the thing you asked to disable.

const viewBudget = z.object({
  enabled: z.boolean().optional().describe("Whether this view class overrides the global budget"),
  percent: z.number().optional().describe("Budget as a percentage of the global error budget"),
});

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_msaa",
    category: "rendering",
    description: "Report the resolved anti-aliasing state without changing it: aaMode (off|msaa|taa — the engine-wide mode in effect), aaModePersisted (the project's rendering.aaMode, a concrete off|msaa|taa token; empty only for a project whose settings file has never been materialized, in which case aaMode is the capability default resolved in memory), samples (the default MSAA sample count cameras with MSAASamples==0 inherit), deviceMax, and persisted (the project's rendering.msaa). This is the only NON-MUTATING way to read the mode; set_aa_mode reports the same applied value but writes project settings to do it. Read aaMode before believing samples: the count keeps its value while the mode is off or taa, so samples alone can report 8x on a run with no MSAA. Both mode fields are the engine-wide default, not necessarily what a given view renders — treat them as a floor: a TAA mode is dropped per-view where TAA does not apply (orthographic, 2D, fixed-orientation, letterboxed or already-multisampled views), and a camera can override the mode outright.",
    schema: {},
  }),

  proxyTool({
    name: "set_msaa",
    category: "rendering",
    description: "Set the engine-wide default MSAA sample count. PERSISTS to the project's rendering.msaa setting and then re-applies live to every window — this outlives the session, unlike the shadow and LOD knobs. Only honoured while the AA mode is msaa. Every accepted value is a concrete count — there is no 'auto', and 0 is rejected: a project that names no usable count takes the capability default (4x MSAA where the device reaches it, else 2x, else TAA). Shadow maps are unaffected (depth-only, always single-sample); the device clamps counts it cannot support.",
    schema: {
      samples: z.coerce.number().int().describe("1 = off, or 2, 4, 8"),
    },
  }),

  proxyTool({
    name: "set_aa_mode",
    category: "rendering",
    description: "Select the anti-aliasing mode. PERSISTS to the project's rendering.aaMode setting and re-applies live (render targets re-spec next frame). The mode leaves the sample count as set_msaa configured it, honoured only in msaa mode. The response reports the APPLIED mode — the same off|msaa|taa token get_msaa returns — plus samples; get_msaa reads the same value without writing settings. Under the GE_AA_MODE environment override the value is still persisted but the live apply is suppressed, because the env var outranks project settings, so 'applied' will not match the request. A project that has never chosen a mode is given one when it opens (4x MSAA if the device supports it, else 2x, else TAA) — there is no 'auto' value to select.",
    schema: {
      mode: z.enum(["off", "msaa", "taa", "fxaa", "smaa", "temporalfxaa", "ssaa"]).describe("Anti-aliasing mode. 'ssaa' persists as editor-level Off + supersampling via set_taa_render_scale"),
    },
  }),

  proxyTool({
    name: "set_taa_render_scale",
    category: "rendering",
    description: "Set the render scale — the resolution the scene renders at before rescaling to the output. PERSISTS to project settings and applies fully live, including the material texture mip bias derived from the actual extent ratio. Works under every AA mode (TAA upscales temporally, the others through the spatial upscale pass). Under GE_TAA_RENDER_SCALE the value persists but the live apply is suppressed.",
    schema: {
      scale: z.coerce.number().describe("Render scale in [0.5, 2.0]; 1.0 = native, above 1.0 supersamples (SSAA)"),
    },
  }),

  proxyTool({
    name: "set_dynamic_resolution",
    category: "rendering",
    description: "Configure dynamic resolution scaling for same-session A/B. Live only — it does NOT persist (targetMs, minScale and maxScale have no project key), so a restart reverts it. 'dynamic' arms the render-graph GPU profiler, whose per-pass timestamps are the cost signal, and drives the same render scale set_taa_render_scale writes, so the two are mutually exclusive by construction. Returns the live controller state, so a harness can watch the loop converge without taking screenshots.",
    schema: {
      mode: z.enum(["off", "fixed", "dynamic"]).optional().describe("off = native, fixed = hold the current scale, dynamic = close the loop on frame cost"),
      targetFps: z.coerce.number().optional().describe("Controller target in frames per second (must be > 0). Alternative to targetMs."),
      targetMs: z.coerce.number().optional().describe("Controller target in milliseconds per frame (must be > 0). Alternative to targetFps."),
      minScale: z.coerce.number().optional().describe("Lower bound on the render scale the controller may choose"),
      maxScale: z.coerce.number().optional().describe("Upper bound on the render scale the controller may choose"),
    },
  }),

  proxyTool({
    name: "set_lod",
    category: "rendering",
    description: "Tune mesh LOD selection. Live only: these write the same RenderServices state the project settings seed at startup, but nothing is written back, so a restart returns to the persisted values. Switching `mode` re-derives every registered GPUMesh row in place, so both arms of an A/B run in one process at one GPU clock state — the reported rowsRewritten is how you confirm the arm actually changed rather than silently no-op'ing.",
    schema: {
      mode: z.enum(["off", "coverage", "sse"]).optional().describe("LOD selection mapping. Re-derives every mesh row; check rowsRewritten in the response."),
      forceLevel: z.coerce.number().int().optional().describe("Pin every mesh to this LOD index; negative means auto"),
      bias: z.coerce.number().optional().describe("Global LOD bias"),
      shadowBias: z.coerce.number().optional().describe("Additional LOD bias applied in shadow passes"),
      cullCoverage: z.coerce.number().optional().describe("Small-object cull coverage threshold (clamped at 0)"),
      budgetPx: z.coerce.number().optional().describe("SSE error budget in pixels; <= 0 disables SSE coarsening (keep-detail fail-safe)"),
      skinnedScale: z.coerce.number().optional().describe("Multiplies the budget for skinned/character chains"),
      hysteresis: z.coerce.number().optional().describe("Hysteresis band that damps LOD flapping"),
      crossfadeDuration: z.coerce.number().optional().describe("LOD crossfade duration in seconds"),
      gameViewBudget: viewBudget.optional().describe("Per-view budget override for Game views. A missing sub-key keeps its current value."),
      sceneViewBudget: viewBudget.optional().describe("Per-view budget override for the editor Scene view. A missing sub-key keeps its current value."),
    },
  }),

  proxyTool({
    name: "set_shadow_quality",
    category: "rendering",
    description: "Select the shadow filtering kernel. Live only. 0 = 5x5 grid PCF (25 taps), 1 = 3x3 grid PCF (9 taps), 2 = Poisson PCF (variable taps), 3 = PCSS (contact hardening), 4 = MSM4 (moment shadow maps). Out-of-range values are clamped, and the response names the kernel that ended up active.",
    schema: {
      quality: z.coerce.number().int().describe("Filtering kernel, 0-4 (see the description)"),
    },
  }),

  proxyTool({
    name: "set_shadow_pcss",
    category: "rendering",
    description: "Toggle and tune PCSS contact-hardening shadows. Live only. The response reports `effective`, which is the only field worth asserting on: PCSS needs bindless textures and is skipped on devices that prefer stable shadow filtering, so `enabled` can be true while nothing changes on screen.",
    schema: {
      enabled: z.boolean().optional().describe("Enable PCSS. Check `effective` in the response, not this value."),
      maxPenumbra: z.coerce.number().optional().describe("World-unit cap on physical penumbra width, applied in the shader"),
      receiverPlaneBias: z.boolean().optional().describe("Enable receiver-plane depth bias"),
      tapCount: z.coerce.number().int().optional().describe("PCSS filter tap count"),
      ditherBasis: z.coerce.number().int().optional().describe("Dither basis for the PCSS sample pattern"),
    },
  }),

  proxyTool({
    name: "set_moments_resolution",
    category: "rendering",
    description: "Set the MSM4 moment shadow map resolution (only meaningful while shadow quality is 4). The value snaps to 512, 1024 or 2048; the response reports what it snapped to. Live only.",
    schema: {
      resolution: z.coerce.number().int().describe("Requested resolution; snaps to 512, 1024 or 2048"),
    },
  }),

  proxyTool({
    name: "set_msm_blur_mode",
    category: "rendering",
    description: "Set the MSM4 blur kernel (only meaningful while shadow quality is 4). Live only. Out-of-range values are clamped and the response names the mode that ended up active.",
    schema: {
      mode: z.coerce.number().int().describe("0 = Linear5Tap (fast), 1 = Discrete9Tap"),
    },
  }),

  proxyTool({
    name: "get_hdr_output",
    category: "rendering",
    description: "Report the device's HDR output state — swapchain mode, encode anchors, static metadata — alongside the OS per-monitor SDR white levels, so the paper-white divisor can be checked against what the OS reports. Also returns the main window UI's last resolved output encoding and its subpixel-text gate.",
    schema: {},
  }),

  proxyTool({
    name: "set_hdr_output",
    category: "rendering",
    description: "Enable or disable HDR output and choose its mode. PERSISTS to project settings and queues a runtime refresh across windows, so the change lands a frame or more later rather than immediately. Mode matching ignores case, spaces, dashes and underscores.",
    schema: {
      enabled: z.boolean().optional().describe("Enable HDR output (default true)"),
      mode: z.string().optional().describe("Auto (default), HDR10_PQ (also PQ/ST2084/HDR10), HLG, scRGB (also ExtendedSRGB), HDR10Plus, or Off"),
    },
  }),
];
