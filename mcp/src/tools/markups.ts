import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { vec3Tuple } from "../schema.js";
import { proxyTool } from "./proxy.js";

/**
 * World mark-ups: places in the world the user and the agent talk about. A mark-up
 * has a title, a status from the project's status group (Proposed, Requested,
 * InProgress, Complete, Revision, Problem by default), free tags, a description and
 * a thread. The user drags, comments and changes status in the editor; the agent
 * reads what changed with markup_list(since) and answers with markup_update and
 * markup_comment. Every write the port makes directly is stamped Agent, unless
 * `author: "User"` is passed for a scripted demonstration; an edit made through injected
 * input (send_key, click_element, perform_drop) counts as the user's. User guide:
 * docs/Editor/world-markups.html.
 */

function author() {
  return z.enum(["User", "Agent"]);
}

function xzPoint() {
  return z.array(z.coerce.number()).min(2).max(3);
}

function outline() {
  return z.array(xzPoint());
}

function members() {
  return z.array(
    z.object({
      entityId: z.coerce.number().optional(),
      tag: z.string().optional(),
      mode: z.enum(["include", "exclude"]),
    }),
  );
}

const REGION =
  "A region is an area of ground: an outline of 3 to 256 [x, z] points in meters, in order around the area, that does not cross itself. It draws as a translucent wall extrudeHeight meters tall standing on the ground (or the sea surface), with a lid that follows the ground. Use it for areas (a forest, a district, a river with its banks); use a box or sphere for a place. A clearing or any other cut is an Exclude member (members, or exclusions as polygons); another mark-up joined to the area is an Include member. Test points with markup_contains, which applies the members.";

const PATH =
  " A path is a way along the ground: 2 to 256 [x, y, z] points in meters, in order along it (kind path, points, type linear or smooth). It draws as a glowing line draped on the terrain. Use it for a river, a road or the route to a place; it encloses no area, so markup_contains refuses it.";

export const tools: ToolDef[] = [
  proxyTool({
    name: "markup_list",
    category: "scene",
    description:
      "List the open scene's mark-ups, newest change first. Returns the scene's `revision`, the `epoch` and one row per mark-up (entityId, tag, title, kind, status, tags, author, updatedBy, created, updated, revision, hidden, hasUpdate, center, radius, entryCount). To see what the user changed since your last look, pass the `revision` your previous answer returned as `since` (strictly after; never a clock) with `changedBy: \"User\"`. Hidden mark-ups are listed and say so.",
    schema: {
      status: z.string().optional().describe("Only this status, by name (e.g. \"Requested\")"),
      author: author().optional().describe("Only mark-ups created by this author"),
      since: z.coerce.number().int().nonnegative().optional().describe("Only rows changed after this scene revision"),
      changedBy: author().optional().describe("Only rows whose last change was by this author"),
      includeHidden: z.boolean().optional().describe("List hidden mark-ups too (default true)"),
    },
  }),

  proxyTool({
    name: "markup_get",
    category: "scene",
    description:
      "One mark-up in full, as `markup`: its row, description, thread (`entries`: kind created/comment/statusChange/edit, author, time, status, text; an edit's `changes`: converted, moved, resized, rotated, renamed, reshaped, recolored, described, its `text` the name a rename gave and its `color` (#RRGGBB) the color a recolor gave) and shape (`shape`: box or sphere, center, halfExtents, rotation [x,y,z,w]; for a region `shape: \"region\"`, `outline` (the evaluated ring in world [x, z], at most 256 points, what markup_contains tests), `knots` (what markup_update takes back as outline), type, extrudeHeight, area (square meters) and perimeter (meters, every ring), both with the members applied as markup_contains tests them (a sphere's circle as its 32-gon), bounds {min, max}, groundRange {min, max} (the lowest and highest heights the region stands on, meters) and `members`: {entityId, mode, deleted, title, tag, kind, footprint: {outline} or {center, radius}}; for a path `shape: \"path\"`, `points` (world [x, y, z]), type and length); beside it the scene's `revision` and the `epoch`.",
    schema: {
      entityId: z.coerce.number().describe("The mark-up's entityId from markup_list"),
    },
  }),

  proxyTool({
    name: "markup_create",
    category: "scene",
    description:
      "Mark a place (kind volume: a box or sphere), an area (kind region: an outline) or a way (kind path: points along it) with a title, status Proposed unless given, authored by the agent, as one undo step. Use it to propose a plan the user can drag, comment on and set to Requested. " +
      REGION +
      PATH +
      " Returns `entityId`, and `exclusionIds` for the regions `exclusions` created (parented under the region).",
    schema: {
      title: z.string().describe("The place's name, e.g. \"Village center\""),
      kind: z.enum(["volume", "region", "path"]).optional().describe("Default volume"),
      points: z.array(vec3Tuple()).optional().describe("A path's points: 2 to 256 world [x, y, z] in meters, in order along the way; the line drapes on the terrain beneath them"),
      shape: z.enum(["box", "sphere"]).optional().describe("A volume's shape; default box"),
      center: vec3Tuple().optional().describe("A volume's world [x, y, z] in meters"),
      halfExtents: vec3Tuple().optional().describe("A volume's half size [x, y, z] in meters; a sphere's radius is x"),
      outline: outline().optional().describe("A region's outline: 3 to 256 world [x, z] points (y ignored if given) in order around the area"),
      type: z.enum(["linear", "smooth"]).optional().describe("A region's outline or a path: linear (default, drawn exactly as sent) or smooth"),
      extrudeHeight: z.coerce.number().optional().describe("A region's wall height in meters, 1 to 100; default 8"),
      members: members().optional().describe("Mark-ups the region includes or excludes, by entityId or tag; at most 32 with exclusions"),
      exclusions: z.array(outline()).optional().describe("Polygons cut out of the region: each becomes an Exclude region parented under it, in the same undo step"),
      rotation: z.array(z.coerce.number()).length(4).optional().describe("Quaternion [x, y, z, w]"),
      description: z.string().optional().describe("What the place is, or the work it asks for"),
      status: z.string().optional().describe("A status by name; default Proposed"),
      color: z.array(z.coerce.number()).length(4).optional().describe("RGBA 0..1; [0,0,0,0] draws the status color"),
      author: author().optional().describe("Default Agent; User only for a scripted demonstration"),
    },
  }),

  proxyTool({
    name: "markup_update",
    category: "scene",
    description:
      "Change a mark-up: any of title, status (appends a status change to the thread), description, tags (free tags by name; a new name joins the project's vocabulary), color, a volume's shape (shape, center, halfExtents, rotation), a region's outline, type, extrudeHeight, members (the full list) and exclusions (new Exclude regions appended to the list), and hidden. `kind: \"region\"` converts a volume to a region, one way (a box without an outline keeps its outline from above and its height); `kind: \"path\"` with `points` (and `type`) converts a volume to a path, one way; either way the title, notes, status, color and thread stay. A region is always closed: `closed: false` is refused. A path keeps its kind and its points here (its shape, outline and volume fields are refused); the user moves them with Edit path. Edit mark-ups through this rather than set_component. Returns the row as `markup`, the scene's `revision` and the `epoch`. Stamped Agent unless `author: \"User\"`. One undo step, whatever it changes; hidden is a view state outside it.",
    schema: {
      entityId: z.coerce.number().describe("The mark-up's entityId"),
      title: z.string().optional(),
      status: z.string().optional().describe("A status by name, e.g. \"InProgress\""),
      description: z.string().optional(),
      tags: z.array(z.string()).optional().describe("The full list of free tags"),
      color: z.array(z.coerce.number()).length(4).optional(),
      shape: z.enum(["box", "sphere"]).optional(),
      center: vec3Tuple().optional(),
      halfExtents: vec3Tuple().optional(),
      rotation: z.array(z.coerce.number()).length(4).optional(),
      kind: z.enum(["volume", "region", "path"]).optional().describe("\"region\" or \"path\" converts a volume"),
      points: z.array(vec3Tuple()).optional().describe("With kind path: the path's points, 2 to 256 world [x, y, z] in meters, in order along the way"),
      outline: outline().optional().describe("A region's new outline, world [x, z] points"),
      type: z.enum(["linear", "smooth"]).optional(),
      extrudeHeight: z.coerce.number().optional(),
      members: members().optional().describe("The region's full member list"),
      exclusions: z.array(outline()).optional().describe("Polygons to cut out, each a new Exclude region"),
      closed: z.boolean().optional().describe("Only true; a region is always closed"),
      hidden: z.boolean().optional().describe("A view state, not a scene edit"),
      author: author().optional().describe("Default Agent; User only for a scripted demonstration"),
    },
  }),

  proxyTool({
    name: "markup_comment",
    category: "scene",
    description:
      "Add a comment to a mark-up's thread. A URL in the text (http:// or https://) shows as a link the user can open; `[[entity:N]]`, N an entityId from markup_list or get_scene_hierarchy, shows as a chip with the entity's current name that selects it on a click and has a frame glyph that frames it. Stamped Agent unless `author: \"User\"`.",
    schema: {
      entityId: z.coerce.number().describe("The mark-up's entityId"),
      text: z.string().describe("The comment"),
      author: author().optional().describe("Default Agent; User only for a scripted demonstration"),
    },
  }),

  proxyTool({
    name: "markup_contains",
    category: "scene",
    description:
      "Which points lie inside a mark-up's area, by the editor's own test: for a region, (its outline or any Include member) and not any Exclude member, a member region counting its own outline only and a deleted member skipped; for a box or sphere, its outline on the ground; a path encloses no area and is refused. Heights are ignored. Returns `inside` (one true or false per point, in order) and `insideCount`. At most 8192 points per call.",
    schema: {
      entityId: z.coerce.number().describe("The mark-up's entityId"),
      points: z.array(xzPoint()).describe("World [x, z] points in meters ([x, y, z] accepted, y ignored)"),
    },
  }),

  proxyTool({
    name: "markup_frame",
    category: "view",
    description:
      "Point the Scene View camera at a mark-up: look_at's pose over the shape's bounding sphere, from 1.8 radii unless `distance` is given. Moves the camera only; never changes the window focus.",
    schema: {
      entityId: z.coerce.number().describe("The mark-up's entityId"),
      direction: vec3Tuple().optional().describe("From the camera toward the mark-up (default [1, -0.5, 1])"),
      distance: z.coerce.number().optional().describe("Camera distance in meters"),
    },
  }),

  proxyTool({
    name: "markup_set_visible",
    category: "view",
    description:
      "Show or hide one mark-up (`entityId`) or all (`all: true`) in the editor's views. A view state: no scene edit, no undo step; hidden mark-ups are still listed.",
    schema: {
      entityId: z.coerce.number().optional(),
      all: z.boolean().optional(),
      visible: z.boolean().describe("true shows, false hides"),
    },
  }),
];
