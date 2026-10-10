import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "open_asset",
    category: "assets",
    description: "Open an asset in the appropriate editor panel (.timeline, .glsl shader graph, .graph, animation, etc.). Returns openMs for graph assets.",
    schema: { path: z.string().describe("Absolute or project-relative path to the asset file") },
  }),

  proxyTool({
    name: "select_asset",
    category: "assets",
    description: "Select an asset in the Assets panel (panelId=Assets) and push it to Asset View (panelId=AssetView). Returns assetView preview debug state.",
    schema: {
      path: z.string().describe("Absolute or project-relative path to the asset file"),
      silent: z.boolean().optional().describe("If true, do not fire inspector/asset callbacks (default false)"),
    },
    params: ({ path, silent }) => ({ path, silent: silent ?? false }),
  }),

  proxyTool({
    name: "generate_folder_thumbnails",
    category: "assets",
    description: "Queue the persistent thumbnails of every model and material under a folder (the Assets panel's Generate Thumbnails command). They bake after the visible tiles. Returns the queued count.",
    schema: { path: z.string().describe("Folder, absolute or relative to the assets root") },
  }),

  proxyTool({
    name: "get_asset_preview",
    category: "assets",
    description: "Get Asset View preview state: video decode/upload flags, texture resource name, preview path. Use after select_asset.",
    schema: {},
  }),
];
