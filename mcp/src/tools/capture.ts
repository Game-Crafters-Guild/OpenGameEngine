import { z } from "zod";
import { editorCall } from "../editor-call.js";
import { defineTool, type ToolDef, type ToolOutput } from "../registry.js";
import { proxyTool } from "./proxy.js";

/**
 * Split a capture response into the PNG and everything else. The base64 payload
 * never travels in `data`: MCP sends it as an image block and the CLI writes it
 * to disk, so leaving it in the structured result would only mean megabytes of
 * base64 on stdout.
 */
function splitImage(result: any): { image: ToolOutput["image"]; data: unknown } {
  const { pngBase64, ...rest } = result;
  return { image: { base64: pngBase64, mimeType: "image/png", filePath: result.filePath }, data: rest };
}

export const tools: ToolDef[] = [
  defineTool({
    name: "take_screenshot",
    category: "capture",
    ipcMethod: "take_screenshot",
    description: "Capture a PNG screenshot of the editor. Default target is the entire editor window (UI + viewport composited). Other targets crop to a specific region of the same composite. Returns an image that Claude can view, plus the file path where the PNG was saved. " +
      "BEFORE DRAWING A VISUAL CONCLUSION, CHECK `pendingMaterialTextureBinds`. It is 0 on a settled frame, and non-zero while that many material texture slots are still waiting on a decode — those materials render their bindless default (white albedo, flat normal) instead of the authored texture. " +
      "On screen the two states are identical, so a capture taken inside that window reads as 'this material never binds'. A texture's first material bind also tags its cook usage, so on a cold derived cache the window is as wide as the block-compression encode: seconds to minutes, not frames. " +
      "Poll get_editor_state until it reports 0 before capturing anything you intend to measure or A/B. " +
      "The capture is the render graph's own frame (`method: \"rendergraph\"`). When the editor cannot produce one — the scene view's tab is behind another, the window is minimized, the frame loop is starved — the call fails with the reason rather than substituting a picture of the window; pass allowWindowCapture=true if a window picture is what you want.",
    schema: {
      target: z.enum(["window", "viewport", "panel", "element", "rect", "asset_preview"]).optional()
        .describe("What to capture: 'window' (full editor, default), 'viewport' (scene view only), 'panel' (a docked panel), 'element' (a UI element by id), 'rect' (custom rectangle in window pixels), or 'asset_preview' (live GPU video frame from Asset View — works on macOS when UI composite readback is unavailable)."),
      panelId: z.string().optional().describe("Required when target='panel'. Dock panel ID: 'Assets', 'AssetView', 'Hierarchy', 'Inspector', etc. Use get_panel_tree to discover bounds."),
      elementId: z.string().optional().describe("Required when target='element'. UI element id (matches FindById). Asset View preview image: 'asset-view-preview-image'."),
      x: z.coerce.number().int().optional().describe("target='rect': left edge."),
      y: z.coerce.number().int().optional().describe("target='rect': top edge."),
      w: z.coerce.number().int().optional().describe("target='rect': width."),
      h: z.coerce.number().int().optional().describe("target='rect': height."),
      coords: z.enum(["logical", "physical"]).optional()
        .describe("target='rect' coordinate space. 'logical' (default) matches get_ui_tree / get_panel_tree output and panel/element bounds. 'physical' is the saved PNG's pixel grid (useful for cropping a previously-saved screenshot)."),
      allowWindowCapture: z.boolean().optional()
        .describe("Accept an OS window capture (method: 'printwindow', with fallbackReason) when the render graph cannot hand over the frame. Default false: an unavailable frame is an error naming the reason, because a window picture looks like a valid capture while showing whatever the compositor holds — including other windows' pixels and stale content."),
    },
    run: async (args) => {
      const params: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(args)) if (v !== undefined) params[k] = v;
      const result = await editorCall("take_screenshot", params) as any;
      if (!result?.pngBase64) return { data: result };
      const tgt = result.target ?? args.target ?? "window";
      const { image, data } = splitImage(result);
      // Surfaced in the summary, not just the payload: an unsettled frame is a
      // plausible PNG, never an error, so the warning has to reach the reader
      // who did not think to look for it.
      const pending = Number(result.pendingMaterialTextureBinds ?? 0);
      const warn = pending > 0
        ? ` — WARNING: ${pending} material texture bind(s) still pending; those materials are showing bindless defaults, not their textures`
        : "";
      // Same reason the pending-bind warning is here: a window capture is a
      // plausible PNG of the wrong thing, so the method has to reach the reader
      // rather than sit in the payload.
      const substitute = result.method === "printwindow"
        ? ` — WARNING: this is an OS window capture, not the rendered frame (${result.fallbackReason ?? "reason not stated"})`
        : "";
      return { data, image, summary: `Screenshot (${tgt}) ${result.width}x${result.height} saved to ${result.filePath}${warn}${substitute}` };
    },
  }),

  defineTool({
    name: "capture_resource",
    category: "capture",
    ipcMethod: "capture_resource",
    description: "Capture a named render graph resource from the live frame: a texture as a PNG, a buffer as bytes. Use get_render_graph_resources to discover names. The name matches exactly, else as a unique case-insensitive substring; a miss or an ambiguity reports the frame's candidate names. Textures support depth (grayscale), float (normalized), packed and color formats — adjust rangeMin/rangeMax for float/depth visualization. No transfer curve is applied, so the pixel values are the ones the graph holds.",
    schema: {
      name: z.string().describe("Render graph resource name (e.g. 'SceneView.Color', 'SceneView.Depth')"),
      rangeMin: z.coerce.number().optional().describe("Textures: value mapped to black (default 0.0). For depth: near plane value."),
      rangeMax: z.coerce.number().optional().describe("Textures: value mapped to white (default 1.0). For depth: far plane value."),
      mip: z.coerce.number().optional().describe("Textures: mip level to capture (default 0)"),
      layer: z.coerce.number().optional().describe("Textures: array layer / cube face to capture (default 0)"),
      offset: z.coerce.number().optional().describe("Buffers: byte offset to start reading (default 0)"),
      byteCount: z.coerce.number().optional().describe("Buffers: bytes to read (default: to the end, capped at 1 MiB)"),
      decode: z.enum(["hex", "float", "uint"]).optional().describe("Buffers: also return the bytes decoded as an array of float32 or uint32 (default hex = base64 only)"),
    },
    run: async (args) => {
      const params: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(args)) if (v !== undefined) params[k] = v;
      const result = await editorCall("capture_resource", params) as any;
      if (result?.pngBase64) {
        const { image, data } = splitImage(result);
        return {
          data, image,
          summary: `Captured ${result.resourceName} (${result.width}x${result.height}, ${result.format}, ${result.channels}ch, mip ${result.mip} layer ${result.layer}) → ${result.filePath}`,
        };
      }
      if (result?.kind === "buffer") {
        const values = result.floats ?? result.uints;
        const preview = values ? `\n${JSON.stringify(values.slice(0, 64))}${values.length > 64 ? ` … (${values.length} total)` : ""}` : "";
        return {
          data: result,
          summary: `Captured buffer ${result.resourceName} (${result.byteCount} bytes @ offset ${result.offset})${preview}`,
        };
      }
      return { data: result };
    },
  }),

  proxyTool({
    name: "trigger_capture",
    category: "capture",
    description: "Trigger a RenderDoc frame capture. The editor must be running under RenderDoc — launch_editor with renderdoc=true does that. Waits for the capture to finish writing (a large scene serializes >100 MB over many frames) and returns {captured, captureIndex, filePath}. The .rdc lands at the editor's capture path template: GE_RENDERDOC_CAPTURE_DIR if set, else <editor exe dir>/Captures/editor_frameN.rdc. When RenderDoc is not attached the call fails with an error naming the reason. If the wait times out, the capture is still completing: call get_capture_status to recover its path.",
    schema: {},
  }),

  proxyTool({
    name: "get_capture_status",
    category: "capture",
    description: "Report RenderDoc capture state immediately: {available, captureCount, lastCapturePath}. Use it to recover a capture whose trigger_capture call timed out before RenderDoc finished writing the .rdc, and to check whether the editor is running under RenderDoc at all before triggering.",
    schema: {},
  }),

  proxyTool({
    name: "nsight_capture",
    category: "capture",
    description: "Trigger an NVIDIA Nsight Graphics frame capture. The editor must have been launched by ngfx-capture.exe (use mcp/scripts/ngfx-capture-editor.mjs) — the capture libraries arrive by injection, so an editor started normally cannot capture. GE_VK_CAPTURE_COMPAT=1 is recommended so VK_EXT_descriptor_buffer is out of the capture's way; the launch script sets it by default. Note that get_gpu_tooling does NOT confirm Nsight: it injects by loader interception rather than registering a Vulkan layer, so attachedTools stays empty even while capture works. The error this tool fails with is the availability answer; to check injection out-of-band, look for ngfx-capture-interception.dll in the editor's loaded modules. Returns the capture file path once Nsight finishes serializing it, which takes noticeably longer than a RenderDoc capture — raise GE_MCP_IPC_TIMEOUT_MS (default 30000) if a heavy scene or a large frames count outruns the client's wall-clock timeout.",
    schema: {
      frames: z.coerce.number().int().min(1).max(60).optional()
        .describe("Number of consecutive frames to capture, starting at the next present (default 1, max 60)."),
    },
  }),
];
