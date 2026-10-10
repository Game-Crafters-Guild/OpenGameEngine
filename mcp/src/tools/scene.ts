import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { flexibleObject, quatSchema, vec3Schema } from "../schema.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "create_entity",
    category: "scene",
    description: "Create a new entity in the scene. A Transform is always added (identity by default). The `components` map accepts the same components as set_component: reflected ones (Camera, Light, PostProcessVolume, SkyEnvironment, VolumetricFogEffect, ...), the hand-handled ones (SplineComponent, particles, physics) and the schema-driven Terrain. Enums take the enumerator name or its integer index; see set_component for the full value vocabulary. One undo step: a following `undo` removes the entity.",
    schema: {
      name: z.string().optional().describe("Entity name"),
      position: vec3Schema.optional().describe("Position (default {x:0,y:0,z:0})"),
      rotation: quatSchema.optional().describe("Rotation quaternion (default {x:0,y:0,z:0,w:1})"),
      scale: vec3Schema.optional().describe("Scale (default {x:1,y:1,z:1})"),
      components: flexibleObject.optional().describe("Additional components, e.g. {\"Terrain\": {\"SizeX\": 1024}}"),
    },
    params: ({ name, position, rotation, scale, components }) =>
      ({ name, position, rotation, scale, components: components ?? {} }),
  }),

  proxyTool({
    name: "set_component",
    category: "scene",
    description: "Set or update a component on an entity, adding it if absent. Reflected components (Name, Camera, Light, MeshRenderer, TerrainGrass, PostProcessVolume, SkyEnvironment, VolumetricFogEffect, ...) are set by field name, matched case-insensitively; an EntityHandle field such as SkyEnvironment.SunLight takes a numeric entity id. A few components are handled by name with exact-case keys: Transform (position, rotation, scale), SplineComponent (DefaultRadius, Enabled, closed, type, points), and the physics components (PhysicsBody: enabled, mass, linearDamping, angularDamping, gravityScale, centerOfMassOffset, motionType as \"Static\"/\"Kinematic\"/\"Dynamic\" or 0/1/2 — any other motionType is an error; PhysicsCollider: enabled, isTrigger; Box/Sphere/CapsuleColliderShape: their camelCase field names). Terrain has no field table and is set through its scene schema. Enums take the enumerator NAME, matched case-insensitively ({\"RenderMode\": \"Blend\"} or \"blend\"), or its integer index. AssetRef fields (textures, materials, models) take a GUID string, {\"guid\": \"...\"}, or an asset path to resolve; \"\" CLEARS the reference (the form a read emits for an unset one), while JSON null means \"no value supplied\" and leaves it alone. get_entity_components output is therefore writable back as-is, including for a default component. Fields with authored ranges are clamped to them, so a value out of range is accepted and bounded rather than refused. Pass field values as object, e.g. {\"ExposureControl\": 3}. Omit values or pass {} to add a component with defaults. Any key naming no writable field is reported as an error rather than silently ignored. One undo step: a following `undo` puts back the values from before, and removes the component if the call added it.",
    schema: {
      entityId: z.coerce.number().describe("Entity handle ID"),
      component: z.string().describe("Component name"),
      values: flexibleObject.optional().describe("Component field values to set (object or JSON string)"),
    },
    params: ({ entityId, component, values }) => ({ entityId, component, values: values ?? {} }),
  }),

  proxyTool({
    name: "delete_entity",
    category: "scene",
    description: "Delete an entity (and its subtree) from the scene, routed through the editor undo system — exactly like the Hierarchy/Scene-View delete key. The delete lands on the undo stack, so a following `undo` restores it and `redo` deletes it again. Returns deletedCount and canUndo.",
    schema: {
      entityId: z.coerce.number().describe("Entity handle ID (uint32); the full parent+descendant subtree is deleted, like the delete key"),
    },
  }),

  proxyTool({
    name: "select_entity",
    category: "scene",
    description: "Select an entity in the editor (updates Inspector and Scene View gizmos)",
    schema: { entityId: z.coerce.number().describe("Entity handle ID") },
  }),

  proxyTool({
    name: "get_entity_components",
    category: "scene",
    description: "Get all component data for a specific entity. Reflected components read back by field name in canonical PascalCase — the same shape set_component accepts, so output feeds straight back in. Enums read as their enumerator name, a set AssetRef as a 32-char hex GUID and an unset one as \"\" (all forms set_component parses back). A field whose type has no JSON form, and any component with no reflected field table at all (physics, and anything the component scanner skipped), reads as {size, data} with data an opaque hex dump of its bytes.",
    schema: { entityId: z.coerce.number().describe("Entity handle ID (uint32)") },
  }),

  proxyTool({
    name: "query_ecs",
    category: "scene",
    description: "Find entities that have a specific component",
    schema: {
      hasComponent: z.string().describe("Component name (e.g. Position, Light, Camera)"),
      limit: z.coerce.number().optional().describe("Max results (default 50)"),
    },
    params: ({ hasComponent, limit }) => ({ hasComponent, limit: limit ?? 50 }),
  }),

  proxyTool({
    name: "save_scene",
    category: "scene",
    description: "Save the active scene document. With `path`, saves-as to that path (project-relative resolves under the asset root, .scene enforced) and adopts it. Without `path`, saves to the current scene path (errors if the scene is untitled). Routes through the same save as Ctrl+S (hierarchy-UI capture + MarkClean). REFUSES while play mode is active (get_editor_state `playMode` is anything but `editing`), with or without `path`: play simulates the authored world in place, so a save would write runtime state over the scene file and stopping play would then discard it. `force` does NOT override this - exit play mode first (set_play_mode action=exit, twice if it lands in change_review). REFUSES a scene that loaded DEGRADED and still has unresolved assignments (get_editor_state `sceneDegraded.outstandingCount` above zero — NOT merely `sceneDegraded` non-null, which stays set for the whole session to match the title) when the target is that scene's own file: saving would write the assignments this build could not read back as defaults. The error names how many would be lost. Read `sceneDegraded.skips` and decide — then either save to a DIFFERENT path (always allowed, leaves the original intact) or pass force=true to accept the loss.",
    schema: {
      path: z.string().optional().describe("Save-as target path (absolute or project-relative). Omit to save to the current scene path."),
      force: z.boolean().optional().describe("Overwrite the source of a degraded scene, accepting the loss of whatever this build could not read. Only needed when the save target is the degraded document's own file."),
    },
  }),

  proxyTool({
    name: "undo",
    category: "scene",
    description: "Invoke the editor's real undo stack (identical to Ctrl+Z and the toolbar Undo). Runs on the editor main thread. Returns what was undone and the new stack tops.",
    schema: {},
  }),

  proxyTool({
    name: "redo",
    category: "scene",
    description: "Invoke the editor's real redo stack (identical to Ctrl+Y and the toolbar Redo). Runs on the editor main thread. Returns what was redone and the new stack tops.",
    schema: {},
  }),

  proxyTool({
    name: "get_undo_stack",
    category: "scene",
    description: "Dump the editor undo/redo stacks. Each entry has its command type (e.g. DeleteEntitiesCommand, WorldSnapshotCommand, Compound), a label, and (undo side) a timestamp. `cursor` = number of applied commands (boundary between undo and redo). Use to see which command a delete/undo produced.",
    schema: {},
  }),
];
