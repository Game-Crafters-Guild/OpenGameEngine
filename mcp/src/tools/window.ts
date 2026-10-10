import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_monitors",
    category: "window",
    description: "Enumerate DebugMetrics monitors (FPS, frame phases, memory, draw counts, ...) with their latest/peak/average. Pass 'name' to also return the full history samples for that monitor.",
    schema: {
      name: z.string().optional().describe("Monitor name to fetch history for (e.g. 'Time/FrameMs')"),
    },
  }),

  proxyTool({
    name: "move_window",
    category: "window",
    description: "Move/resize an editor window. Pass 'monitor' to place it on a specific display (by index from get_editor_state's monitor list) — it fills that display's work area unless width/height are given. Or pass explicit x/y (and optional width/height). HDR output is applied on a runtime refresh, so after moving onto an HDR display you may still need to toggle HDR in Settings.",
    schema: {
      windowIndex: z.coerce.number().optional().describe("Which editor window (default 0 = main)"),
      monitor: z.coerce.number().optional().describe("Target display index; places the window on that monitor's work area"),
      x: z.coerce.number().optional().describe("Window top-left X (overrides monitor placement)"),
      y: z.coerce.number().optional().describe("Window top-left Y (overrides monitor placement)"),
      width: z.coerce.number().optional().describe("Window width"),
      height: z.coerce.number().optional().describe("Window height"),
    },
  }),
];
