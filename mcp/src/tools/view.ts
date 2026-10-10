import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { vec3Tuple } from "../schema.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_camera",
    category: "view",
    description:
      "Read the selected view's camera (scene by default). Scene View reports position, yawDeg, pitchDeg, distance, " +
      "is2D and its last projection. Game View reports its last declared camera: position, column-major 4x4 " +
      "worldTransform including roll, viewId, frameIndex, source=last_declared and waitingForExtraction. Its exposure " +
      "contains the camera mode and nullable metered {linearScale, ev100, frameIndex}: the latest completed GPU " +
      "sample can lag the declared camera frame. Neither frame proves presentation completion. No Game camera " +
      "declaration returns a refusal; an unavailable meter returns null. Both views' projection reports matrix-derived " +
      "aspect and fovVerticalDeg/fovHorizontalDeg, or orthoHeight without angles. viewportWidthPx/viewportHeightPx " +
      "describe the projection viewport, which can differ from panel pixels. Scene projection is null before its first frame.",
    schema: {
      view: z.enum(["scene", "game"]).optional().describe("View to read; defaults to scene"),
    },
  }),

  proxyTool({
    name: "set_camera",
    category: "view",
    description: "Set the editor scene view camera. Omitted fields keep current values.",
    schema: {
      position: vec3Tuple().optional().describe("Camera position [x, y, z]"),
      yawDeg: z.coerce.number().optional().describe("Yaw in degrees (rotation around Y axis)"),
      pitchDeg: z.coerce.number().optional().describe("Pitch in degrees (up/down tilt)"),
      distance: z.coerce.number().optional().describe("Orbit distance from pivot"),
    },
  }),

  proxyTool({
    name: "move_camera",
    category: "view",
    description: "Smoothly animate the editor camera to a target pose using interpolation. Same params as set_camera plus optional durationSec.",
    schema: {
      position: vec3Tuple().optional().describe("Target position [x, y, z]"),
      yawDeg: z.coerce.number().optional().describe("Target yaw in degrees"),
      pitchDeg: z.coerce.number().optional().describe("Target pitch in degrees"),
      distance: z.coerce.number().optional().describe("Target orbit distance"),
      durationSec: z.coerce.number().optional().describe("Animation duration in seconds (default: 0.7)"),
    },
  }),

  proxyTool({
    name: "look_at",
    category: "view",
    description: "Point the scene camera at a world position or entity. Computes position/rotation from direction and distance.",
    schema: {
      entityId: z.coerce.number().optional().describe("Entity to look at (uses its Position component)"),
      target: vec3Tuple().optional().describe("World position [x, y, z] to look at (alternative to entityId)"),
      direction: vec3Tuple().optional().describe("Direction FROM camera TO target [x, y, z] (default: front-right-above)"),
      distance: z.coerce.number().optional().describe("Distance from target (default: 5)"),
    },
  }),

  proxyTool({
    name: "set_gizmos_visibility",
    category: "view",
    description: "Show or hide the scene view's gizmo overlays so a screenshot shows the scene rather than the editor's light rings and transform handles. An omitted group keeps its current visibility; calling with no arguments reports the current state without changing it. Always returns the resulting all/light/transform/markups state.",
    schema: {
      // Plain z.boolean(), not z.coerce.boolean(): coercion maps the string "false"
      // to true, which would silently leave the overlays in the shot.
      all: z.boolean().optional().describe("Master gate for every gizmo overlay. 'light', 'transform' and 'markups' apply only while this is true."),
      light: z.boolean().optional().describe("Light gizmos (radius rings, direction handles)."),
      transform: z.boolean().optional().describe("Transform gizmos (the move/rotate/scale handles on the selection)."),
      markups: z.boolean().optional().describe("Mark-up volumes and their labels."),
    },
  }),
];
