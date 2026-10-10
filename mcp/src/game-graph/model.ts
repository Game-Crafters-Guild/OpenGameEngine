import * as fs from "fs";
import { ensureDir, resolveContentPath } from "../paths.js";
import * as path from "path";
import { findGameGraphAction, type GameGraphPin } from "./actions.js";

// Read/write side of a .graph document, plus the id and grid-snap rules new
// nodes must follow to match what the editor produces.

export interface GameGraphNode {
  id: string;
  typeId: string;
  positionX: number;
  positionY: number;
  pins: GameGraphPin[];
  parameters?: Record<string, string>;
}
export interface GameGraphLink {
  id: string;
  sourceNodeId: string;
  sourcePinId: string;
  targetNodeId: string;
  targetPinId: string;
}
export interface GameGraphModel {
  kind: string;
  viewport?: { panX?: number; panY?: number; zoom?: number };
  nodes: GameGraphNode[];
  links: GameGraphLink[];
  variables?: unknown[];
  textures?: unknown[];
}

export function resolveGraphPath(input: string): string {
  return resolveContentPath(input);
}

export function readGameGraph(input: string): GameGraphModel {
  const full = resolveGraphPath(input);
  const model = JSON.parse(fs.readFileSync(full, "utf-8")) as GameGraphModel;
  if (model.kind !== "game_logic") throw new Error(`Graph '${input}' is not kind=game_logic.`);
  model.nodes ??= [];
  model.links ??= [];
  model.variables ??= [];
  model.textures ??= [];
  model.viewport ??= { panX: 0, panY: 0, zoom: 1 };
  return model;
}

export function writeGameGraph(input: string, model: GameGraphModel): void {
  const full = resolveGraphPath(input);
  ensureDir(path.dirname(full));
  fs.writeFileSync(full, `${JSON.stringify(model, null, 2)}\n`, "utf-8");
}

export function nextGraphId(prefix: string, existing: Set<string>): string {
  let index = 0;
  while (existing.has(`${prefix}_${index}`)) index++;
  return `${prefix}_${index}`;
}

const kGameGraphGridSize = 20;

export function snapGameGraphCoordinate(value: number): number {
  return Math.round(value / kGameGraphGridSize) * kGameGraphGridSize;
}

export function makeGameGraphNode(typeId: string, nodeId: string, x: number, y: number, parameters: Record<string, string>): GameGraphNode {
  const action = findGameGraphAction(typeId);
  return {
    id: nodeId,
    typeId,
    positionX: snapGameGraphCoordinate(x),
    positionY: snapGameGraphCoordinate(y),
    pins: action.pins.map(p => ({ ...p })),
    parameters: { ...action.defaults, ...parameters },
  };
}
