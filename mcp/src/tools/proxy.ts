import type { z, ZodRawShape } from "zod";
import { editorCall } from "../editor-call.js";
import { defineTool, type ToolCategory, type ToolDef } from "../registry.js";

/**
 * Drop keys the caller omitted, so the debug server sees only what was asked
 * for and applies its own defaults to the rest.
 */
function definedOnly(args: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(args)) if (v !== undefined) out[k] = v;
  return out;
}

/**
 * A tool that forwards its arguments to one debug-server method and returns the
 * response as data. This covers most of the registry; a tool needs its own
 * `run` only when it reshapes the response (images, text summaries).
 */
export function proxyTool<S extends ZodRawShape>(def: {
  name: string;
  category: ToolCategory;
  description: string;
  schema: S;
  /** Defaults to `name`; set it only where the tool and the method differ. */
  ipcMethod?: string;
  /** Client-side defaults or argument reshaping, when the method needs them. */
  params?: (args: z.infer<z.ZodObject<S>>) => Record<string, unknown>;
}): ToolDef {
  const method = def.ipcMethod ?? def.name;
  return defineTool({
    name: def.name,
    category: def.category,
    description: def.description,
    schema: def.schema,
    ipcMethod: method,
    run: async (args) => ({
      data: await editorCall(method, def.params ? def.params(args) : definedOnly(args as Record<string, unknown>)),
    }),
  });
}
