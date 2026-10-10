import { z } from "zod";
import { readDebugMethodDocs, readRawOnlyReasons } from "../debug-server-docs.js";
import { defineTool, type ToolDef } from "../registry.js";

// One tool that makes the whole debug-server surface discoverable, instead of
// promoting every method to its own tool. A typed tool costs ~150 tokens of
// every agent's tools/list forever; this buys discoverability for all of them
// once, and the methods stay callable through `raw`.

export const tools: ToolDef[] = [
  defineTool({
    name: "list_debug_methods",
    category: "meta",
    description: "Discover debug-server methods that have no typed tool, with the parameter docs from the comments above their C++ registration. Call these with the `raw` passthrough (CLI: `ge raw <method> '{...}'`; MCP: there is no raw tool, so ask the user to run it). Defaults to the methods that are NOT already tools — those you can call directly and should not look up here.",
    schema: {
      filter: z.string().optional().describe("Substring match on the method name or its docs"),
      method: z.string().optional().describe("Return one method's full documentation"),
      includeCovered: z.boolean().optional().describe("Also list methods that already have a typed tool (default false)"),
    },
    run: async ({ filter, method, includeCovered }) => {
      // Imported here rather than at module scope: tools/index.ts imports this
      // module, so a top-level import would close a cycle.
      const { allTools } = await import("./index.js");
      const coveredBy = new Map<string, string>();
      for (const tool of allTools) {
        if (tool.ipcMethod) coveredBy.set(tool.ipcMethod, tool.name);
        for (const extra of tool.coversIpcMethods ?? []) coveredBy.set(extra, tool.name);
      }

      const reasons = readRawOnlyReasons();
      const docs = readDebugMethodDocs();
      if (docs.length === 0) {
        return { data: { methods: [] }, summary: "No debug-server sources found — is this running outside the repo?" };
      }

      const rows = docs.map(d => ({
        method: d.method,
        source: `${d.file}:${d.line}`,
        tool: coveredBy.get(d.method) ?? null,
        rawOnlyReason: reasons[d.method] ?? null,
        doc: d.doc,
      }));

      if (method) {
        const one = rows.find(r => r.method === method);
        if (!one) {
          // Match both directions: a typo is as often longer than the real
          // name ("open_scenes") as it is shorter ("open_sce").
          const near = rows
            .filter(r => r.method.includes(method) || method.includes(r.method))
            .map(r => r.method)
            .slice(0, 8);
          return {
            data: { method, found: false, similar: near },
            summary: `No debug-server method '${method}'.${near.length ? ` Did you mean: ${near.join(", ")}?` : ""}`,
          };
        }
        return {
          data: one,
          summary: [
            `${one.method}  (${one.source})`,
            one.tool ? `Typed tool: ${one.tool} — call that instead of raw.` : "No typed tool — call it with `raw`.",
            one.rawOnlyReason ? `Raw-only because: ${one.rawOnlyReason}` : "",
            "",
            one.doc ?? "(no documentation comment above its registration)",
          ].filter(Boolean).join("\n"),
        };
      }

      const needle = filter?.toLowerCase();
      const selected = rows.filter(r => {
        if (!includeCovered && r.tool) return false;
        if (!needle) return true;
        return r.method.toLowerCase().includes(needle) || (r.doc ?? "").toLowerCase().includes(needle);
      });

      const width = selected.length ? Math.max(...selected.map(r => r.method.length)) : 0;
      const lines = selected.map(r => {
        // First doc line only — `method:` returns the full text.
        const gist = (r.doc ?? r.rawOnlyReason ?? "").split("\n")[0];
        return `  ${r.method.padEnd(width)}  ${r.tool ? `[tool: ${r.tool}] ` : ""}${gist}`;
      });

      const scope = includeCovered ? "debug-server methods" : "methods with no typed tool";
      return {
        data: { total: rows.length, shown: selected.length, methods: selected },
        summary: selected.length === 0
          ? `No ${scope} match${needle ? ` '${filter}'` : ""}.`
          : `${selected.length} of ${rows.length} ${scope}${needle ? ` matching '${filter}'` : ""}:\n${lines.join("\n")}\n\nFull docs for one: list_debug_methods with method=<name>. Call them with raw.`,
      };
    },
  }),
];
