import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

/**
 * A world-XZ list, e.g. `[[12.5, -40.0], [13.0, -40.0]]`.
 *
 * A factory rather than a shared const, for the reason vec3Tuple() documents:
 * one zod instance reused across two properties emits a `$ref` for the second
 * instead of inlining it.
 */
function xzPointList() {
  return z.preprocess(
    (val) => {
      if (typeof val === "string") {
        try { return JSON.parse(val); } catch { return val; }
      }
      return val;
    },
    z.array(z.array(z.coerce.number()).length(2)),
  );
}

export const tools: ToolDef[] = [
  proxyTool({
    name: "sample_ground_height",
    category: "scene",
    description:
      "Sample the COMPOSED ground height at world XZ points — the CPU heightfield after the modifier stack (flattens, corridors, stamps, noise), which is what the renderer draws and what conforming placement lands on. This is not the base heightmap: a scene-authored corridor exists only in the composed result, so an offline model that reads the shipped heightmap cannot see it. Terrain only — never reports a mesh, prop or bridge deck as ground. Each sample reports `inside` (within the terrain footprint at all) separately from `sampled` (a trustworthy height came back); inside-but-unsampled means a tile has not streamed in, and is NOT ground level.",
    schema: {
      points: xzPointList().describe("World XZ points as [[x, z], ...] (max 8192)"),
    },
  }),

  proxyTool({
    name: "get_placement_ground_gap",
    category: "scene",
    description:
      "Measure the SIGNED ground-to-underside gap for every piece a spline route placed: POSITIVE = floating above the composed ground, NEGATIVE = buried in it. Probes the piece's OWN underside — the oriented box's lower face, at its centre and four corners, so every probe is a point of the real underside over ground the piece actually covers — and reports per-probe x/z/undersideY/groundY/gap, per-piece minGap/maxGap/centerGap, and a route summary (worstFloating, worstBuried, each with the entity it came from). A tilted piece bedded at its middle reads a negative minGap and a positive maxGap at once; that spread is the reading. `pieces[].status` says why a piece has no numbers (noGroundSampled, undersideNearVertical, degenerateFootprint) instead of reporting a zero. This is the axis a conforming placement fails on — station tilt is anti-correlated with it, because flattening the ground under a slab lowers tilt and opens the gap at the same time. `pieces[].name` is a display label that renumbers with spacing; match on entityId.",
    schema: {
      entityId: z.coerce.number().describe("The spline route entity whose placed pieces to measure"),
    },
  }),
];
