import type { ToolDef } from "../registry.js";
import { tools as animation } from "./animation.js";
import { tools as assets } from "./assets.js";
import { tools as capture } from "./capture.js";
import { tools as debugMethods } from "./debug-methods.js";
import { tools as ecsAuthoring } from "./ecs-authoring.js";
import { tools as editorState } from "./editor-state.js";
import { tools as gameGraph } from "./game-graph.js";
import { tools as ground } from "./ground.js";
import { tools as lifecycle } from "./lifecycle.js";
import { tools as markups } from "./markups.js";
import { tools as profiling } from "./profiling.js";
import { tools as renderGraph } from "./render-graph.js";
import { tools as renderSettings } from "./render-settings.js";
import { tools as scene } from "./scene.js";
import { tools as ui } from "./ui.js";
import { tools as view } from "./view.js";
import { tools as windowTools } from "./window.js";

/**
 * Every agent-facing operation, in one array. The MCP server and the CLI both
 * build their surface from this — there is no second list.
 */
export const allTools: readonly ToolDef[] = Object.freeze([
  ...editorState,
  ...scene,
  ...ground,
  ...markups,
  ...animation,
  ...ui,
  ...assets,
  ...view,
  ...capture,
  ...renderGraph,
  ...renderSettings,
  ...profiling,
  ...windowTools,
  ...gameGraph,
  ...ecsAuthoring,
  ...lifecycle,
  ...debugMethods,
]);

const byName = new Map(allTools.map(t => [t.name, t]));
// A duplicate name silently shadows a tool in both front-ends; fail at import
// rather than shipping a surface that answers to the wrong handler.
if (byName.size !== allTools.length) {
  const seen = new Set<string>();
  const dupes = allTools.map(t => t.name).filter(n => seen.size === seen.add(n).size);
  throw new Error(`Duplicate tool name(s) in registry: ${[...new Set(dupes)].join(", ")}`);
}

export function toolByName(name: string): ToolDef | undefined {
  return byName.get(name);
}
