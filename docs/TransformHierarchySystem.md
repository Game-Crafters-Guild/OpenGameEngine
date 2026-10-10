# Transform Hierarchy

Parenting an entity makes its transform relative to its parent. `Transform` is always
local space, `WorldTransform` is the derived world-space matrix that rendering, bounds
and physics read, and `TransformHierarchySystem` is what turns one into the other. This
page is the contract between those three: which component you write, which one you read,
and when the derived value is valid.

## Architecture

- `Transform` stores the local-space 4x4 matrix.
- `WorldTransform` stores the derived world-space matrix used by rendering and other world-space consumers.
- `Parent` defines transform hierarchy edges as well as editor hierarchy relationships.
- `TransformHierarchySystem` owns propagation and version/dirty-feed emission.

## Component Semantics

### Transform (local)

- Interpreted as a **local transform relative to the parent**:
  - For root entities (no `Parent` or `Parent.parent == 0`): local == world.
  - For children: `WorldTransform = ParentWorldTransform * Transform`.
- Stores a full 4x4 matrix (float[16]) to keep compatibility with existing code and GPUScene.
- Changes are detected through ECS component-column versions and an exact cached local-matrix snapshot; no per-component `isDirty` flag is required.

### WorldTransform (world)

- New ECS component holding the **world-space** 4x4 matrix for an entity.
- Owned and maintained **only** by `TransformHierarchySystem` (and possibly physics/animation systems in clearly defined ways).
- Consumers such as:
  - Render extraction / GPUScene upload,
  - World bounds / culling systems,
  - Physics or gameplay code that needs world-space transforms,
  should read from `WorldTransform`, not from `Transform`.

### Parent

- Existing component that encodes hierarchy:
  - `ECS::EntityHandle parent; // 0 = no parent / root`
- `TransformHierarchySystem` will interpret this as the parent in transform space.
- Editor hierarchy UI can continue to use it for logical structure; transform semantics now line up with that UI.

## TransformHierarchySystem Responsibilities

- Run once per frame after local transform writers.
- Ensure every entity with `Transform` also has `WorldTransform`.
- Resolve every entity **without a `Parent` component** on the flat parallel pass: `WorldTransform = Transform` for changed chunks only, spread across the job system. This holds whether or not hierarchies exist elsewhere in the world, so adding one parented entity never slows the rest of the world down.
- Keep a cached direct entity-index lookup and parent-to-child topology for the **hierarchy members**: the entities that have a `Parent` component and the roots above them. Rebuild it only after a `Parent` edit or an ECS structural change.
- On ordinary transform edits:
  1. Use `Changed<Transform>` to skip unchanged chunks of entities with a `Parent`, and compare cached local matrices to find the exact entities that changed.
  2. Add the hierarchy roots whose world matrix the flat pass just changed.
  3. Keep only the topmost of them: an entity under a changed ancestor is covered by that ancestor's walk.
  4. Walk those subtrees iteratively, rather than recursively, and on the job system when there are many of them. For each visited entity:
     - Read local-space `Transform`.
     - Read the parent's `WorldTransform` (if any).
     - Write `WorldTransform` as:
       - `world = local` for a root with an unset `Parent` handle (a root without a `Parent` component was already written by the flat pass);
       - `world = parentWorld * local` for children.

A still frame costs nothing beyond the changed-chunk checks. Multiple edited entities are supported naturally: independent selections walk independent subtrees, and when a parent and one of its descendants change in the same frame, the branch is walked once, from the parent.

## Update Order and Integration

`TransformHierarchySystem` is part of the default world's schedule, registered in
`RegisterRenderingSystems.cpp` so that:

1. **Local-space updaters** run first
   - Movement / animation / gameplay systems that modify `Transform` (local) run **before** the hierarchy system.
2. **TransformHierarchySystem** runs next
   - Computes `WorldTransform` for all entities using `Transform` + `Parent`.
3. **World-space consumers** run after
   - Systems that depend on world-space data (e.g., bounds computation, visibility/culling, GPUScene transform upload, editor gizmo placement) read `WorldTransform`.

Concretely, the order relative to the rendering systems is:

- `TransformHierarchySystem`
- `RenderExtractionSystem` (writes GPUScene instances from `WorldTransform`)
- `RenderGraphBuildSystem` (consumes GPUScene + view/camera data)

## Edge Cases and Debugging

- **Missing Parent**
  - If `Parent.parent` references an entity that does not exist or lacks `Transform`, the topology rebuild treats the child as a root (`world = local`) and logs a warning naming both entities, in every build.

- **Cycles in the hierarchy**
  - Cycles are invalid (e.g., A is parent of B and B is parent of A).
  - The topology rebuild detects them: it walks parent links from every node, marking the nodes on the current walk, so a link back into that walk is a cycle. It then clears the parent edge of the node the walk re-entered — that entity becomes a root — and logs an error naming it. The break lives in the cached topology, so it holds until the next rebuild rather than being redone each frame, and the cached graph is always a forest, which is what keeps the dirty walks iterative and bounded.

- **Dynamic reparenting**
  - When `Parent` changes at runtime, the system recomputes `WorldTransform` using the new hierarchy on the next update.
  - In the editor, this makes drag-and-drop reparenting in the hierarchy immediately visible via child movement.

## Who reads WorldTransform

Render extraction reads `WorldTransform` when it uploads GPUScene rows, and views and
cameras read it for their world placement. Both observe the propagation order above, so
a system that wants the world matrix of an entity it just moved must run after
`TransformHierarchySystem`, not before it.
