import { ipcConfig } from "./config.js";
import { editorCall } from "./editor-call.js";
import type { ToolDef } from "./registry.js";
import { allTools } from "./tools/index.js";

// What an MCP server started for the editor's AI Assistant serves and asks. The
// editor writes both variables into the server's environment when a conversation
// attaches it; a server started without them serves every tool, as before.

/**
 * The tools this server registers: the names in GE_MCP_TOOLS (comma-separated),
 * or every tool when it is unset. Throws on a name the registry does not have, so
 * a mistyped or retired name stops the server at start instead of shrinking the
 * set without a word.
 */
export function servedTools(): readonly ToolDef[] {
  const list = process.env.GE_MCP_TOOLS;
  if (list === undefined) return allTools;
  const names = list.split(",").map(n => n.trim()).filter(n => n.length > 0);
  const known = new Set(allTools.map(t => t.name));
  const unknown = names.filter(n => !known.has(n));
  if (unknown.length > 0) {
    throw new Error(
      `GE_MCP_TOOLS names tools this server does not have: ${unknown.join(", ")}. ` +
      `List the server's tools with \`node mcp/ge.mjs --help\`, or rebuild the McpServer target if the editor is newer.`);
  }
  const wanted = new Set(names);
  return allTools.filter(t => wanted.has(t.name));
}

/**
 * Before a tool that acts on this machine runs (ToolDef.actsOnHost), a server bound
 * to an assistant session asks the editor's gate, as the editor gates the requests
 * it receives itself. Throws the editor's refusal; an unbound server asks nothing.
 */
export async function authorizeHostAction(tool: ToolDef, args: Record<string, unknown>): Promise<void> {
  if (!tool.actsOnHost || !ipcConfig().assistantToken) return;
  await editorCall("assistant_authorize", { tool: tool.name, arguments: args });
}
