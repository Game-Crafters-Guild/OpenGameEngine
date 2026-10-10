import * as fs from "fs";
import * as path from "path";
import { z } from "zod";
import { kGameGraphActions } from "../game-graph/actions.js";
import { compileGameGraphCpp } from "../game-graph/compile-cpp.js";
import {
  type GameGraphLink,
  makeGameGraphNode,
  nextGraphId,
  readGameGraph,
  resolveGraphPath,
  snapGameGraphCoordinate,
  writeGameGraph,
} from "../game-graph/model.js";
import { contentRoot, ensureDir } from "../paths.js";
import { defineTool, type ToolDef, ToolError } from "../registry.js";

// These tools edit .graph documents and generate their C++ glue directly on
// disk — no editor connection is involved.

export const tools: ToolDef[] = [
  defineTool({
    name: "list_game_graph_actions",
    category: "graph",
    description: "List precompiled game graph action types available for game_logic graphs, grouped like an action browser.",
    schema: {
      category: z.string().optional().describe("Optional category filter, e.g. Audio, Transform, Physics, Animation"),
    },
    run: async ({ category }) => ({
      data: category
        ? kGameGraphActions.filter(a => a.category.toLowerCase() === category.toLowerCase())
        : kGameGraphActions,
    }),
  }),

  defineTool({
    name: "get_game_graph",
    category: "graph",
    description: "Read a game_logic .graph file and return its nodes, links, variables, and viewport.",
    schema: { path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)") },
    run: async ({ path: graphPath }) => ({ data: readGameGraph(graphPath) }),
  }),

  defineTool({
    name: "add_game_graph_node",
    category: "graph",
    actsOnHost: true,
    description: "Add a precompiled action node to a game_logic .graph file. Pins/default parameters are filled from the action catalog.",
    schema: {
      path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)"),
      typeId: z.string().describe("Action type id from list_game_graph_actions, e.g. PlaySound"),
      nodeId: z.string().optional().describe("Optional explicit node id. Defaults to next node_N."),
      x: z.coerce.number().optional().describe("Graph X position"),
      y: z.coerce.number().optional().describe("Graph Y position"),
      parameters: z.record(z.string()).optional().describe("Parameter overrides as string values"),
    },
    run: async ({ path: graphPath, typeId, nodeId, x, y, parameters }) => {
      const model = readGameGraph(graphPath);
      const ids = new Set(model.nodes.map(n => n.id));
      const id = nodeId ?? nextGraphId("node", ids);
      if (ids.has(id)) throw new ToolError(`Node id '${id}' already exists.`);
      const node = makeGameGraphNode(typeId, id, x ?? 80, y ?? 80, parameters ?? {});
      model.nodes.push(node);
      writeGameGraph(graphPath, model);
      return { data: { path: resolveGraphPath(graphPath), node } };
    },
  }),

  defineTool({
    name: "update_game_graph_node",
    category: "graph",
    actsOnHost: true,
    description: "Update an existing game graph node's type, position, and/or parameters.",
    schema: {
      path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)"),
      nodeId: z.string().describe("Node id to update"),
      typeId: z.string().optional().describe("Optional new action type id"),
      x: z.coerce.number().optional().describe("Optional new graph X position"),
      y: z.coerce.number().optional().describe("Optional new graph Y position"),
      parameters: z.record(z.string()).optional().describe("Parameter values to merge into the node"),
      replaceParameters: z.coerce.boolean().optional().describe("If true, replace parameters instead of merging"),
    },
    run: async ({ path: graphPath, nodeId, typeId, x, y, parameters, replaceParameters }) => {
      const model = readGameGraph(graphPath);
      const node = model.nodes.find(n => n.id === nodeId);
      if (!node) throw new ToolError(`Node '${nodeId}' not found.`);
      if (typeId !== undefined && typeId !== node.typeId) {
        const replacement = makeGameGraphNode(typeId, node.id, node.positionX, node.positionY, parameters ?? {});
        node.typeId = replacement.typeId;
        node.pins = replacement.pins;
        node.parameters = replacement.parameters;
      } else if (parameters !== undefined) {
        node.parameters = replaceParameters ? { ...parameters } : { ...(node.parameters ?? {}), ...parameters };
      }
      if (x !== undefined) node.positionX = snapGameGraphCoordinate(x);
      if (y !== undefined) node.positionY = snapGameGraphCoordinate(y);
      writeGameGraph(graphPath, model);
      return { data: { path: resolveGraphPath(graphPath), node } };
    },
  }),

  defineTool({
    name: "delete_game_graph_node",
    category: "graph",
    actsOnHost: true,
    description: "Delete a node from a game_logic graph and remove links attached to it.",
    schema: {
      path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)"),
      nodeId: z.string().describe("Node id to delete"),
    },
    run: async ({ path: graphPath, nodeId }) => {
      const model = readGameGraph(graphPath);
      const beforeNodes = model.nodes.length;
      const beforeLinks = model.links.length;
      model.nodes = model.nodes.filter(n => n.id !== nodeId);
      model.links = model.links.filter(l => l.sourceNodeId !== nodeId && l.targetNodeId !== nodeId);
      writeGameGraph(graphPath, model);
      return { data: { removedNodes: beforeNodes - model.nodes.length, removedLinks: beforeLinks - model.links.length } };
    },
  }),

  defineTool({
    name: "link_game_graph_nodes",
    category: "graph",
    actsOnHost: true,
    description: "Create or replace a link between two game graph node pins.",
    schema: {
      path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)"),
      sourceNodeId: z.string().describe("Source node id"),
      sourcePinId: z.string().describe("Source output pin id"),
      targetNodeId: z.string().describe("Target node id"),
      targetPinId: z.string().describe("Target input pin id"),
      replaceExistingInput: z.coerce.boolean().optional().describe("Replace any existing link into the target input pin (default true)"),
    },
    run: async ({ path: graphPath, sourceNodeId, sourcePinId, targetNodeId, targetPinId, replaceExistingInput }) => {
      const model = readGameGraph(graphPath);
      if (!model.nodes.some(n => n.id === sourceNodeId)) throw new ToolError(`Source node '${sourceNodeId}' not found.`);
      if (!model.nodes.some(n => n.id === targetNodeId)) throw new ToolError(`Target node '${targetNodeId}' not found.`);
      if (replaceExistingInput ?? true)
        model.links = model.links.filter(l => !(l.targetNodeId === targetNodeId && l.targetPinId === targetPinId));
      const id = nextGraphId("link", new Set(model.links.map(l => l.id)));
      const link: GameGraphLink = { id, sourceNodeId, sourcePinId, targetNodeId, targetPinId };
      model.links.push(link);
      writeGameGraph(graphPath, model);
      return { data: { path: resolveGraphPath(graphPath), link } };
    },
  }),

  defineTool({
    name: "write_game_graph_cpp",
    category: "graph",
    actsOnHost: true,
    description: "Generate precompiled C++ glue for a game_logic graph and write it to disk. This is the fast-play path source that calls native action implementations.",
    schema: {
      path: z.string().describe("Path to a .graph asset, relative to the project (the repository when the server serves no project)"),
      outputPath: z.string().describe("Path for the generated .cpp file, relative to the project"),
      functionName: z.string().optional().describe("Generated function name (default ExecuteGameLogicGraph)"),
      overwrite: z.coerce.boolean().optional().describe("Replace the file when it exists (default false: an existing file is refused)"),
    },
    run: async ({ path: graphPath, outputPath, functionName, overwrite }) => {
      const model = readGameGraph(graphPath);
      const cpp = compileGameGraphCpp(model, functionName ?? "ExecuteGameLogicGraph");
      const fullOut = resolveGraphPath(outputPath);
      if (!overwrite && fs.existsSync(fullOut))
        throw new ToolError(`${fullOut} exists: pass overwrite: true to replace it.`);
      ensureDir(path.dirname(fullOut));
      fs.writeFileSync(fullOut, cpp, "utf-8");
      return { data: { outputPath: fullOut, bytes: Buffer.byteLength(cpp, "utf-8") } };
    },
  }),

  defineTool({
    name: "write_game_graph_action_code",
    category: "graph",
    actsOnHost: true,
    description: "Create a C++ custom action implementation for a game graph action, or replace an existing one when overwrite is true. Use this when changing the native code behind a node/action.",
    schema: {
      typeId: z.string().describe("Action type id, e.g. PlaySound or MyCustomAction"),
      code: z.string().describe("C++ function body statements. The generated function receives GameLogicRuntimeContext& ctx."),
      outputDir: z.string().optional().describe("Directory, relative to the project. Default: Assets/Scripts/GameGraphActions"),
      overwrite: z.coerce.boolean().optional().describe("Replace the action's file when it exists (default false: an existing file is refused)"),
    },
    run: async ({ typeId, code, outputDir, overwrite }) => {
      const cleanType = typeId.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[0-9]/, "_$&");
      if (!cleanType) throw new ToolError("typeId must contain at least one identifier character.");
      const dir = outputDir ? resolveGraphPath(outputDir) : path.join(contentRoot(), "Assets", "Scripts", "GameGraphActions");
      const filePath = path.join(dir, `${cleanType}.cpp`);
      if (!overwrite && fs.existsSync(filePath))
        throw new ToolError(`${filePath} exists: pass overwrite: true to replace it.`);
      ensureDir(dir);
      const source = `#include "Graph/GameLogicRuntime.h"

namespace GameEngine::GameGraphActions
{

void ${cleanType}(GameLogicRuntimeContext& ctx)
{
${code.split("\n").map(line => `    ${line}`).join("\n")}
}

} // namespace GameEngine::GameGraphActions
`;
      fs.writeFileSync(filePath, source, "utf-8");
      return { data: { path: filePath, bytes: Buffer.byteLength(source, "utf-8") } };
    },
  }),
];
