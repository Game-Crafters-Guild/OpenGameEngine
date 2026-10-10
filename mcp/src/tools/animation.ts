import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { vec3Schema } from "../schema.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "spawn_skinned_model",
    category: "animation",
    // The debug server still calls this handler by its original test-pair name.
    ipcMethod: "spawn_humanoid_test_pair",
    description: "Spawn a skinned-mesh entity from a ModelAsset (FBX/glTF) with optional AnimationClip wired into AnimatorRef. Mirrors the editor's drag-FBX-into-scene flow: invokes ModelEntityFactory::CreateFromModel which adds Transform + SkinnedMeshRenderer + SkeletonRef + AnimatorRef on every skinned submesh entity. If the supplied clipPath's source skeleton differs from the model's (cross-rig), AnimationSystem's auto-bootstrap fires on the next frame and routes evaluation through HumanoidRetargetSystem. Use this for end-to-end retargeting verification (e.g. BusinessMale.fbx + A_Walk_F_Masc.fbx).",
    schema: {
      modelPath: z.string().describe("Asset-relative path to the model FBX/glTF (e.g. 'Models/FBXTest/BusinessMale.fbx')"),
      clipPath: z.string().optional().describe("Asset-relative path to the AnimationClip source. If a Model is given, the first embedded clip is used. Omit for bind-pose."),
      name: z.string().optional().describe("Entity root name (default: file stem)"),
      position: vec3Schema.optional().describe("World position (default origin)"),
      loop: z.coerce.boolean().optional().describe("Loop the clip (default true)"),
      paused: z.coerce.boolean().optional().describe("Start paused (default false)"),
      speed: z.coerce.number().optional().describe("Playback speed multiplier (default 1.0)"),
    },
  }),

  proxyTool({
    name: "get_skeleton_store_state",
    category: "animation",
    description: "Get skeleton store diagnostics: per-skeleton, per-world runtime state (palette sizes, atlas offsets, GPU buffers). Use to verify multi-world skinning isolation and cleanup.",
    schema: {},
  }),
];
