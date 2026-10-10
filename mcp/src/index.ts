#!/usr/bin/env node

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { authorizeHostAction, servedTools } from "./assistant.js";
import { ipcConfig } from "./config.js";
import { editorCallCount, lastEditorEndpoint } from "./editor-call.js";
import { ipc } from "./ipc-singleton.js";
import type { ToolDef } from "./registry.js";
import { toolByName } from "./tools/index.js";

// MCP front-end over the shared tool registry (src/tools/). This file adapts
// ToolOutput to MCP content blocks and applies the AI Assistant's tool set and
// host-action check (assistant.ts) — a new tool is a new registry entry, never an
// edit here. The CLI front-end (src/cli.ts) adapts the
// same registry for the terminal.
//
// Which editor a call reached is this layer's business, not a tool's: an MCP
// server has no per-call port and takes its editor from GE_EDITOR_DEBUG_PORT at
// launch, so a session whose server was started without one talks to whatever
// holds the default port — often another session's editor. Every answer that
// came from an editor therefore says which editor it came from.

const server = new McpServer({
  name: "GameEngine",
  version: "2.0.0",
});

let tools: readonly ToolDef[];
try {
  tools = servedTools();
} catch (err: any) {
  console.error(`[gameengine-mcp] ${err?.message ?? String(err)}`);
  process.exit(1);
}
const servedNames = new Set(tools.map(t => t.name));

for (const tool of tools) {
  server.tool(tool.name, tool.description, tool.schema, async (args: Record<string, unknown>) => {
    // Only when this invocation actually reached an editor — attributing an
    // endpoint to a build or codegen tool would be a claim it never made. A
    // refusal counts as reaching one; an unreachable editor does not.
    const callsBefore = editorCallCount();
    const answeredEndpoint = () => (editorCallCount() > callsBefore ? lastEditorEndpoint() : null);
    const withEndpoint = (body: string) => {
      const endpoint = answeredEndpoint();
      return endpoint ? `${body}\n\n[editor ${endpoint}]` : body;
    };
    try {
      await authorizeHostAction(tool, args);
      const out = await tool.run(args);
      const content: Array<
        | { type: "text"; text: string }
        | { type: "image"; data: string; mimeType: string }
      > = [];
      // Image first: an agent reading the response sees the picture before the
      // path, which is the order take_screenshot has always answered in.
      if (out.image) content.push({ type: "image", data: out.image.base64, mimeType: out.image.mimeType });
      content.push({ type: "text", text: withEndpoint(out.summary ?? JSON.stringify(out.data, null, 2)) });
      return { content };
    } catch (err: any) {
      return { content: [{ type: "text" as const, text: withEndpoint(err?.message ?? String(err)) }], isError: true };
    }
  });
}

// Resources are views onto the same tools, so they cannot report something the
// tools disagree with. Each degrades to an empty document when the editor is
// unreachable: a resource read is ambient, not a command the user issued. A
// resource whose tool this server does not serve (GE_MCP_TOOLS) is not registered.
function registerResource(name: string, toolName: string, description: string, args: Record<string, unknown>, empty: string) {
  if (!servedNames.has(toolName)) return;
  server.resource(
    name,
    `gameengine://${name}`,
    { description, mimeType: "application/json" },
    async (uri) => {
      try {
        const out = await toolByName(toolName)!.run(args);
        return { contents: [{ uri: uri.href, mimeType: "application/json", text: JSON.stringify(out.data, null, 2) }] };
      } catch {
        return { contents: [{ uri: uri.href, mimeType: "application/json", text: empty }] };
      }
    },
  );
}

registerResource("log", "get_log", "Recent engine log messages", { count: 200 }, "[]");
registerResource("scene", "get_scene_hierarchy", "Current scene entity hierarchy", {}, "{}");
registerResource("stats", "get_render_stats", "Real-time render/perf stats", {}, "{}");

async function main() {
  // Connect on first tool call (on-demand), not eagerly.
  ipc().on("connected", () => console.error(`[gameengine-mcp] Connected to editor ${ipc().remoteEndpoint}`));
  ipc().on("disconnected", () => console.error("[gameengine-mcp] Disconnected from editor"));

  const transport = new StdioServerTransport();
  await server.connect(transport);
  // Name the target at startup: the port is fixed for this server's lifetime,
  // so this is the moment a session can still notice it is aimed elsewhere.
  const cfg = ipcConfig();
  console.error(
    `[gameengine-mcp] MCP server ready — editor ${cfg.host}:${cfg.port}` +
    ` (set GE_EDITOR_DEBUG_PORT in this server's environment to target another editor)`);
}

main().catch((e) => {
  console.error("[gameengine-mcp] Fatal:", e);
  process.exit(1);
});
