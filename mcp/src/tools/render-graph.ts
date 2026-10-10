import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_render_graph_overview",
    category: "rendergraph",
    description: "Get render graph compile stats, timing, pass/resource/barrier counts, transient pool stats, validation summary, and pipelineCompiles (render pipeline compiles so far; climbing every frame means a pipeline is compiled every frame)",
    schema: {},
  }),

  proxyTool({
    name: "get_render_graph_passes",
    category: "rendergraph",
    description: "List all render graph passes in execution order with phase, queue type, barrier count, and optionally resource accesses",
    schema: {
      includeAccesses: z.coerce.boolean().optional().describe("Include per-pass resource access details (default false — set true for full info)"),
    },
    params: ({ includeAccesses }) => ({ includeAccesses: includeAccesses ?? false }),
  }),

  proxyTool({
    name: "get_render_graph_resources",
    category: "rendergraph",
    description: "List all render graph resources with type, lifetime, format, dimensions, aliasing group, and current state",
    schema: {
      lifetime: z.string().optional().describe("Filter by lifetime: Transient, Persistent, or Imported"),
      type: z.string().optional().describe("Filter by type: Texture or Buffer"),
      aliveOnly: z.coerce.boolean().optional().describe("Only show resources that are actively used (default false)"),
    },
    // An empty filter string means "no filter", so it is dropped rather than
    // sent as a value nothing matches.
    params: ({ lifetime, type, aliveOnly }) => {
      const params: Record<string, unknown> = {};
      if (lifetime) params.lifetime = lifetime;
      if (type) params.type = type;
      if (aliveOnly !== undefined) params.aliveOnly = aliveOnly;
      return params;
    },
  }),

  proxyTool({
    name: "get_render_graph_pass_detail",
    category: "rendergraph",
    description: "Get detailed info for a specific render pass: resource accesses, barriers (with before/after states), dependencies",
    schema: {
      passName: z.string().describe("Exact pass name (use get_render_graph_passes to discover names)"),
    },
    // This handler reads `name`, while its sibling get_pass_descriptors reads
    // `passName` (DebugHandlers.cpp:7513). The tool keeps the sibling's
    // spelling and renames on the wire.
    params: ({ passName }) => ({ name: passName }),
  }),

  proxyTool({
    name: "get_render_graph_dependencies",
    category: "rendergraph",
    description: "Get all dependency edges between render passes (the compiled DAG). Each edge shows which pass must execute before another.",
    schema: {},
  }),

  proxyTool({
    name: "get_render_graph_validation",
    category: "rendergraph",
    description: "Get render graph validation errors and warnings: same-pass R/W conflicts, version bumps, back-in-time reads, mixed version reads",
    schema: {},
  }),

  proxyTool({
    name: "get_pass_descriptors",
    category: "rendergraph",
    description: "Get descriptor set bindings for all draws in a named render pass. Captures the physical VkBuffer/VkImage handles bound at draw time — useful for diagnosing cases where CPU-side data looks correct but the GPU reads from the wrong resource.",
    schema: {
      passName: z.string().describe("Render pass name (use get_render_graph_passes to discover names)"),
    },
  }),
];
