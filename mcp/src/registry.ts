import type { z, ZodRawShape } from "zod";

/**
 * The single definition of an agent-facing engine operation.
 *
 * Both front-ends consume this array: the MCP server registers each entry as a
 * tool, the CLI exposes each as a command. Adding an entry yields both; there
 * is deliberately no second list to keep in step.
 */

export type ToolCategory =
  | "editor"
  | "scene"
  | "animation"
  | "ui"
  | "assets"
  | "view"
  | "capture"
  | "rendergraph"
  | "rendering"
  | "profiling"
  | "window"
  | "graph"
  | "ecs"
  | "lifecycle"
  | "meta";

/** Category order and headings for generated help. */
export const kCategoryTitles: Record<ToolCategory, string> = {
  editor: "Editor & scene state",
  scene: "Entities & components",
  animation: "Animation & skinning",
  ui: "UI interaction",
  assets: "Assets",
  view: "Scene view camera",
  capture: "Screenshots & captures",
  rendergraph: "Render graph introspection",
  rendering: "Render quality settings",
  profiling: "Rendering stats & profilers",
  window: "Windows & monitors",
  graph: "Game logic graphs",
  ecs: "ECS authoring (C# codegen)",
  lifecycle: "Build, launch & lifecycle",
  meta: "Debug surface discovery",
};

/**
 * What every handler returns. One shape; each front-end renders it its own way.
 */
export interface ToolOutput {
  /** Structured result. Printed as JSON when there is no summary. */
  data: unknown;
  /**
   * Human-facing text. MCP emits it verbatim as the text block, which is how
   * the build/test/codegen tools keep their plain-text output; the CLI prints
   * it instead of `data` unless --json is set.
   */
  summary?: string;
  /** A PNG the tool produced. MCP emits an image block; the CLI writes a file. */
  image?: { base64: string; mimeType: "image/png"; filePath?: string };
}

export type ToolErrorCode =
  /** Nothing is listening, or the socket closed with requests outstanding. */
  | "EDITOR_UNREACHABLE"
  /** Connected, but the editor did not answer within the budget. */
  | "EDITOR_TIMEOUT"
  /** The handler ran and said no (build failed, file exists, unknown node id). */
  | "TOOL_FAILED"
  /** The caller's arguments are wrong; caught before dispatch. */
  | "BAD_ARGUMENT";

export class ToolError extends Error {
  constructor(message: string, readonly code: ToolErrorCode = "TOOL_FAILED") {
    super(message);
    this.name = "ToolError";
  }
}

export interface ToolDef {
  /** snake_case identity: the MCP tool name, the CLI command, the drift key. */
  readonly name: string;
  readonly category: ToolCategory;
  readonly description: string;
  /** Handed to server.tool() unchanged, and used to derive CLI flags. */
  readonly schema: ZodRawShape;
  /**
   * The debug-server method this tool proxies, as literal data rather than a
   * detail buried in `run`. Tests/Mcp/ipc-method-drift.mjs reads it with a regex
   * so the coverage check needs neither a build nor an import of this module.
   */
  readonly ipcMethod?: string;
  /** Further debug-server methods this tool drives. Drift metadata only. */
  readonly coversIpcMethods?: readonly string[];
  /**
   * `run` writes files or starts or stops processes from this process, where the
   * editor's request gate cannot see it. A server bound to an assistant session
   * (GE_ASSISTANT_TOKEN) asks the editor with `assistant_authorize` first and runs
   * the tool only when the editor admits it.
   */
  readonly actsOnHost?: true;
  run(args: Record<string, unknown>): Promise<ToolOutput>;
}

/**
 * Authoring helper: keeps `run`'s argument type inferred from `schema` at the
 * definition site, while the registry stores the erased `ToolDef`.
 */
export function defineTool<S extends ZodRawShape>(def: {
  name: string;
  category: ToolCategory;
  description: string;
  schema: S;
  ipcMethod?: string;
  coversIpcMethods?: readonly string[];
  actsOnHost?: true;
  run: (args: z.infer<z.ZodObject<S>>) => Promise<ToolOutput>;
}): ToolDef {
  return def as unknown as ToolDef;
}

/** Convenience for the common case: a handler whose whole result is JSON data. */
export function jsonOutput(data: unknown): ToolOutput {
  return { data };
}
