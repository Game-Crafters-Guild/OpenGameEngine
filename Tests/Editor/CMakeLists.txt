# Editor Test Sources (Google Test based)
#
# Note: These tests currently focus on SceneView/TransformTool behavior and do
# not depend on the legacy UISystem. They do, however, transitively depend on
# the new UI module via AssetRuntime's ParserRegistry (UILayoutAsset/UIStyleAsset
# registration).
set(EDITOR_TEST_SOURCES
	SceneViewTransformToolTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/MeasureSceneGizmo.cpp
	# The shared gizmo icon helpers: billboard basis, icon world radius, circle emitter.
	SceneViewGizmoHelpersTests.cpp
	SceneViewFramingTests.cpp
	SceneViewProjectionTests.cpp
	MarkupEditorBridgeTests.cpp
	MarkupRequestsTests.cpp
	MarkupRegionInspectorTests.cpp
	MarkupRegionSceneViewTests.cpp
	MarkupSceneViewTests.cpp
	# The debug server's request gates over a loopback connection, with the mark-ups' gate.
	DebugServerRequestGatesTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/EditorDebugServer.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/AssistantSessionHandlers.cpp
	SceneViewToolStripTests.cpp
	MarkupNotesEditCommandTests.cpp
	GizmoEllipseTests.cpp
	SceneViewPostProcessSwitchesTests.cpp
	SelectionOutlineContributorsTests.cpp
	SceneViewFarPlaneTests.cpp
	SceneViewCameraRigTests.cpp
	TerrainPickingRayTests.cpp
	ScenePickTerrainInclusionTests.cpp
	MeshPickingPendingBvhTests.cpp
	UndoRedoServiceTests.cpp
	PlayModeChangeReviewTests.cpp
	PlayModeMarkupNotesTests.cpp
	RenderPipelineInspectorTests.cpp
	InspectorSimulationRefreshTests.cpp
	InspectorSourceRowTests.cpp
	TextureCoveragePolicyNoticeTests.cpp
	ScriptVariablesEmptyStateTests.cpp
	OnDiskFileNameTests.cpp
	MeshBvhDiskFormatTests.cpp
	EntityDuplicateTests.cpp
	DefaultSceneEntitiesTests.cpp
	TerrainZoneStrokeCommandTests.cpp
	SphereSculptStrokeCommandTests.cpp
	SceneBackupZonePayloadFlushTests.cpp
	SceneBackupSphereSculptFlushTests.cpp
	SceneBackupRecoveryTests.cpp
	SceneSaveDrivesOnceTests.cpp
	SaveBlockedDuringPlayTests.cpp
	SceneLoadFailureTests.cpp
	TerrainEntityUndoResolutionTests.cpp
	# The Add Component preset vocabulary: the table pairing a terrain preset with
	# its canonical effect, and the three answers derived from it (picker catalog,
	# undo name, created-component list) — the sites that used to restate the
	# Terrain*Area enumerators as case labels. Leaf TU over World + the component
	# headers, so it compiles in rather than dragging InspectorPanel.cpp, which
	# keeps only the UI wiring.
	ComponentPresetsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/ComponentPresets.cpp
	# Shared terrain material-layer descriptors + edit helpers behind the Terrain
	# inspector's layer cards. Header-only, so there is no production TU to compile
	# here; the widget wiring itself is covered by the editor visual gate.
	TerrainLayerTests.cpp
	SceneSwapOrderingTests.cpp
	SkeletonRuntimeUndoTests.cpp
	SplineToolTests.cpp
	# Draped spline display polylines (centerline + width envelope) against the
	# shared conform ray. The drape TU shares CenterSample/HoldSurfaceAcrossGaps
	# and the world-length scaling with the placement leaf TUs below;
	# MeshPickingService/TerrainPicking below provide the raycast.
	SplineDrapeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineDrapePolylines.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineSurfaceConform.cpp
	# The settle observation's surface term: a ConformMode=None recipe takes no
	# term, so terrain churn (tile streaming, modifier bakes) cannot starve its
	# rebuilds; conforming recipes pass the world digest through.
	SplineSurfaceObservationTests.cpp
	# The observe, settle and defer gate every spline recipe controller rebuilds
	# through, over the real class (the controllers need live RenderServices).
	SplineRebuildGateTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineRebuildGate.cpp
	# Shared module suites exercise the runtime implementation through Engine.
	# Editor controller/integration tests retain the Placement forwarding headers.
	${CMAKE_SOURCE_DIR}/Engine/Modules/SplineLayout/Tests/TileLayoutTests.cpp
	# Per-station variation over that same API: the jitter/dropout/end-taper knobs
	# and their determinism contract, plus the golden that pins the DEFAULT layout
	# to what it was before the knobs existed.
	${CMAKE_SOURCE_DIR}/Engine/Modules/SplineLayout/Tests/SplineStationJitterTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Modules/SplineLayout/Tests/SeamShearTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Modules/SplineLayout/Tests/CenterlineSamplingTests.cpp
	# Fence layout: per-run station walk, bisector corner yaw, closed-loop wrap,
	# variable-length span fill and derived span frames, supplied by Engine.
	${CMAKE_SOURCE_DIR}/Engine/Modules/SplineLayout/Tests/FenceLayoutTests.cpp
	# Recipe contract: pool selection hash goldens, active-prefix/hole rules,
	# and the every-field equality guard (header-only — no extra TU).
	SplinePlacementRecipeTests.cpp
	# Same contract for the fence recipe, plus the NaN self-inequality guard
	# that keeps the settle-timer rebuild from re-arming forever.
	SplineFenceRecipeTests.cpp
	# What a fence conform ray may land on, end to end: the drape, the run fill
	# solved from the DRAPED length, and the per-station probe. Needs the world
	# fixture (terrain + picker) from the drape group above as well as the fence
	# runtime layout linked through Engine.
	FenceConformTargetTests.cpp
	# Same contract again for the generated-geometry recipe, whose every-field
	# equality guard is what keeps a mesh regenerating after a profile edit,
	# plus the NaN self-inequality guard shared with the fence. The sanitize TU
	# is a leaf over the component header only.
	SplineExtrudeRecipeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineExtrudeSanitize.cpp
	# The swept wall's recipe contract: its exact size, every field in equality,
	# the sanitizer, and the corner a wall starts with, derived from its spline's
	# type when it is added and when it is reset.
	SplineWallRecipeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineWallSanitize.cpp
	# What a recipe's declared ground authority provisions, and the bar it is held
	# to: a provisioned run must bake the hand-authored pooled flatten to the BIT,
	# or there are two shapes of graded run to keep correct instead of one. Also
	# pins the steady-state no-write, without which the run re-bakes every frame.
	SplineGroundAuthorityTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineGroundAuthority.cpp
	# The extrude rebuild's slot reconciliation and chunk carving: chunk slots
	# are addressed by index so a mid-run empty chunk leaves a hole rather than
	# shifting its neighbour's mesh into its slot, and the carve must cover the
	# whole run — shared boundaries, no head hole, capped tail folded in. Leaf
	# TU (Types.h only).
	SplineChunkSlotTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineChunkSlots.cpp
	# The chunk commit both generated-geometry controllers register their meshes
	# through: stable keys re-registered in place, holes, tail retirement and
	# the release of every key a build no longer reaches. Driven on a registry
	# with no device and a bare world.
	SplineChunkCommitTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineChunkCommit.cpp
	# The fence's cut span variants from the layout's plan to the registry: the
	# unit SplineFenceController::Rebuild hands its plan to, on a registry with
	# no device — dedupe, stable keys, release, the byte budget, open meshes.
	FenceMitreVariantsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FenceMitreVariants.cpp
	# End-to-end coverage of the extrude's None-conform station path over the
	# RiverVignette water spline: arc sampling (Spline, via Engine.dll), chunk
	# carving and the strip sweep (SplineGeometry, via Engine.dll) together must
	# sweep every metre of the authored curve.
	SplineExtrudeCoverageTests.cpp
	# The water fill through its editor entry point, against a real terrain: the
	# ROI lands on the terrain's own sample lattice, and EndTaperMetres is
	# IGNORED in FitToBanks (asserted as byte identity, with the sea-level floor
	# as the positive control that the comparison can fail at all).
	SplineFillRebuildTests.cpp
	# The refusal warnings themselves: a count and a magnitude locate nothing, so
	# each line has to carry the world position of the sample it is worst at.
	WaterFillDiagnosticsReportTests.cpp
	# Where the region STOPS, against the IslandVignette terrain rebuilt from its
	# own scene: no wet corner may stand above the elevation at which its region
	# would escape, and the region's edge over falling ground is that rim rather
	# than the MaxHalfWidth corridor.
	WaterFillTerminationTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineFillRebuild.cpp
	# The composed-ground query and the signed ground-to-underside gap, against a
	# flat synthetic base so a modifier is the only thing that can move a reading.
	# Pieces are rotated, scaled and mirrored — a level piece is the one pose where
	# a box around a piece and the piece itself agree — and both residencies run.
	PlacementGroundGapTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/PlacementGroundGap.cpp
	# Byte-identity oracle for the Channel sweep over that same run. The region
	# fill takes over FitToBanks only; this pins Channel's vertices, indices and
	# bounds to a digest captured before the fill landed, tapered and untapered.
	SplineChannelParityTests.cpp
	# The same byte-identity bar for every path and road the generic sweep builds
	# (Bevel, Crown), through the controller's own station stream: open and
	# closed, Linear and smooth, level and sloped ground, a transformed placer.
	SplineSweepParityTests.cpp
	# The swept wall's corners, closed loops, base, steps and texture coordinates,
	# held at the numbers the swept-wall design names, through the wall
	# controller's own pipeline on the same station stream.
	SplineSweepWallTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineSweepStations.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineWallBuild.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/RecipeValidation.cpp
	# Piece material binding shared by both spline controllers: which material a
	# spawned piece ends up with. The bind half touches no RenderServices, so the
	# override contract is pinned here as a leaf TU over ModelRenderSetup.
	SplinePieceMaterialTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplinePieceMaterial.cpp
	# The fence controller's emission list: stations, spans, then the crest row
	# in layout order, each role under its own mesh signature base, and a
	# registered crest at its wall's length scale. Leaf TU over plain pool
	# entries; the controller needs a live RenderServices to resolve them.
	FenceEmissionTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FenceEmission.cpp
	# A fence's piece entities survive a rebuild by what they are, not where
	# they fall in the list, so a picked span keeps its identity.
	FencePieceReuseTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FencePieceReuse.cpp
	# What a fence layout reads from its recipe, the way the controller reads it.
	FenceRecipeLayoutTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FenceRecipeLayout.cpp
	# Writing a fence's span overrides: gate, pinned piece, clear, full table.
	FenceSpanOverridesTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FenceSpanOverrides.cpp
	# A fence's span overrides follow its spline's points through insert,
	# delete and resample, and undo with them.
	FenceSpanOverrideRemapTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/FenceSpanOverrideRemap.cpp
	# Piece entities are children of the placer, so the pose both controllers
	# write is parent-local. The runtime helper uses TileLayout's TilePose; the
	# coherence half drives the real TransformHierarchySystem.
	PlacementPieceParentingTests.cpp
	# The inspector's notice control, a statement and optionally the one action
	# that resolves it: its text and action from construction, and its layout
	# over the real editor stylesheets. Its layout file is staged below.
	InspectorNoticeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/InspectorNotice.cpp
	# The two halves that keep a generated entity from being treated as authored
	# content: the hierarchy drop rule, and the Inspector notice that says what a
	# generated entity is and selects its owner. Leaf TUs over World and the UI
	# element tree.
	GeneratedEntityGuardTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyReparentRules.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/GeneratedEntityNotice.cpp
	# The notice a component carries when this build could not read one of its
	# authored values, in both its placements: inline over the row for the
	# reflection-driven path, hoisted to the component for a custom inspector,
	# which never runs that path. Same leaf-TU shape as the notice above.
	PreservedFieldNoticeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/PreservedFieldNotice.cpp
	# The Model Inspector's per-submesh LOD badge: whether the renderer draws the
	# chain the importer read. The classifier asks the shared geometry resolver,
	# so the rule an author reads is checkable without a running editor.
	SubmeshLodProvenanceTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SubmeshLodProvenance.cpp
	# The sky's Day of year field: a typed date reads as the day it falls on.
	DayOfYearTextTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/DayOfYearText.cpp
	# The control every explanatory card is built through, and the property the
	# notice tests below rest on: the copy is a Label in the tree from
	# construction, so a card is readable without a layout pass. Ships in
	# EditorSDK.dll (linked below), so there is no TU to compile in here.
	CollapsibleInfoCardTests.cpp
	# The outlined inspector action: the icon before the text, and an icon swap
	# (Pause to Play) that leaves only the new class. Ships in EditorSDK.dll too.
	InspectorActionButtonTests.cpp
	# The material inspector's undeclared-keys notice line model: an unknown key
	# gets the nearest declared name, a key matching a laneless declaration (an
	# adapter constant no surface stores) gets the declare-it fix-it, and the
	# transitional StandardPBR fill's seeded keys stay exempt. Same leaf-TU
	# shape as the notices above.
	MaterialUndeclaredKeysNoticeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/UndeclaredKeysNotice.cpp
	# The material inspector's section and line builders: the notice appends its line through
	# them, and MaterialInspectorLayoutTests builds its sections and lines with them.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/MaterialInspectorSections.cpp
	# The declared-row model: what a row shows and whether it shows at all.
	# Same leaf-TU shape as the notice beside it.
	DeclaredPropertyRowModelTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/DeclaredPropertyRowModel.cpp
	# The Shape::Global authoring notices: what the shape takes away (edge,
	# falloff, transform-scoped region) and which effect it silently drops.
	# Same leaf-TU shape as the generated-entity notice above.
	TerrainVolumeNoticeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TerrainVolumeNotices.cpp
	# The order that puts a section host ahead of the sections it hosts, over the
	# traits registry in EditorSDK.dll. Leaf TU, like the notices above.
	InspectorSectionHostsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/InspectorSectionHosts.cpp
	# The Surface Rules condition widget's testable halves: the unit and band
	# copy, the per-row ignored state, and the arm that holds the terrain preview
	# cadence open for a drag. Leaf TUs over the component header, the UI element
	# tree and TerrainService — the widget's own layout is the visual gate's, since
	# this target runs no layout pass.
	TerrainSurfaceRuleWidgetTests.cpp
	TerrainSizingNoticeTests.cpp
	# The grass Render Mode row's copy: the tooltip and the inactive note, over the one
	# gating answer (TerrainGrassNeedsAlpha) both are derived from. Leaf TU, no UI types.
	TerrainGrassVocabularyTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainGrassVocabulary.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainRuleConditionVocabulary.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TerrainRuleNotices.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TerrainSizingNotice.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TextureCoveragePolicyNotice.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainInteractiveModifierEdit.cpp
	AxisLayoutSerializerTests.cpp
	CancellableShellProcessTests.cpp
	VCSCommandExecutorTests.cpp
	VCSStatusPollerTests.cpp
	LoreJsonEventsTests.cpp
	SceneDiffTests.cpp
	HierarchySearchTests.cpp
	EditorTreeResizeGestureTests.cpp
	PolyhavenDownloadCompletenessTests.cpp
	EditorStartupTests.cpp
	EditorCommandLineTests.cpp
	EditorSessionDescriptorTests.cpp
	MainThreadHangWatchdogTests.cpp
	OpenSceneResolutionTests.cpp
	# The `path` parameter of open_asset/select_asset: resolved through the asset
	# mounts, never the CWD the engine parks at the project root. Leaf TU
	# (AssetManager only), so it compiles in rather than dragging DebugHandlers.cpp.
	OpenAssetPathTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/OpenAssetPath.cpp
	RevertSceneTests.cpp
	# SettingsStore itself ships in EditorSDK.dll (linked below); these tests
	# lock the multi-process merge-on-save contract for Preferences.json.
	SettingsStoreTests.cpp
	PhysicsProjectSettingsTests.cpp
	ShaderErrorRowsTests.cpp
	ScriptDiagnosticsByAssemblyTests.cpp
	# Script Editor language dispatch + tokenizer: shader extensions highlight as
	# GLSL, everything else as C#; pure over strings, no UI.
	ScriptSyntaxTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/ScriptSyntax.cpp
	# B1: pooled GraphPortedNode slots, BindModel, Display:None surplus.
	# Leaf TUs over the Graph UI elements (no GraphCanvas.cpp — canvas compile is
	# gated by Editor; these tests cover pool/node binding without it).
	MaterialGraphVariablesTests.cpp
	MaterialGraphDiagnosticsSummaryTests.cpp
	# The verdict the controller hands the shader panel when a graph comes into
	# view. The controller TU compiles in; the preview atlas and preview host it
	# reaches are link-time no-op stubs (see MaterialGraphPreviewStubs.cpp).
	MaterialGraphControllerTests.cpp
	MaterialGraphPreviewStubs.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphController.cpp
	GraphNodePoolTests.cpp
	GraphPortDropSlotTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNode.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNodeIcons.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphPort.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphPortedNode.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphVariables.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphDiagnosticsSummary.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphNode.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphPortDropSlot.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphCanvasEditKeys.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNodePool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphOverlay.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNodeSummary.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphVariableValues.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphNodeTypes.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNodeRegistry.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/AnimationGraphNodeTypes.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphKindChrome.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphSubgraphStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNest.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphBlendSpace1DStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphBlendSpace2DStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphTransitionStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphAnimationRuntimeDebug.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/NodeColorSettings.cpp
	# Source-level guard: staged engine assets are resolved through
	# PathUtils::GetInstallAssetsRoot() (EditorPaths' installAssetsRoot in editor
	# code), never by joining the executable directory to "Assets" or spelling the
	# bundle's Resources/Assets sibling by hand — the first offset does not exist
	# inside a macOS app bundle, the second exists only in one, and the loaders
	# that get it wrong fall back silently. Scans the production source roots,
	# compiles none of them.
	StagedAssetPathGuardTests.cpp
	# Source-level guard: no debug-server handler may reach for an engine call
	# that mutates the state it reports. It scans the editor source tree, so it
	# needs none of those sources compiled in.
	DebugServerReadPurityTests.cpp
	# The socket contract the debug server's port-exclusivity refusal rests on:
	# SO_REUSEADDR really does permit a second live bind, and a probe-connect
	# separates a live listener from a TIME_WAIT remnant. Raw sockets only, so it
	# compiles none of the server.
	DebugServerPortExclusivityTests.cpp
	# Source-level guard: the scene view keeps no cross-frame texture handle
	# outside the device-scoped cache, so no read of one can skip the rebuild
	# generation compare. Scans the source tree, compiles none of it.
	SceneViewSnapshotCacheGuardTests.cpp
	# Scene view toolbar wiring is one-shot per button instance: the panel pushes its
	# controller every frame and the UI re-runs the wiring pass on every layout, so
	# neither may re-bind an already-wired button. Leaf TU apart from
	# SceneViewController's two out-of-line members (stubbed in SceneViewLeafStubs.cpp)
	# and ToolbarDragDrop, which compiles in below.
	SceneViewToolbarWiringTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/SceneViewToolbar.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ActivateOnRelease.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ToolbarDragDrop.cpp
	# Which buttons ToolbarDragDrop::Setup makes draggable, and that the answer survives
	# the re-Setup the toolbar performs on every layout pass. Shares the ToolbarDragDrop
	# TU compiled in just above.
	ToolbarDragDropSetupTests.cpp
	# set_component's key-consumption contract: a values key that names no field must be
	# reported, never answered `ok`. The reader is a leaf TU (nlohmann only), so it
	# compiles in directly rather than dragging DebugHandlers.cpp.
	ComponentValueReaderTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/ComponentValueReader.cpp
	# set_component's write through a hand-written scene schema (Terrain): a bare GUID binds the
	# heightmap, and a refused key leaves the component as it was. Leaf TU (scene schema + ECS).
	SchemaComponentApplyTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/SchemaComponentApply.cpp
	# The `sceneDegraded` shape get_editor_state reports. It reconciles two records whose
	# disagreement is the whole point — the load-time one the window title reads, and the
	# outstanding one a save answers for — so the shape is a contract worth pinning. Same leaf-TU
	# shape as the reader above (SceneLoadFailure + nlohmann), so it compiles in rather than
	# dragging DebugHandlers.cpp.
	SceneDegradedReportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/SceneDegradedReport.cpp
	# get_job_system's reply shape: field names and units an agent reads (the
	# thread names, runningForMs only where the pool has a start time). Leaf TU
	# (JobSystemStatistics + nlohmann).
	JobSystemReportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/JobSystemReport.cpp
	# Where synthesized pointer input enters the engine. Injection that calls
	# UIManager directly reaches the UI alone, leaving the editor InputSystem and
	# the play-pointer remap — the whole runtime-input leg gameplay reads — unfed
	# and untestable. Leaf TU (WindowInputRouter + UIManager + Window), so it
	# compiles in rather than dragging DebugHandlers.cpp and EditorApplication.
	InjectedInputRoutingTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/InjectedInput.cpp
	# What the game's input system keeps across a switch-off of game input. Leaf
	# TU (InputSystem only), so it compiles in rather than dragging EditorApplication.
	RuntimeInputConnectionTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/RuntimeInputConnection.cpp
	# The colour picker dialog's own routing: a UI-only chain, plus the
	# eyedropper's screen-wide pointer grab and the Escape that ends it. Leaf TU
	# (WindowInputRouter + std::function), so it compiles in rather than dragging
	# ColorPickerWindow.cpp and EditorApplication.
	ColorPickerWindowInputTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ColorPicker/ColorPickerWindowInput.cpp
	# A colour picker closes with the scope of the page that opened it. Leaf TU
	# (std::function + weak_ptr), so it compiles in rather than dragging the
	# presenter and EditorApplication.
	ColorPickerScopeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ColorPicker/ColorPickerScope.cpp
	# Source-level guard: WindowInputRouter::BindBasicHandlers is the only place
	# production code binds a window's platform input handlers, so no window can be
	# given a partial chain by hand. Scans the source tree, compiles none of it.
	WindowInputRouterBindingGuardTests.cpp
	# The request-shaped half of that gate: which field a rejected pointer coordinate
	# is attributed to, and that the reply echoes the raw request number rather than
	# the infinity it narrows to. Leaf TU (nlohmann + the injection predicate above),
	# so it compiles in rather than dragging DebugHandlers.cpp.
	PointerCoordsRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/PointerCoordsRequest.cpp
	# The debug server's drop: a payload reaches the drop target under a point through the
	# window's drag-and-drop manager. Leaf TU (UIManager), so it compiles in rather than
	# dragging DebugHandlers.cpp.
	PerformDropRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/PerformDropRequest.cpp
	# The other half of the same gate: an elementId that resolves to an element with no
	# layout box. FindById is visibility-blind, so hidden elements and mount hosts come back
	# with a 0x0 box, and the centre of that box is the window corner — the pointer lands on
	# whatever is there while the reply reports success. Leaf TU (UIElement + nlohmann), so
	# it compiles in rather than dragging DebugHandlers.cpp.
	ElementPointerTargetTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/ElementPointerTarget.cpp
	# How a handler's result becomes the response: a RefuseRequest refusal goes out as
	# ok:false, any other result — one with its own "error" member included — as ok:true;
	# plus a source scan that no handler spells a refusal as an "error" member. Leaf TU
	# (nlohmann only); the pointer-request helpers above refuse through it as well.
	DebugServerReplyTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/DebugServerReply.cpp
	# Whether a UI-tree dump may claim it can prove an id absent. Two defaults quietly cut the
	# walk — maxDepth, and the hidden filter that drops whole subtrees — so an incomplete dump
	# is the normal reply, and this bookkeeping is what stops a caller reading a filter as an
	# absence. Leaf TU (UIElement + nlohmann).
	UITreeDumpTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/UITreeDump.cpp
	# Stroke-endpoint resolution for terrain_sculpt_dab. A rejected endpoint here is the
	# only place a degenerate target can die: downstream every guard is a length test, and
	# a non-finite target passes all of them. Leaf TU (nlohmann + Vector3), so it compiles
	# in directly rather than dragging TerrainDebugHandlers.cpp and EditorApplication.
	TerrainStrokeTargetTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/TerrainStrokeTarget.cpp
	# Parameter resolution for physics_raycast. Same disease as the stroke target above: a
	# JSON number outside float range narrows to inf and the backend's segment math has no
	# guard for it. Leaf TU (nlohmann + header-only Physics query types), so it compiles in
	# directly rather than dragging PhysicsDebugHandlers.cpp and the physics world service.
	PhysicsRaycastRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/PhysicsRaycastRequest.cpp
	# get_render_graph_resources' lifetime/type/aliveOnly selector. The handler used to
	# discard its request context, so every filtered query answered with the full list -
	# a wrong answer shaped exactly like a right one. Leaf TU (nlohmann only), so it
	# compiles in directly rather than dragging DebugHandlers.cpp.
	RenderGraphResourceFilterTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/RenderGraphResourceFilter.cpp
	# set_play_mode's action/state contract. Play-mode transitions are the one editor
	# action where a silent no-op looks exactly like success: EnterPlayMode and
	# ExitPlayMode both return early when already in the requested state, so a request
	# that was merely forwarded would answer ok having done nothing. Also pins the wire
	# spelling shared with get_editor_state. Leaf TU (nlohmann + the state enum), so it
	# compiles in directly rather than dragging DebugHandlers.cpp.
	PlayModeRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/PlayModeRequest.cpp
	# focus_view's view -> panel + focus-policy mapping. The Scene View needs UI focus
	# CLEARED (or UIManager eats its W/A/S/D camera keys) and the Game View needs its
	# viewport FOCUSED - opposite rules that look identical in a screenshot when swapped.
	# Leaf TU (nlohmann + the panel-id constants).
	ViewFocusRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/ViewFocusRequest.cpp
	# set_parallax_steps_view's enable/toggle contract: a toggle reads what the Scene Views
	# show, and a non-boolean enable is refused rather than read as truthy. Leaf TU
	# (nlohmann only).
	ParallaxStepsViewRequestTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/ParallaxStepsViewRequest.cpp
	# The material inspector's hand-off of a document's textures to the runtime material:
	# the write-back of every surface slot and the by-name transform routing
	# (Editor/Materials/MaterialRuntimeTextures, in EditorSDK).
	MaterialRuntimeTexturesTests.cpp
	# get_camera's projection report: which FOV axis the numbers are, and that the
	# aspect comes from the matrix rather than the viewport pixels. Leaf TU
	# (nlohmann + the reverse-Z projection helpers), so it compiles in rather than
	# dragging DebugHandlers.cpp.
	CameraProjectionReportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/CameraProjectionReport.cpp
	# The applied terrain split threshold in get_terrain_stats: what the kernels were given at the
	# view's render height, beside the authored target. Leaf TU (nlohmann only).
	TerrainSplitThresholdReportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/TerrainSplitThresholdReport.cpp
	# get_scene_hierarchy's report, compiled in on the same terms as the projection report.
	SceneHierarchyReportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/DebugServer/SceneHierarchyReport.cpp
	# Panel tab-icon defaults. The two panels compiled in are the light,
	# self-contained end of the panel set (Engine-only dependencies) and are enough
	# to exercise a real editor panel built the way the fallback docking path
	# builds one; the rest of the panel types are covered by the source scan.
	PanelDefaultTabIconTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/UIDemoPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/UndoHistoryPanel.cpp
	# Whether a dock config authored icon= at all, and whether it authored it
	# empty, are different instructions. The two TUs below are the pair that has
	# to keep them apart: the element that receives the attribute and the parser
	# that lifts it into a panel def. Both are self-contained (UIElement only).
	DockConfigIconAttributeTests.cpp
	# menu= (panel-listing visibility) rides the same two TUs: attribute intake
	# on the element, default-true lift into the panel def, and the guarantee
	# that a delisted panel still registers and activates by id.
	DockConfigMenuAttributeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Layout/DockConfigElements.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Layout/EditorDockConfigParser.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Startup/EditorStartup.cpp
	# Parser TU for EditorCommandLineTests — stateless free functions, same
	# compile-the-source pattern EditorStartup.cpp above uses.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Startup/EditorCommandLine.cpp
	# Sole owner of the project settings "physics" block; stateless free
	# functions over SettingsStore, same compile-the-source pattern.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/PhysicsProjectSettings.cpp
	# Session-provenance derivation for EditorSessionDescriptorTests — same
	# stateless free-function pattern; it takes the exe directory as a parameter
	# so tests can point it at synthetic trees.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Startup/EditorSessionDescriptor.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchySearch.cpp
	# The tree resize step both editor trees share; free functions over the UI module.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorTreeTitleIconVars.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/VersionControl/SceneDiff.cpp
	# Sole owner of the Shader Errors row model (parse + classify + order);
	# stateless free functions, same compile-the-source pattern.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/ShaderErrorRows.cpp
	# The Script Errors panel's rows kept per assembly; a leaf over the
	# compile-server diagnostic type, same compile-the-source pattern.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/ScriptDiagnosticsByAssembly.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Picking/MeshBvhDiskFormat.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Picking/TerrainPicking.cpp
	# The scene pick entry points (ScenePickTerrainInclusionTests) plus the two leaf TUs
	# they own outright. EditorPickProviderRegistry is deliberately NOT compiled in — it
	# ships in EditorSDK.dll (linked below), so a second copy here would clone the
	# registry singleton (issue #356 disease).
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Picking/MeshPickingService.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Picking/MeshBvhCache.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Picking/PrimitiveMeshes.cpp
	# The Scene View drop queries over the picks (MeshPickingPendingBvhTests).
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewDropPlacement.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewFraming.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewProjection.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/ConvertMarkupShapeCommand.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupEditorBridge.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRequestGate.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupCommentText.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupDrawList.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupGizmo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupKind.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupLabelOverlay.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupNotesEditCommand.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupPresentation.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRenderFeature.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupPathPort.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRegionDisplay.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupInspectorRows.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupPathInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRegionInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRegionPort.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupRequests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupTool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupsPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Markups/MarkupsPanelRows.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/AssetBoundDockPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewToolStrip.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewToolStripRegistry.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineToolMenu.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineToolStripEntry.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/TerrainBrushTool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/TerrainBrushToolStripEntry.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/TerrainPresence.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewPostProcessSwitches.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SelectionOutlineContributors.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewGizmos.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewGizmoThickLines.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineOwnerQuery.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineStrokeCapture.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineTool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SplineSceneGizmo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UndoRedo/SplinePointEdits.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewTransformTool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/SceneViewSelectionTool.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/AnimationPreviewManager.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/PlayModeManager.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Scene/SceneEditorController.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Scene/SceneDocumentManager.cpp
	# SceneDocumentManager's asset resolver reports references it could bind to
	# neither a record nor a file into the tracker.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/MissingAssetTracker.cpp
	# SceneDocumentManager scopes its scene-build drain in a HangWatchdogPhase;
	# self-contained (Logger, the JobSystem headers and Win32), so compiling it
	# in costs nothing.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Diagnostics/MainThreadHangWatchdog.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Scene/SceneBackupRecovery.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/SphereSculptSaver.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainZonePayloadSaver.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainBakeCacheSave.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/SaveSceneAsModal.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/SaveSceneChangesModal.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/RestoreSceneBackupModal.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/ConfirmActionModal.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Scene/DefaultSceneEntities.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/ModelAnimationPlayMode.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/AnimationPlayModeHelpers.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/AnimatorInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TransformInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/RenderPipelineInspector.cpp
	# Inspectors build menus through CreateContextMenu(), which wraps them in
	# the InterceptableContextMenu decorator (debug-server menu automation).
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/ContextMenuBackendPolicy.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/UIContextMenu.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/InterceptableContextMenu.cpp
	# The ownership slot every call site that does not keep a menu member shows
	# through, plus the reentrancy and shutdown hazards owning one introduces.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/UIContextMenu.cpp
	OwnedContextMenuLifetimeTests.cpp
	# Handlers set on a menu built before a backend change must survive the
	# inner-backend swap its next Clear() performs.
	ContextMenuBackendSwapHandlerTests.cpp
	# The wire router: a route that crosses no body, and the short one among those.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphRouteSolver.cpp
	GraphRouteSolverTests.cpp
	GraphRouteSolverHostileTests.cpp
	# The capture fields automation reads through open_context_menu, for the
	# state only a backend setter carries (SetItemColor's swatch hex).
	MenuCaptureColorTests.cpp
	# Command id 0 addresses no row, so the decorator's setters refuse it rather
	# than smearing one stamp across every separator, submenu and label row.
	MenuCaptureZeroIdTests.cpp
	# SettingsStore/EditorPaths ship in EditorSDK.dll (linked below); the VCS
	# seam is the EditorVcsProviderRegistry there too — with no providers
	# registered in tests, ActiveIntegration() is null (the old stub behavior).
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/PolyhavenService.cpp
	# The two editor surfaces that enumerate the whole registry — smart folder
	# collection and global asset search — against a project holding one asset of
	# a type both used to omit. AssetSearchProvider ships in EditorSDK.dll (linked
	# below, so no TU here); the smart folder evaluator is an Editor.exe source and
	# compiles in as a leaf over AssetRegistry plus the VCS seam noted above.
	AssetSurfaceTypeCoverageTests.cpp
	# DeleteAssetPaths refuses a weakly-canonical path that is not strictly
	# under AssetsRoot (symlink-to-Documents). Header-only guard, same helper
	# the controller calls; AssetsBrowserController.cpp is not in this target.
	AssetDeletePathGuardTests.cpp
	# Inline rename: stem validation, the shader+material pair lookup/rewrite, and the
	# undoable command over a temp directory (no AssetManager — registry updates are the
	# command's other half and need a live editor).
	AssetRenameTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/AssetRename.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UndoRedo/RenameAssetFileCommand.cpp
	# The undoable import-setting write, over a live registry: the command keys on the
	# asset's GUID, so a rename between the edit and the undo still undoes the edit.
	AssetMetaValueCommandTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UndoRedo/SetAssetMetaValueCommand.cpp
	# A raw heightmap's grid edit: one undo step, the stored value back on undo, one re-decode
	# per write and none for the value the store already holds.
	RawHeightmapSettingsCommitTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/RawHeightmapSettingsCommit.cpp
	# A 16-bit PNG heightmap past the decoder's limit: refused at the terrain's decode with the fix.
	PngHeightmapDecodeLimitTests.cpp
	# A terrain heightmap's cooked pages packed into its shipped terrain container.
	TerrainPageExportTests.cpp
	# The one open-asset decision every entry point (Assets double-click, inspector
	# buttons) executes. Pure over extension + preferences; GraphKindChrome.cpp is
	# already compiled in above for the graph-panel extension query.
	AssetOpenRoutingTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Assets/AssetOpenRouting.cpp
	# The C# file location a log line names, which a Log double-click opens.
	# Pure over the line text; the opener that acts on it needs a live editor.
	SourceLocationTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ScriptEditor/SourceLocation.cpp
	# The C# project a script opens with in the external editor: the path the
	# scripts project is generated at. Opening the editor itself needs a desktop.
	ExternalScriptEditorLauncherTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ExternalScriptEditorLauncher.cpp
	# The key the browser compares file-watch events against its current
	# directory with: the registry hands out folded roots while the watcher
	# delivers on-disk case, so both sides must key through the registry's own
	# normalizer. Header-only, same helper the controller and the data
	# providers call; AssetsBrowserController.cpp is not in this target.
	AssetPathKeyTests.cpp
	# The terrain material library inspector: registered from its own TU (no
	# BuiltInInspectorRegistration here), driven over a real asset on disk.
	TerrainMaterialLibraryInspectorTests.cpp
	MaterialTexturePreviewHostTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/TerrainMaterialLibraryInspector.cpp
	# The inspector's texture rows go through InspectorTextureSlotPreview.h, which
	# builds a MaterialTexturePreviewHost inline.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/MaterialTexturePreviewHost.cpp
	# The view overlay host and the overlay registered on it first.
	ViewOverlayHostTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ViewOverlayHost.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Scene/SceneLoadProgressOverlay.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/RenderPipeline/RenderPipelineStandInNotice.cpp
	# The load-then-continue helper the drops and inspectors use.
	AsyncAssetHelpersTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/AsyncAssetHelpers.cpp
	# A model's settings reload, which requests a model that is not loaded instead of waiting.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/ModelSettingsReload.cpp
	# Source-level guard: the terrain panel and the library panel name the
	# terrain-level tiling scale identically. The terrain inspector is a component
	# inspector over a live World and the terrain service, so it is scanned rather
	# than compiled in — same shape as PanelDefaultTabIconTests above.
	TerrainMaterialPanelVocabularyTests.cpp
	# The Scene View's component-keyed gizmo registry and the volume gizmos registered on it.
	ComponentGizmoRegistryTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/ComponentGizmoRegistry.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/PostProcessVolumeGizmo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/WindVolumeGizmo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/LocalVolumeGizmo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/Gizmos/ParticleEmitterGizmo.cpp
	# The sky's Scene View overlay, registered on that registry.
	SkyPathGizmoTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyPathGizmo.cpp
	# What the sun does over the day, read once for the overlay and the readout.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunDayKind.cpp
	# The sky inspector's sun readout: its wording and the info card it is shown in.
	SkySunReadoutTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunReadoutCard.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunReadoutText.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunPathCaptions.cpp
	# Switching the Sun path across a selection, over the real undo service.
	SkySunPathSwitchTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunPathSwitch.cpp
	# Switching the sun light's illuminance source and replacing the curve, over the real undo service.
	SkySunIlluminanceSwitchTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunIlluminanceSwitch.cpp
	# What the sun illuminance curve's reference line is drawn from, compared field by field.
	SkySunCurveReferenceTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunCurveReference.cpp
	# Switching the Sky Mode across a selection, the same way.
	SkyModeSwitchTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyEnvironmentEdit.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/LuxText.cpp
	# Source-level guard on the sky inspector: which container each group's rows are added to.
	SkyInspectorSectionsTests.cpp
	# Undo of the sky edits that start and end its sun light drive, over the real undo
	# service and the production sky system, including the generic edits (the component
	# header's enable toggle, Remove Component, creation) that start one through the sky's
	# registered record.
	SkySunLinkUndoTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunLinkUndo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyEnvironmentComponentTraits.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyEnvironmentCreation.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyOrdering.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UndoRedo/GenericEditUndo.cpp
	# The editor's terrain creation over the engine's provisioning: the rules volume's
	# hierarchy position and the refusal text.
	TerrainEntityProvisioningTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainEntityProvisioning.cpp
	# One undo step for a write that adds or changes components it does not name in advance.
	EntityComponentsEditTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UndoRedo/EntityComponentsEdit.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Entities/ComponentEnabledToggle.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Entities/ComponentRemoval.cpp
	# Source-level guard on dropdown option icons: the classes the Light and Camera fields
	# declare, the rules that answer them, and the artwork those rules name must agree. The
	# fields are component inspectors, so they are scanned rather than compiled in.
	DropdownOptionIconsTests.cpp
	# Source-level guard on the other half of that panel: its sections are bars, which only
	# happens while the class it asks for and the rule that answers it both exist.
	TerrainInspectorSectionStyleTests.cpp
	# Source-level guard on the Asset View preview: it fits a raw texture only while it both asks
	# for contain sizing and overrides the texture's intrinsic 9-slice with a zero slice. The panel
	# drives a video player and GPU texture uploads, so it is scanned rather than compiled in.
	AssetViewPreviewSliceTests.cpp
	# The measured half of that same pair, for the class an inspector sub-heading shares with a
	# component title: a declaration that suits the fixed-height header row can cost the
	# sub-heading its height, which no structural check can see. Lays the real widgets out over
	# the real editor stylesheets, so the section widget is compiled in rather than scanned.
	InspectorSectionHeaderLayoutTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/InspectorSection.cpp
	# The material inspector's row rhythm, measured the same way: every section bar, row and line
	# one row gap below the last, open or closed, in the entity inspector and the asset view.
	MaterialInspectorLayoutTests.cpp
	# The Hierarchy's slider-and-search row, laid out over the real editor stylesheets, and the
	# search-bar placement that moves it. The row, its slider and the search-bar registry are
	# compiled in.
	HierarchyNavigationBarLayoutTests.cpp
	# The row's slider contract: wheel pass-through, the resize gesture, the size round trip.
	HierarchyNavigationBarTests.cpp
	# The editor theme's own slider track colours, over the real theme sheets.
	EditorSliderThemeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyNavigationBar.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/ItemSizeSlider.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/EditorSearchBars.cpp
	# The terrain panel's material surface: the library's materials as a grid, the channel-role
	# binding under it, and the picker semantics both terrain material surfaces share. Sits over
	# the library asset, UndoRedoService and the library inspector's tint encoding — that TU is
	# compiled in above — and is where the library inspector takes its undoable whole-document
	# edit and its add path from.
	TerrainRoleMaterialsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainRoleMaterials.cpp
	# TerrainRoleMaterials calls InspectorUI::AddComponentSection, which lives here.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/InspectorComponentSection.cpp
	# The material-library mint, reached from SceneDocumentManager::Save above. Its file
	# creation goes through AssetCreation, whose other entry points are Engine-side (material
	# compiler, model bridge) and resolve through Engine.dll.
	TerrainMaterialLibraryMintTests.cpp
	# The join between a bound library and the entries extraction reads. An engine-side chain
	# (AssetManager + TerrainMaterialLibraryCache), hosted here because it needs an initialized
	# EngineCore, which this target already stands up for the terrain picking tests.
	TerrainMaterialLibraryConsumptionTests.cpp
	# The whole chain at once, editor-shaped: the REAL ComposedIsland scene through the real
	# loader + real TerrainExtractionSystem against a Vulkan device, asserting on the mapped
	# table SSBO bytes. Fixture: Engine/Tests/Fixtures/TerrainMatlibScene (StageTestAssets).
	TerrainMaterialLibraryEditorSceneTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Terrain/TerrainMaterialLibraryMint.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/AssetCreation.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/EntityMaterialTextureAssign.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/SmartFolder/SmartFolderFilterEvaluator.cpp
	# SettingsPanel / shortcut-catalog stub: real SettingsPanel.cpp is ~7000
	# lines with heavy UI control deps, and EditorShortcuts.cpp loads
	# Preferences.json. Compiled TUs call NotifyActivePipelineChanged,
	# MatchesCatalogShortcut, and ShouldSuppressEditorShortcutActions; the
	# stub defines those without pulling the panel or the catalog TU.
	SettingsPanelStub.cpp
	# Leaf stubs for SceneViewController members that SelectionTool and the
	# toolbar call; the real definitions live in SceneViewController.cpp (heavy
	# deps). No EditorTests test file exercises scene-view pick callbacks, so
	# no-op link-time stubs are sufficient.
	SceneViewLeafStubs.cpp
	# Additional layout/pool cases (same production TUs as GraphNodePoolTests above).
	ActivateOnReleaseTests.cpp
	GraphDiagnosticsStripTests.cpp
	GraphNodeLayoutTests.cpp
	GraphNodeIconsTests.cpp
	GraphNodeRegistryTests.cpp
	GraphKindChromeTests.cpp
	GraphSubgraphStoreTests.cpp
	GraphBlendSpace1DStoreTests.cpp
	GraphBlendSpace2DStoreTests.cpp
	GraphTransitionStoreTests.cpp
	GraphLocomotionSampleTests.cpp
	GraphAnimationRuntimeDebugTests.cpp
	GraphNestTests.cpp
	# Node-preview atlas cell bookkeeping: stable assignment across graph edits,
	# page growth, recycling, and the CSS sprite addressing a thumb paints with.
	# Pure logic — no device — so the layout TU compiles in as a leaf.
	GraphPreviewAtlasLayoutTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/GraphPreviewAtlasLayout.cpp
	# The preview projection that turns constant-node values into material param
	# lane reads. Pure model-to-model logic — no device, no EditorContext — so it
	# compiles in as a leaf the same way the layout does.
	MaterialGraphPreviewModelTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphPreviewModel.cpp
	# The atlas's ViewRegistry camera/view slots: allocated on the first tick with
	# cells, and handed back on Shutdown. Needs a real device + RenderServices, so
	# it skips where no Vulkan device is available.
	GraphPreviewAtlasViewLifetimeTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/GraphPreviewAtlas.cpp
	# The registration surface panel-owned render-graph participants reach the
	# frame through. Needs a real RGFrame (headless device via UIRgTestHarness),
	# so it skips where no Vulkan device is available.
	RenderGraphTickRegistryTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/RenderGraphTickRegistry.cpp
	# Inspector graph parameter rows: schema/keys, not typeId special cases.
	# Leaf TU — InspectorPanel.cpp stays out of EditorTests.
	GraphInspectorRowsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphInspectorRows.cpp
	# Suites above leave the engine running; this shuts it down before the process exits.
	${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
)
# Always create EditorTests when BUILD_TESTING is enabled; no UISystem/UI gating.
add_executable(EditorTests ${EDITOR_TEST_SOURCES})

# Find Google Test
find_package(GTest REQUIRED)

# Opt-in: the water fill's investigation instruments, which run against scenes in other content
# repositories (GE_WATER_LIFT_REPRO_SCENE, GE_SCENE_WATER_GATE) and skip without them. Not
# registered with CTest and run by no gate: build WaterFillInvestigationTests and run it with the
# scene set.
add_executable(WaterFillInvestigationTests
	# The lifted river over an authored island terrain: does the lift enclose dry ground?
	WaterFillLiftReproTests.cpp
	# The fill applied to an authored scene file: load, provision the terrain data the schema
	# does not carry, bake the modifier stack, and run the fill over every FitToBanks recipe.
	SceneWaterFillGateTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Placement/SplineFillRebuild.cpp
	${CMAKE_SOURCE_DIR}/Tests/EngineShutdownEnvironment.cpp
)
target_include_directories(WaterFillInvestigationTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Engine/Tests
	${CMAKE_SOURCE_DIR}/Tests
)
target_link_libraries(WaterFillInvestigationTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(WaterFillInvestigationTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(WaterFillInvestigationTests)
ge_stage_runtime_dependencies(WaterFillInvestigationTests)

# Pure editing algorithms, independent of UI initialization and engine fixtures.
add_executable(AnimationEditingTests AnimationEditingTests.cpp)
target_include_directories(AnimationEditingTests PRIVATE ${CMAKE_SOURCE_DIR}/Apps/Editor/Include)
target_compile_features(AnimationEditingTests PRIVATE cxx_std_20)
target_link_libraries(AnimationEditingTests PRIVATE GTest::gtest GTest::gtest_main)
ge_set_output_tests(AnimationEditingTests)
add_test(NAME AnimationEditingTests COMMAND $<TARGET_FILE:AnimationEditingTests>)

# Keep the entity toggle's coverage focused and out of EditorTests' large Unity batches.
# Inserting files into those batches can reshuffle unrelated editor translation units whose
# anonymous namespaces contain duplicate helper names.
add_executable(HierarchyEnableStateTests
	HierarchyEnableStateTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyEnableState.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyRowActivity.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Hierarchy/HierarchyLightIconTint.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Entities/EntityDisplayName.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/InspectorEntityActivity.cpp
)
target_include_directories(HierarchyEnableStateTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	# UIRgTestHarness.h: a headless device for the tree the row test lays out.
	${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Tests
)
target_link_libraries(HierarchyEnableStateTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(HierarchyEnableStateTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(HierarchyEnableStateTests)
ge_stage_runtime_dependencies(HierarchyEnableStateTests)
add_test(NAME HierarchyEnableStateTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:HierarchyEnableStateTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:HierarchyEnableStateTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# What the 9-slice editor offers on a texture it cannot write. Its own target
# rather than a file in EDITOR_TEST_SOURCES, for the reason above: the control's
# anonymous namespace would join EditorTests' unity batches.
add_executable(NineSliceFieldTests
	NineSliceFieldDisabledInputTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/NineSliceField.cpp
)
target_include_directories(NineSliceFieldTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
)
target_link_libraries(NineSliceFieldTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(NineSliceFieldTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(NineSliceFieldTests)
ge_stage_runtime_dependencies(NineSliceFieldTests)
add_test(NAME NineSliceFieldTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:NineSliceFieldTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:NineSliceFieldTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The shared curve editor's axis, reference line and playback marker. Its own
# target for the reason above; it links EditorSDK for the curve editor settings
# the playback indicator reads its colour from.
add_executable(CurveFieldTests
	CurveFieldTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveField.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveTangents.cpp
)
target_include_directories(CurveFieldTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Tests
)
target_link_libraries(CurveFieldTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(CurveFieldTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(CurveFieldTests)
ge_stage_runtime_dependencies(CurveFieldTests)
ge_stage_test_asset(CurveFieldTests
	"${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Regular.ttf"
	"Assets/Fonts/Roboto-Regular.ttf"
)
add_test(NAME CurveFieldTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:CurveFieldTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:CurveFieldTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The sky inspector's sun path rows, typed into and dragged over the real undo
# service. Its own target for the reason above: the rows draw the day on the
# shared curve editor, whose sources stay out of EditorTests' unity batches.
add_executable(SkySunPathRowsTests
	SkySunPathRowsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunPathRows.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunPathCaptions.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/DayOfYearText.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunDayKind.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunPathSwitch.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyEnvironmentEdit.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunLinkUndo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveField.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveTangents.cpp
)
target_include_directories(SkySunPathRowsTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
)
target_link_libraries(SkySunPathRowsTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(SkySunPathRowsTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(SkySunPathRowsTests)
ge_stage_runtime_dependencies(SkySunPathRowsTests)
add_test(NAME SkySunPathRowsTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:SkySunPathRowsTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:SkySunPathRowsTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The sky inspector's drive set: the custom illuminance curve switch and the colour switch shown
# under it. Its own target for the reason above: the drive rows draw the curve on the shared curve
# editor, with its preset picker and its context menu.
add_executable(SkySunDriveRowsTests
	SkySunDriveRowsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunDriveRows.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkyDayCurveRow.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/SkySunCurveReference.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/LuxText.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunIlluminanceSwitch.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkyEnvironmentEdit.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Sky/SkySunLinkUndo.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveField.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveTangents.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CubicBezierField.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurvePresetPicker.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurvePresets.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/ContextMenuBackendPolicy.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/UIContextMenu.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/InterceptableContextMenu.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/RenameLayoutModal.cpp
)
target_include_directories(SkySunDriveRowsTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules
)
target_link_libraries(SkySunDriveRowsTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(SkySunDriveRowsTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(SkySunDriveRowsTests)
ge_stage_runtime_dependencies(SkySunDriveRowsTests)
add_test(NAME SkySunDriveRowsTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:SkySunDriveRowsTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:SkySunDriveRowsTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The particle stack inspector over a processor type registered by the test, not the particle
# module: its section, rows, picker entry and undo come from the descriptor alone. Its own target
# for the reason above: the inspector draws curves on the shared curve editor.
add_executable(ParticleStackInspectorTests
	ParticleStackInspectorTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/Particles/ParticleStackInspector.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/Particles/ParticleStackEditor.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/InspectorNotice.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/Particles/ParticleParameterFields.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/Particles/ParticleProcessorSearchProvider.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/Particles/ParticleStackReference.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Inspectors/InspectorComponentSection.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveField.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/CurveTangents.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/ContextMenuBackendPolicy.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/UIContextMenu.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules/EditorContextMenu/InterceptableContextMenu.cpp
)
target_include_directories(ParticleStackInspectorTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
	${CMAKE_SOURCE_DIR}/Apps/Editor/Modules
)
target_link_libraries(ParticleStackInspectorTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(ParticleStackInspectorTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ParticleStackInspectorTests)
ge_stage_runtime_dependencies(ParticleStackInspectorTests)
add_test(NAME ParticleStackInspectorTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:ParticleStackInspectorTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:ParticleStackInspectorTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# Shipped outdoor scenes must be lit for the stop the Scene View pins. Leaf TU:
# it needs the engine's shared manual-exposure default and its photometric
# conversion, and nothing the editor links.
add_executable(ShippedSceneExposureTests
	ShippedSceneExposureTests.cpp
)
target_include_directories(ShippedSceneExposureTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Tests
)
target_link_libraries(ShippedSceneExposureTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(ShippedSceneExposureTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
# The suite reads the shipped scene bytes out of the staged mirror.
if(TARGET StageTestAssets)
	add_dependencies(ShippedSceneExposureTests StageTestAssets)
endif()
ge_set_output_tests(ShippedSceneExposureTests)
ge_stage_runtime_dependencies(ShippedSceneExposureTests)
add_test(NAME ShippedSceneExposureTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:ShippedSceneExposureTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:ShippedSceneExposureTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)


# Shortcut catalog persistence + capture suppression, hermetic: the tests
# build SettingsStore instances on paths they never Load()/Save(), and the
# tests that read the binding cache point the preferences at a scratch
# directory, so this machine's Preferences.json is never read or written.
# Compiles the real EditorShortcuts.cpp
# (EditorTests links a stub instead — see SettingsPanelStub.cpp).
add_executable(EditorShortcutsTests
	EditorShortcutsTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Shortcuts/EditorShortcuts.cpp
	# The editor's action registration, whose reserved shortcuts the routing rows read.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorInputActions.cpp
	# The size slider's tooltip follows the real catalog's Resize Items binding.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/Controls/ItemSizeSlider.cpp
)
target_include_directories(EditorShortcutsTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
)
target_link_libraries(EditorShortcutsTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(EditorShortcutsTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(EditorShortcutsTests)
ge_stage_runtime_dependencies(EditorShortcutsTests)
add_test(NAME EditorShortcutsTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:EditorShortcutsTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorShortcutsTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The per-frame allocation contract of ToolbarDragDrop::Setup. Its own target because it
# counts allocations in a process-wide window, which must not take in the work of
# EditorTests' shared batches.
add_executable(ToolbarDragDropAllocationTests
	ToolbarDragDropAllocationTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ToolbarDragDrop.cpp
	# ToolbarDragDrop persists button order through the editor preference store.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
)
target_include_directories(ToolbarDragDropAllocationTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
)
target_link_libraries(ToolbarDragDropAllocationTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(ToolbarDragDropAllocationTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ToolbarDragDropAllocationTests)
ge_stage_runtime_dependencies(ToolbarDragDropAllocationTests)
add_test(NAME ToolbarDragDropAllocationTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:ToolbarDragDropAllocationTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:ToolbarDragDropAllocationTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The render pipelines an export ships: the build-settings resolve, then the collector. Its
# own target because each test points EngineCore's process-wide workspace root at a temporary
# project, which must not land in EditorTests' shared batches.
add_executable(BuildRenderPipelineExportTests
	BuildRenderPipelineExportTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/BuildRenderPipelineSettings.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/BuildSettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Settings/RenderPipelineSettings.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
	# The registry's derived cache, registered the way AssetSystemTests registers it.
	${CMAKE_SOURCE_DIR}/Tests/Assets/AssetDbCacheFactoryTestEnv.cpp
)
target_include_directories(BuildRenderPipelineExportTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Tests
)
target_link_libraries(BuildRenderPipelineExportTests PRIVATE
	Engine
	AssetDatabaseSqlite
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(BuildRenderPipelineExportTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(BuildRenderPipelineExportTests)
ge_stage_runtime_dependencies(BuildRenderPipelineExportTests)
add_test(NAME BuildRenderPipelineExportTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:BuildRenderPipelineExportTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:BuildRenderPipelineExportTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The deferred panel-bind latch: OnPostLayout must schedule the bind once, not once per
# layout pass while the .uxml resolves. Its own target because it initializes the engine to
# get an AssetManager and installs a thread-local UI dispatcher for the whole process —
# neither belongs in EditorTests' shared batches.
add_executable(PanelDeferredBindLatchTests
	PanelDeferredBindLatchTests.cpp
	${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/WebPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/GameViewPanel.cpp
	# GameViewPanel publishes its viewport to the view overlay host.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ViewOverlayHost.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/UIDemoPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/AssetBoundDockPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
	WebPanelLeafStubs.cpp
)
target_include_directories(PanelDeferredBindLatchTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
)
target_link_libraries(PanelDeferredBindLatchTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(PanelDeferredBindLatchTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(PanelDeferredBindLatchTests)
ge_stage_runtime_dependencies(PanelDeferredBindLatchTests)
add_test(NAME PanelDeferredBindLatchTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:PanelDeferredBindLatchTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:PanelDeferredBindLatchTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The project picker's network jobs on their channels: the community catalog fetch and a
# project acquisition hold no compute worker, and the catalog service's destructor waits for a
# fetch in flight. Its own target because it points GE_EDITOR_USER_DATA_ROOT at a scratch
# preferences file for the whole process.
add_executable(ProjectPickerJobTests
	ProjectCatalogServiceTests.cpp
	ProjectAcquisitionTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Projects/ProjectCatalogService.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Projects/ProjectCatalog.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Projects/ProjectAcquisition.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Projects/SubprocessStream.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Projects/ZipArchive.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tdef.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tinfl.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_zip.c
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
)
target_include_directories(ProjectPickerJobTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz
)
# The vendored miniz sources are third-party C; source properties are scoped to the
# directory that sets them, so the Editor's /w does not reach this target's copies.
if(MSVC)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz.c
		${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tdef.c
		${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tinfl.c
		${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_zip.c
		PROPERTIES COMPILE_OPTIONS "/w")
endif()
target_link_libraries(ProjectPickerJobTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(ProjectPickerJobTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ProjectPickerJobTests)
ge_stage_runtime_dependencies(ProjectPickerJobTests)
add_test(NAME ProjectPickerJobTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:ProjectPickerJobTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:ProjectPickerJobTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# The Polyhaven download manager is constructed before the engine initializes, so it creates
# its transfer channel on first use. Its own target because its test needs a process whose
# engine never initializes, and EditorTests leaves the engine running between suites.
add_executable(PolyhavenDownloadManagerTests
	PolyhavenDownloadManagerTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/PolyhavenDownloadManager.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/PolyhavenService.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/DownloadPillOverlay.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/AsyncAssetHelpers.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/AssetCreation.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/EntityMaterialTextureAssign.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Assets/SpriteEntityFactory.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorComponentInstantiations.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
)
target_include_directories(PolyhavenDownloadManagerTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source
)
target_link_libraries(PolyhavenDownloadManagerTests PRIVATE
	Engine
	EditorSDK
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(PolyhavenDownloadManagerTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(PolyhavenDownloadManagerTests)
ge_stage_runtime_dependencies(PolyhavenDownloadManagerTests)
add_test(NAME PolyhavenDownloadManagerTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:PolyhavenDownloadManagerTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:PolyhavenDownloadManagerTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# A panel destroyed while its asset load is in flight must not be called back. Its own
# target for the same reasons as the latch suite above, plus a held UI-asset parser and an
# editor asset source it mounts on the engine's AssetManager for the whole process.
add_executable(PanelAssetLoadWithdrawalTests
	PanelAssetLoadWithdrawalTests.cpp
	${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/WebPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/AssetBoundDockPanel.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
	WebPanelLeafStubs.cpp
)
target_include_directories(PanelAssetLoadWithdrawalTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Tests
)
target_link_libraries(PanelAssetLoadWithdrawalTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(PanelAssetLoadWithdrawalTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(PanelAssetLoadWithdrawalTests)
ge_stage_runtime_dependencies(PanelAssetLoadWithdrawalTests)
add_test(NAME PanelAssetLoadWithdrawalTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:PanelAssetLoadWithdrawalTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:PanelAssetLoadWithdrawalTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# Real editor UIManager -> viewport -> gameplay UIManager/Button input, without a
# GPU or render tick. Its own process keeps UI dispatch ownership isolated.
add_executable(GameViewPointerDeliveryTests
	GameViewPointerDeliveryTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Panels/GameViewPanel.cpp
	# GameViewPanel publishes its viewport to the view overlay host.
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/UI/ViewOverlayHost.cpp
)
target_include_directories(GameViewPointerDeliveryTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
)
target_link_libraries(GameViewPointerDeliveryTests PRIVATE GTest::gtest GTest::gtest_main Engine)
set_target_properties(GameViewPointerDeliveryTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(GameViewPointerDeliveryTests)
ge_stage_runtime_dependencies(GameViewPointerDeliveryTests)
add_test(NAME GameViewPointerDeliveryTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:GameViewPointerDeliveryTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:GameViewPointerDeliveryTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# Actual controller input uses the last presented extent and world scope without
# creating a GPU device; camera readback uses a headless device in a separate fixture.
add_executable(GameViewControllerInputTests
	GameViewControllerInputTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/GameViewController.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/GameViewMovieCapture.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/ProjectUIScaleBinding.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/UIScaleProjectSettingsWriter.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SettingsStore.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/EditorPaths.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/ViewPresentationSnapshot.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/ViewSnapshotTexture.cpp
)
target_include_directories(GameViewControllerInputTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Engine/Tests
)
target_link_libraries(GameViewControllerInputTests PRIVATE GTest::gtest GTest::gtest_main Engine)
set_target_properties(GameViewControllerInputTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(GameViewControllerInputTests)
ge_stage_runtime_dependencies(GameViewControllerInputTests)
add_test(NAME GameViewControllerInputTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:GameViewControllerInputTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:GameViewControllerInputTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# Device-scoped texture cache for the editor's cross-frame snapshots, and the
# ViewPresentationSnapshot wrapper its consumers read through. Its own target
# for the same reason as HierarchyEnableStateTests, plus one of its own: the
# rebuild cases inject a device loss, which must not run inside EditorTests'
# shared-process batches.
add_executable(ViewSnapshotTextureTests
	ViewSnapshotTextureTests.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/ViewSnapshotTexture.cpp
	${CMAKE_SOURCE_DIR}/Apps/Editor/Source/SceneView/ViewPresentationSnapshot.cpp
)
target_include_directories(ViewSnapshotTextureTests PRIVATE
	${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	${CMAKE_SOURCE_DIR}/Engine/Tests
)
target_link_libraries(ViewSnapshotTextureTests PRIVATE
	Engine
	GTest::gtest
	GTest::gtest_main
)
set_target_properties(ViewSnapshotTextureTests PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ViewSnapshotTextureTests)
ge_stage_runtime_dependencies(ViewSnapshotTextureTests)
add_test(NAME ViewSnapshotTextureTests
	COMMAND "${CMAKE_COMMAND}"
		-DTEST_EXE=$<TARGET_FILE:ViewSnapshotTextureTests>
		-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:ViewSnapshotTextureTests>
		-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
)

# Child fixture for RunProcessCapturedTests and VCSCommandExecutorTests: what a
# shell-free spawn delivers can only be read from the child's argv, so the test
# needs a program that reports it.
# Staged beside EditorTests, which resolves it from its own executable directory.
add_executable(ArgvEcho ArgvEcho.cpp)
set_target_properties(ArgvEcho PROPERTIES
	FOLDER "Tests"
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ArgvEcho)
add_dependencies(EditorTests ArgvEcho)

# Child fixture for VCSCommandExecutorTests' environment tests: prints the
# variables it was given. Staged beside EditorTests like ArgvEcho.
add_executable(EnvEcho EnvEcho.cpp)
set_target_properties(EnvEcho PROPERTIES
	FOLDER "Tests"
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(EnvEcho)
add_dependencies(EditorTests EnvEcho)

# Child fixture for the argv CancellableShellProcess::Run tests: writes known
# shapes to stdout, stderr and from stdin. Staged beside EditorTests like ArgvEcho.
add_executable(ProcessStreamProbe ProcessStreamProbe.cpp)
set_target_properties(ProcessStreamProbe PROPERTIES
	FOLDER "Tests"
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	VCPKG_APPLOCAL_DEPS OFF
)
ge_set_output_tests(ProcessStreamProbe)
add_dependencies(EditorTests ProcessStreamProbe)

# Prevent vcpkg's applocal.ps1 from copying extra DLLs (notably mimalloc-redirect.dll),
# since we do our own controlled staging via ge_stage_runtime_dependencies.
set_target_properties(EditorTests PROPERTIES VCPKG_APPLOCAL_DEPS OFF)

# Link required libraries:
# - Engine for ECS/World/Engine::GetInstance, AssetRuntime, and the UI module
#   (UI is an OBJECT module spliced into Engine.dll — linking it here as well
#   would give the exe a second copy of its file-scope state, issue #356, same
#   disease as #340; symbols resolve through Engine's export table).
# - MeshPicking is editor-only and NOT part of Engine.dll, so the exe-local
#   copy is the only copy in the process — a direct link is the correct home.
# - EditorSDK carries the editor extension registries (InspectorRegistry,
#   EditorPluginRegistry, component traits, pick providers) the compiled-in
#   editor sources dispatch through. Link it — NEVER compile its sources into
#   this exe, which would clone the registry singletons (issue #356 disease).
# - GTest for the test framework
# - LoreJsonEvents lives in the lore-vcs package module (the editor core
#   carries no provider code); LoreJsonEventsTests pins the `lore --json` wire
#   shapes the provider reads (status, revision header, history) by compiling
#   the package TU directly. Pure functions over nlohmann, no state.
target_sources(EditorTests PRIVATE
	${CMAKE_SOURCE_DIR}/Packages/lore-vcs/Editor/LoreJsonEvents.cpp)
target_include_directories(EditorTests PRIVATE
	${CMAKE_SOURCE_DIR}/Packages/lore-vcs/Editor)
# The mark-up glow's rim term (Shaders/Includes/markup_glow_rim.glsl) is EXTRACTED and compiled
# as C++ through the shared GLSL shim, so MarkupSceneViewTests run the shipped term rather than a
# copy of it. A missing or reordered marker fails the build.
set(MARKUP_GLOW_RIM_GLSL ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/markup_glow_rim.glsl)
set(MARKUP_GLOW_SHIM_DIR ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests)
set(MARKUP_GLOW_RIM_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/MarkupGlowRimExtracted.h)
add_custom_command(
	OUTPUT ${MARKUP_GLOW_RIM_HEADER}
	COMMAND ${CMAKE_COMMAND} -DSRC=${MARKUP_GLOW_RIM_GLSL}
	                         -DOUT=${MARKUP_GLOW_RIM_HEADER}
	                         "-DBEGIN_MARKER=// GE_SHARED_MARKUP_GLOW_RIM_BEGIN"
	                         "-DEND_MARKER=// GE_SHARED_MARKUP_GLOW_RIM_END"
	                         -P ${MARKUP_GLOW_SHIM_DIR}/ExtractShaderBlock.cmake
	DEPENDS ${MARKUP_GLOW_RIM_GLSL} ${MARKUP_GLOW_SHIM_DIR}/ExtractShaderBlock.cmake
	COMMENT "Extracting the mark-up glow's rim from markup_glow_rim.glsl"
	VERBATIM)
target_sources(EditorTests PRIVATE ${MARKUP_GLOW_RIM_HEADER})
target_include_directories(EditorTests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${MARKUP_GLOW_SHIM_DIR})
# Shuts the engine down before main returns: fixtures here initialize it and
# never do, so the process would otherwise exit with job workers still running.
target_sources(EditorTests PRIVATE
	${CMAKE_SOURCE_DIR}/Tests/EngineShutdownEnvironment.cpp)
target_link_libraries(EditorTests
    PRIVATE
	    Engine
	    EditorSDK
	    MeshPicking
    GTest::gtest
    GTest::gtest_main
)

# DebugServerPortExclusivityTests talks to the socket stack directly (the debug
# server's own Winsock use lives behind Engine's PRIVATE link).
if(WIN32)
    target_link_libraries(EditorTests PRIVATE ws2_32)
endif()

# Editor unit tests should not require the native scripting ABI DLL by default.
# Linking `GameEngine.Native` can introduce allocator/hosting side effects (e.g. mimalloc redirect)
# that are irrelevant to most editor tests and can destabilize test teardown.
# When it is not linked, the GE_* hot-reload entry points resolve against the
# no-op dispatcher Engine.dll itself exports (Engine/Source/HotReloadNativeStubs.cpp).
option(EDITOR_TESTS_LINK_NATIVE_ABI "Link GameEngine.Native into EditorTests" OFF)
if(EDITOR_TESTS_LINK_NATIVE_ABI AND TARGET GameEngine.Native)
    target_link_libraries(EditorTests PRIVATE GameEngine.Native)
endif()

# Include directories and properties guarded by target existence
if(TARGET EditorTests)
	target_include_directories(EditorTests
	    PRIVATE
	    ${CMAKE_SOURCE_DIR}/Engine/Source
	    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source
	    ${CMAKE_SOURCE_DIR}/Apps/Editor/Include
	    ${CMAKE_SOURCE_DIR}/Apps/Editor/Modules
	    # TestDeviceHelper.h + StagedTestPaths.h for the editor-scene terrain fixture.
	    ${CMAKE_SOURCE_DIR}/Engine/Tests
	    # UIRgTestHarness.h: headless device for a real UIManager.
	    ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Tests
	    # TestTempDir.h + WatchedFileEvents.h, shared across the test trees.
	    ${CMAKE_SOURCE_DIR}/Tests
	    ${CMAKE_SOURCE_DIR}/dependencies/vcpkg/installed/x64-windows/include
	)

	# Set target properties
	set_target_properties(EditorTests PROPERTIES
	    CXX_STANDARD 20
	    CXX_STANDARD_REQUIRED ON
	)

	# PanelDefaultTabIconTests scans the editor sources it locks (the panel
	# headers, layout.uxml and the dock-config factory) to prove every panel type
	# carries a tab-icon default. Source-level invariant, so it reads the source
	# tree rather than the staged output.
	# TerrainLayerTests locks the editor's mirror of the engine's untextured layer palette
	# against the engine source that owns it (file-local, so nothing links the two).
	target_compile_definitions(EditorTests PRIVATE
	    GE_EDITOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Apps/Editor"
	    GE_REPO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

	# Output layout: bin/<Config>/Tests
	ge_set_output_tests(EditorTests)
	ge_stage_runtime_dependencies(EditorTests)
	# The suite reads fixtures out of the staged repo mirrors under the build root.
	if(TARGET StageTestAssets)
		add_dependencies(EditorTests StageTestAssets)
	endif()
	# The mark-up glow tests read the compiled shader packages from the build root.
	if(TARGET CompileShaderPkgs)
		add_dependencies(EditorTests CompileShaderPkgs)
	endif()

	# GraphPortedNode resolves its inner template through the editor
	# install-assets root, which for a test executable — never an app bundle —
	# is <exe>/Assets on every platform. Stage the .uxml there so the graph node
	# tests exercise the template body rather than the C++ fallback. Nothing may
	# be staged at bin/<Config>/Resources: a directory beside the shared Tests
	# tree is not part of any executable's layout.
	ge_stage_test_asset(EditorTests
	    "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/Graph/graph-ported-node.uxml"
	    "Assets/UI/Graph/graph-ported-node.uxml")

	# InspectorNotice builds its plate, text and action from its layout, read
	# from the same install-assets root.
	ge_stage_test_asset(EditorTests
	    "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/controls/InspectorNotice/InspectorNotice.uxml"
	    "Assets/UI/controls/InspectorNotice/InspectorNotice.uxml")

	# The shipped animation-graph sample, read from the same exe-anchored asset
	# root the editor loads it from.
	ge_stage_test_asset(EditorTests
	    "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Graphs/LocomotionSample.animgraph"
	    "Assets/Graphs/LocomotionSample.animgraph")

	# EditorTests compiles the editor's GraphNodeRegistry, whose material palette
	# comes from reflecting the engine shader-graph node library off disk.
	ge_stage_shader_graph_nodes(EditorTests)

	# Add to CTest for automated discovery
	# Avoid build-time test discovery (gtest_discover_tests runs the test binary during build).
	# Some runtime dependency loaders (e.g. mimalloc redirect) can fail during that phase.
	# Run the whole suite via CTest instead.
	add_test(NAME EditorTests
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	# Focused aliases to make it easy to run a specific suite via CTest:
	#   ctest -R EditorTests.PlayModeChangeReview -C <Config>
	add_test(NAME EditorTests.PlayModeChangeReview
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=PlayModeChangeReviewTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SceneViewFraming
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SceneViewFramingTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SceneViewTransformTool
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SceneViewTransformToolTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.UndoRedoService
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=UndoRedoServiceTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SkeletonRuntimeUndo
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SkeletonRuntimeUndoTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.InspectorSimulationRefresh
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=InspectorSimulationRefreshTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.MeshBvhDiskFormat
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=MeshBvhDiskFormatTest.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SceneBackupRecovery
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SceneBackupRecoveryTests.*:SceneBackupZonePayloadFlushTests.*:SceneBackupSphereSculptFlushTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SaveBlockedDuringPlay
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SaveBlockedDuringPlayTests.*:SaveBlockedDuringPlayWiring.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.SettingsStore
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=SettingsStoreTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	# Spline recipe geometry: the fence layout solver, the recipe contracts and
	# the extrude sweep's station stream.
	add_test(NAME EditorTests.SplineRecipes
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=FenceLayout.*:FenceEmission.*:FencePieceReuse.*:FenceRecipeLayout.*:FenceSpanOverrides.*:FenceSpanOverrideRemap.*:SplinePointEdits.*:SplineFenceRecipe.*:SplinePlacementRecipe.*:SplineExtrudeRecipe.*:SplineWallRecipe.*:SplineChunkSlots.*:SplineChunkCommit.*:FenceMitreVariants.*:CarveChunkRanges.*:SplineExtrudeCoverage.*:SplineSweepParity.*:SplineSweepWall.*:SplineSurfaceObservation.*:SplinePoolSelection.*:TileLayout.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	add_test(NAME EditorTests.CancellableShellProcess
	  COMMAND "${CMAKE_COMMAND}"
	    -DTEST_EXE=$<TARGET_FILE:EditorTests>
	    -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorTests>
	    -DTEST_ARGS=--gtest_filter=CancellableShellProcessTests.*:RunProcessCapturedTests.*
	    -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)

	# Copy required DLLs for Editor tests
	if(WIN32)
	    # Copy Engine DLLs
	    add_custom_command(TARGET EditorTests POST_BUILD
	        COMMAND ${CMAKE_COMMAND} -E copy_if_different
	        $<TARGET_FILE:Engine>
	        $<TARGET_FILE_DIR:EditorTests>
	    )
	endif()
endif()

# Exercise the real Editor entry point against the shared embedded-source resolver.
add_executable(EditorEmbeddedAnimationTests
    ${CMAKE_SOURCE_DIR}/Engine/Tests/EmbeddedAnimationPersistenceTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/ModelAnimationPlayMode.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/PlayMode/AnimationPlayModeHelpers.cpp)
target_link_libraries(EditorEmbeddedAnimationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(EditorEmbeddedAnimationTests PRIVATE ${CMAKE_SOURCE_DIR}/Apps/Editor/Include)
target_compile_definitions(EditorEmbeddedAnimationTests PRIVATE EMBEDDED_ANIMATION_EDITOR_TESTS=1)
set_target_properties(EditorEmbeddedAnimationTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Editor/Tests")
ge_set_output_tests(EditorEmbeddedAnimationTests)
ge_stage_runtime_dependencies(EditorEmbeddedAnimationTests)
add_test(NAME EditorEmbeddedAnimationTests COMMAND $<TARGET_FILE:EditorEmbeddedAnimationTests>)

# A process that leaves with the engine initialized while its startup scan runs
# faults inside a job worker. Its own target: the executable's exit code is the
# result, and the widened exit window in the file must not touch other tests.
# ELF and Mach-O only: the exit-window guard uses usleep, and the race is only
# verified there.
if(NOT WIN32)
	add_executable(EngineExitDuringScanTests
		EngineExitDuringScanTests.cpp
		${CMAKE_SOURCE_DIR}/Tests/EngineShutdownEnvironment.cpp
	)
	target_include_directories(EngineExitDuringScanTests PRIVATE
		${CMAKE_SOURCE_DIR}/Tests
	)
	target_link_libraries(EngineExitDuringScanTests PRIVATE
		Engine
		GTest::gtest
		GTest::gtest_main
	)
	set_target_properties(EngineExitDuringScanTests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		VCPKG_APPLOCAL_DEPS OFF
	)
	ge_set_output_tests(EngineExitDuringScanTests)
	ge_stage_runtime_dependencies(EngineExitDuringScanTests)
	add_test(NAME EngineExitDuringScanTests
		COMMAND "${CMAKE_COMMAND}"
			-DTEST_EXE=$<TARGET_FILE:EngineExitDuringScanTests>
			-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EngineExitDuringScanTests>
			-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)
endif()

# Opening a project in the real editor writes no engine content into it (#2510).
# Each case launches the built editor, so it needs a display and a GPU device;
# it builds the editor first, and reads the engine's ForwardPlus from the
# Assets/ mirror StageTestAssets stages into the build root.
if(TARGET Editor)
	add_executable(EditorProjectOpenTests
		EditorProjectOpenTests.cpp
	)
	target_include_directories(EditorProjectOpenTests PRIVATE
		${CMAKE_SOURCE_DIR}/Tests
		# StagedTestPaths.h — exe-anchored resolution of the staged Assets/ root.
		${CMAKE_SOURCE_DIR}/Engine/Tests
	)
	target_link_libraries(EditorProjectOpenTests PRIVATE
		Engine
		GTest::gtest
		GTest::gtest_main
	)
	target_compile_definitions(EditorProjectOpenTests PRIVATE
		GE_TEST_EDITOR_EXE="$<TARGET_FILE:Editor>"
	)
	if(WIN32)
		target_link_libraries(EditorProjectOpenTests PRIVATE ws2_32)
	endif()
	set_target_properties(EditorProjectOpenTests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		VCPKG_APPLOCAL_DEPS OFF
		FOLDER "Editor/Tests"
	)
	add_dependencies(EditorProjectOpenTests Editor)
	if(TARGET StageTestAssets)
		add_dependencies(EditorProjectOpenTests StageTestAssets)
	endif()
	ge_set_output_tests(EditorProjectOpenTests)
	ge_stage_runtime_dependencies(EditorProjectOpenTests)
	add_test(NAME EditorProjectOpenTests
		COMMAND "${CMAKE_COMMAND}"
			-DTEST_EXE=$<TARGET_FILE:EditorProjectOpenTests>
			-DTEST_WORKING_DIR=$<TARGET_FILE_DIR:EditorProjectOpenTests>
			-P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
	)
	set_tests_properties(EditorProjectOpenTests PROPERTIES LABELS "Editor" TIMEOUT 1900)
endif()
