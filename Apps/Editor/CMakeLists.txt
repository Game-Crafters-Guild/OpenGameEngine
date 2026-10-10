# Editor application target

set(EDITOR_SOURCES
    Source/main.cpp
    Source/Editor/Application/BuildConfigStatus.cpp
    Source/Editor/Application/EditorVersion.cpp
    Source/EditorApplication.cpp
    Source/DeviceLossSurfacer.cpp
    Source/Diagnostics/MainThreadHangWatchdog.cpp
    Source/EditorTreeTitleIconVars.cpp
    Source/Settings/RenderPipelineSettings.cpp
    Source/Editor/Assets/AssetRelativePath.cpp
    Source/Editor/BuildExporters/SteamDeckBuildExporter.cpp
    Source/Editor/BuildExporters/WebBuildExporter.cpp
    Source/Editor/Settings/BuildRenderPipelineSettings.cpp
    Source/Editor/Settings/BuildSettingsPage.cpp
    Source/Editor/Settings/BuildSettingsStore.cpp
    Source/Editor/Settings/DirectionalShadowSettingsPage.cpp
    Source/Editor/Settings/ExperimentalRenderingSettingsPage.cpp
    Source/Editor/Settings/InterfaceSettingsPage.cpp
    Source/Editor/Settings/AssetPreviewSettings.cpp
    Source/Editor/Settings/ToggleAppearanceSettings.cpp
    Source/Editor/Settings/LodProjectSettingsWriter.cpp
    Source/Editor/Settings/GameUIScaleSettingsPage.cpp
    Source/Editor/Settings/GraphSettingsPage.cpp
    Source/Editor/Settings/LodSettingsPage.cpp
    Source/Editor/Settings/PhysicsProjectSettings.cpp
    Source/Editor/Settings/ProjectUIScaleBinding.cpp
    Source/Editor/Settings/RenderProjectSettings.cpp
    Source/Editor/Settings/SvgRasterSettings.cpp
    Source/Editor/Settings/UIScaleProjectSettingsWriter.cpp
	Source/Editor/RenderGraphTickRegistry.cpp
	Source/Editor/Hierarchy/HierarchySearch.cpp
	Source/Editor/Hierarchy/HierarchyEnableState.cpp
	Source/Editor/Hierarchy/HierarchyRowActivity.cpp
	Source/Editor/Hierarchy/HierarchyLightIconTint.cpp
	Source/Editor/Hierarchy/HierarchyNavigationBar.cpp
	Source/Editor/Hierarchy/HierarchyRenderLayerEdit.cpp
	Source/Editor/Hierarchy/HierarchyReparentRules.cpp
	Source/Startup/BuiltinAssetSync.cpp
	Source/Startup/EditorCommandLine.cpp
	Source/Startup/EditorPathPolicy.cpp
	Source/Startup/EditorSessionDescriptor.cpp
	Source/Startup/EditorStartup.cpp
	Source/Automation/EditorSmokeHooks.cpp
	Source/Automation/UiReplayRunner.cpp
	Source/Telemetry/RGCsvSidecar.cpp
    Source/Assets/AssetCreation.cpp
    Source/Assets/AssetPipelineCommands.cpp
    Source/Assets/AssetRename.cpp
    Source/Assets/AssetRenameController.cpp
    Source/UndoRedo/GenericEditUndo.cpp
    Source/UndoRedo/EntityComponentsEdit.cpp
    Source/UndoRedo/RenameAssetFileCommand.cpp
    Source/UndoRedo/SetAssetMetaValueCommand.cpp
    Source/UndoRedo/SplinePointEdits.cpp
    Source/Assets/AsyncAssetHelpers.cpp
    Source/Assets/EntityMaterialTextureAssign.cpp
    Source/Assets/SpriteEntityFactory.cpp
    Source/Editor/Entities/ComponentEnabledToggle.cpp
    Source/Editor/Entities/ComponentRemoval.cpp
    Source/Editor/Entities/SkyboxEntityFactory.cpp
    Source/Assets/AssetsBrowserController.cpp
    Source/Assets/EditorAssetActionsInstall.cpp
    Source/Assets/AssetsDataProviders.cpp
    Source/Assets/DownloadPillOverlay.cpp
    Source/Assets/PolyhavenDownloadManager.cpp
    Source/Assets/PolyhavenPlaceholderFactory.cpp
    Source/Assets/PolyhavenService.cpp
    Source/UI/SmartFolder/SmartFolder.cpp
    Source/UI/SmartFolder/SmartFolderController.cpp
    Source/UI/SmartFolder/SmartFolderManager.cpp
    Source/UI/SmartFolder/SmartFolderCommands.cpp
    Source/UI/SmartFolder/SmartFolderFilterEvaluator.cpp
    Source/UI/SmartFolder/SmartFolderInspector.cpp
    Source/UI/EditorUIFontSettings.cpp
    Source/Panels/AssetsPanel.cpp
    Source/Panels/TodoPanel.cpp
    Source/Panels/BookmarksPanel.cpp
    Source/Panels/UndoHistoryPanel.cpp
    Source/Panels/BuildPanel.cpp
    Source/Panels/BuiltInPanelTypes.cpp
    Source/Panels/MovieRecorderPanel.cpp
    Source/Panels/HierarchyPanel.cpp
    Source/Editor/Hierarchy/HierarchyEntityIcon.cpp
    Source/Editor/Hierarchy/HierarchyOrdering.cpp
    Source/Panels/InspectorPanel.cpp
    Source/Panels/AssetViewPanel.cpp
    Source/Panels/LogPanel.cpp
    Source/Panels/LogView.cpp
    Source/Panels/ScriptDiagnosticsByAssembly.cpp
    Source/Panels/ScriptErrorsPanel.cpp
    Source/Panels/ShaderErrorRows.cpp
    Source/Panels/ShaderErrorsPanel.cpp
    Source/Panels/MonitorsPanel.cpp
    Source/Panels/RenderGraphPanel.cpp
    Source/Panels/CpuProfilerPanel.cpp
    Source/Panels/VisualProfilerPanel.cpp
    Source/Panels/BacktraceFrameView.cpp
    Source/Panels/VramPanel.cpp
    Source/Panels/ProjectFolderPickerModal.cpp
    Source/Projects/ProjectCatalog.cpp
    Source/Projects/ProjectCatalogService.cpp
    Source/Projects/ProjectAcquisition.cpp
    Source/Projects/SubprocessStream.cpp
    Source/Projects/ZipArchive.cpp
    Source/Panels/SaveSceneAsModal.cpp
    Source/Panels/DownloadInProgressModal.cpp
    Source/Panels/ConfirmActionModal.cpp
    Source/Panels/SaveSceneChangesModal.cpp
    Source/Panels/RestoreSceneBackupModal.cpp
    Source/Panels/RenameLayoutModal.cpp
    Source/Panels/CameraCustomAspectModal.cpp
    Source/Panels/AddTagModal.cpp
    Source/Panels/PlayModeChangeReviewModal.cpp
    Source/Panels/VideoPlayerModal.cpp
    Source/EditorVideoPlayerController.cpp
    Source/Panels/GameViewPanel.cpp
    Source/Panels/SceneViewPanel.cpp
    Source/Panels/SceneViewPanelMenus.cpp
    Source/Panels/SceneViewPanelQuad.cpp
    Source/Panels/SettingsPanel.cpp
    Source/Panels/SettingsPanelTreeScaling.cpp
	Source/Panels/CompletionPopup.cpp
	Source/Panels/ScriptEditorPanel.cpp
	Source/Panels/ScriptSyntax.cpp
	Source/Panels/ScriptTextArea.cpp
    Source/Panels/ColorPicker.cpp
    Source/Panels/ColorPickerPopup.cpp
    Source/Panels/SceneCameraSettingsPopup.cpp
    Source/ColorPicker/ColorPickerPresenter.cpp
    Source/ColorPicker/ColorPickerScope.cpp
    Source/ColorPicker/ColorPickerWindow.cpp
    Source/ColorPicker/ColorPickerEditorIntegration.cpp
    Source/ColorPicker/ColorPickerWindowInput.cpp
	Source/Panels/DiffPanel.cpp
    Source/Panels/AnimationWindowPanel.cpp
    Source/Panels/MixerPanel.cpp
    Source/Panels/TimelineBarElement.cpp
    Source/Panels/DopeSheetView.cpp
    Source/Panels/CurvesGraphView.cpp
    Source/Panels/TimeCompositeView.cpp
    Source/Panels/LaneClipEditorView.cpp
    Source/Panels/SequencerModelSerialization.cpp
    Source/Panels/TimeCompositeTimelineBridge.cpp
    Source/Panels/UIDemoPanel.cpp
    Source/Panels/MissingAssetsPanel.cpp
    Source/Panels/PackageManagerPanel.cpp
    Source/MissingAssetTracker.cpp
    Source/Panels/WebPanel.cpp
    Source/PlayMode/PlayModeManager.cpp
    Source/PlayMode/ModelAnimationPlayMode.cpp
    Source/PlayMode/AnimationPlayModeHelpers.cpp
    Source/PlayMode/RuntimeInputConnection.cpp
    Source/Scene/DefaultSceneEntities.cpp
    Source/Scene/SceneBackupRecovery.cpp
    Source/Scene/SceneDocumentManager.cpp
    Source/Scene/SceneLoadProgressOverlay.cpp
    Source/Scene/SceneEditorController.cpp
    Source/Scene/SceneThumbnailCapture.cpp
    Source/GameViewController.cpp
    Source/GameViewMovieCapture.cpp
    Source/MovieRecorderController.cpp
    Source/MovieRecorderToolbarMenu.cpp
    Source/SceneViewController.cpp
    Source/EditorComponentInstantiations.cpp
    Source/Scripting/EditorScriptMenuRegistry.cpp
	Source/Thumbnails/FolderBakeReport.cpp
	Source/Thumbnails/FolderBakeStatus.cpp
	Source/Thumbnails/SimpleThumbnailProvider.cpp
	Source/Thumbnails/ThumbnailService.cpp
	Source/Thumbnails/ThumbnailMenuItems.cpp
	Source/Thumbnails/ShaderGraphThumbnailMaterial.cpp
	Source/Thumbnails/ModelThumbnailHandler.cpp
	Source/Thumbnails/ModelThumbnailBakeQueue.cpp
	Source/Thumbnails/ModelThumbnailBakedImages.cpp
	Source/Thumbnails/ModelThumbnailCacheFiles.cpp
	Source/Thumbnails/ModelThumbnailImports.cpp
	Source/Thumbnails/ModelThumbnailPngPersistence.cpp
	Source/Thumbnails/ModelThumbnailPreviewScene.cpp
	Source/Thumbnails/ModelThumbnailRenderLanes.cpp
	Source/Thumbnails/TextureThumbnailHandler.cpp
	Source/Thumbnails/VideoThumbnailHandler.cpp
	Source/UI/InspectorSection.cpp
	Source/UI/EditorSearchBars.cpp
	Source/UI/UniversalSearchProvider.cpp
	Source/Search/UniversalSearchController.cpp
	Source/Packages/PackageManagerController.cpp
	Source/Display/HdrOutputController.cpp
	Source/Docking/LayoutPresetController.cpp
	Source/Display/HdrOutputMatch.cpp
	Source/Docking/FloatingFrameHost.cpp
	Source/Docking/WindowDockingController.cpp
	Source/UI/EditorTags.cpp
	Source/UI/ToolbarDragDrop.cpp
	Source/UI/UIAccentStyleHelper.cpp
	Source/UI/UICursorHelper.cpp
	Source/UI/UILabelHelpers.cpp
	Source/UI/DragDropGhostRenderers.cpp
	Source/UI/MaterialTexturePreviewHost.cpp
	Source/UI/ViewOverlayHost.cpp
	Source/UI/BuiltInViewOverlays.cpp
	Source/Assets/PendingAssetLoadsOverlay.cpp
	Source/Editor/Materials/ShaderCompileProgressOverlay.cpp
	Source/Editor/RenderPipeline/RenderPipelineStandInNotice.cpp
	Source/UI/Controls/SceneViewToolbar.cpp
	Source/UI/Controls/ScenePostFxMenu.cpp
	Source/UI/Controls/LayoutPresetToolbar.cpp
	Source/UI/Controls/DraggableModal.cpp
	Source/UI/Controls/EditorTopToolbar.cpp
	Source/UI/Controls/ItemSizeSlider.cpp
	Source/UI/Controls/InspectorNotice.cpp
	Source/UI/Controls/SkyDayKeyGradientBar.cpp
	Source/UI/Controls/CurveField.cpp
	Source/UI/Controls/NineSliceField.cpp
	Source/UI/Controls/CubicBezierField.cpp
	Source/UI/Controls/CurveTangents.cpp
	Source/UI/Controls/CurvePresets.cpp
	Source/UI/Controls/CurvePresetPicker.cpp
	Source/UI/Layout/DockConfigElements.cpp
	Source/UI/Layout/EditorDockConfigParser.cpp
	Source/Inspectors/TransformInspector.cpp
	Source/Inspectors/BuiltInInspectorRegistration.cpp
	Source/Inspectors/ComponentPresets.cpp
	Source/Inspectors/DefaultComponentInspector.cpp
	Source/Inspectors/GeneratedEntityNotice.cpp
	Source/Inspectors/InspectorEntityActivity.cpp
	Source/Inspectors/InspectorSectionHosts.cpp
	Source/Inspectors/PreservedFieldNotice.cpp
	Source/Inspectors/TerrainVolumeNotices.cpp
	Source/Inspectors/InspectorComponentSection.cpp
	Source/Inspectors/TerrainRuleNotices.cpp
	Source/Inspectors/TerrainSizingNotice.cpp
	Source/Inspectors/MeshRendererInspector.cpp
	Source/Inspectors/SkinnedMeshRendererInspector.cpp
	Source/Inspectors/AnimatorInspector.cpp
	Source/Inspectors/ValueCurveInspector.cpp
	Source/Inspectors/AnimationPreviewManager.cpp
	Source/Inspectors/CameraInspector.cpp
	Source/Inspectors/LightInspector.cpp
	Source/Inspectors/LensFlareSourceInspector.cpp
	Source/Inspectors/MeasureInspector.cpp
	Source/Inspectors/PhysicsBodyInspector.cpp
	Source/Inspectors/CharacterControllerInspector.cpp
	Source/Inspectors/PhysicsColliderInspector.cpp
	Source/Inspectors/MaterialInspector.cpp
	Source/Inspectors/MaterialInspectorSections.cpp
	Source/Inspectors/DeclaredPropertyRowModel.cpp
	Source/Inspectors/UndeclaredKeysNotice.cpp
	Source/Inspectors/ModelInspector.cpp
	Source/Inspectors/ModelSettingsReload.cpp
	Source/Inspectors/SubmeshLodProvenance.cpp
	Source/Inspectors/TextureCoveragePolicyNotice.cpp
	Source/Inspectors/TextureInspector.cpp
	Source/Inspectors/ShaderInspector.cpp
	Source/Inspectors/ScriptInspector.cpp
	Source/Inspectors/NativeSourceInspector.cpp
	Source/Inspectors/RenderPipelineInspector.cpp
	Source/Inspectors/TerrainMaterialLibraryInspector.cpp
	Source/Inspectors/SkyEnvironmentInspector.cpp
	Source/Inspectors/DayOfYearText.cpp
	Source/Inspectors/SkyDayCurveRow.cpp
	Source/Inspectors/LuxText.cpp
	Source/Inspectors/SkySunCurveReference.cpp
	Source/Inspectors/SkySunDriveRows.cpp
	Source/Inspectors/SkySunIlluminanceRow.cpp
	Source/Inspectors/SkySunPathRows.cpp
	Source/Inspectors/SkySunPathCaptions.cpp
	Source/Inspectors/SkySunReadoutCard.cpp
	Source/Inspectors/SkySunReadoutText.cpp
	Source/Inspectors/AmbientLightInspector.cpp
	Source/Inspectors/SkyboxInspector.cpp
	Source/Inspectors/OceanWaveSpectrumInspector.cpp
	Source/Inspectors/OceanDepthCacheSourceInspector.cpp
	Source/Inspectors/Particles/ParticleInspector.cpp
	Source/Inspectors/Particles/Controls/ParticlePreviewControls.cpp
	Source/Inspectors/Particles/ParticleStackInspector.cpp
	Source/Inspectors/Particles/ParticleStackEditor.cpp
	Source/Inspectors/Particles/ParticleParameterFields.cpp
	Source/Inspectors/Particles/ParticleProcessorSearchProvider.cpp
	Source/Inspectors/Particles/ParticleEmitterStackSection.cpp
	Source/Inspectors/Particles/ParticleStackReference.cpp
	Source/Inspectors/Particles/ParticleRendererInspector.cpp
	Source/Inspectors/PostProcessVolumeInspector.cpp
	Source/Inspectors/WindVolumeInspector.cpp
	Source/Inspectors/BloomEffectInspector.cpp
	Source/Inspectors/ChromaticAberrationEffectInspector.cpp
	Source/Inspectors/DepthOfFieldEffectInspector.cpp
	Source/Inspectors/FilmSimulationEffectInspector.cpp
	Source/Inspectors/AmbientOcclusionEffectInspector.cpp
	Source/Inspectors/ScreenSpaceReflectionsEffectInspector.cpp
	Source/Inspectors/ContrastAdaptiveSharpenEffectInspector.cpp
	Source/Inspectors/ExposureAdjustmentEffectInspector.cpp
	Source/Inspectors/CrtEffectInspector.cpp
	Source/Inspectors/FastBlurEffectInspector.cpp
	Source/Inspectors/HeatDistortionEffectInspector.cpp
	Source/Inspectors/VhsEffectInspector.cpp
	Source/Inspectors/HeightFogEffectInspector.cpp
	Source/Inspectors/ColorFilterEffectInspector.cpp
	Source/Inspectors/ColorGradeEffectInspector.cpp
	Source/Inspectors/CubeLutEffectInspector.cpp
	Source/Inspectors/DebandEffectInspector.cpp
	Source/Inspectors/VignetteEffectInspector.cpp
	Source/Inspectors/ShadowSettingsEffectInspector.cpp
	Source/Inspectors/AtmosphericCloudLayerInspector.cpp
	Source/Inspectors/VolumetricCloudsInspector.cpp
	Source/Inspectors/VolumetricFogEffectInspector.cpp
	Source/Inspectors/AudioInspector.cpp
	Source/Inspectors/AudioEmitterInspector.cpp
	Source/Inspectors/SkeletonProfileInspector.cpp
	Source/Inspectors/HumanoidRigInspector.cpp
	Source/Inspectors/RetargetMapInspector.cpp
	Source/Terrain/TerrainEntityProvisioning.cpp
	Source/Terrain/TerrainBaseSourceSection.cpp
	Source/Terrain/TerrainHeightmapInspector.cpp
	Source/Terrain/RawHeightmapSettingsCommit.cpp
	Source/Terrain/TerrainInspector.cpp
	Source/Terrain/TerrainPageCookOverlay.cpp
	Source/Terrain/TerrainModifierInspectors.cpp
	Source/Terrain/TerrainMaterialLibraryMint.cpp
	Source/Terrain/TerrainBakeCacheSave.cpp
	Source/Terrain/TerrainGrassVocabulary.cpp
	Source/Terrain/TerrainRoleMaterials.cpp
	Source/Terrain/TerrainRuleConditionVocabulary.cpp
	Source/Terrain/TerrainRuleWeightProfileStrip.cpp
	Source/Terrain/TerrainRuleRows.cpp
	Source/Terrain/TerrainInteractiveModifierEdit.cpp
	Source/Terrain/SphereSculptSaver.cpp
	Source/Terrain/TerrainZonePayloadSaver.cpp
	Source/Sky/SkyEnvironmentComponentTraits.cpp
	Source/Sky/SkyEnvironmentCreation.cpp
	Source/Sky/SkySunLinkUndo.cpp
	Source/Sky/SkyPathGizmo.cpp
	Source/Sky/SkySunDayKind.cpp
	Source/Sky/SkySunIlluminanceSwitch.cpp
	Source/Sky/SkySunPathSwitch.cpp
	Source/Sky/SkyEnvironmentEdit.cpp
	Source/Inspectors/SplineInspector.cpp
	Source/Inspectors/SplineFenceInspector.cpp
	Source/Inspectors/SplineFenceSpanPieceInspector.cpp
	Source/Inspectors/SplineFenceOverrideFields.cpp
	Source/Inspectors/SplinePlacementInspector.cpp
	Source/ExternalScriptEditorLauncher.cpp
	Source/ScriptEditor/SourceLocation.cpp
	Source/ScriptEditor/SourceLocationOpener.cpp
	Source/Editor/Assets/ShaderGlslOpen.cpp
	Source/Editor/Assets/AssetOpenRouting.cpp

	Modules/EditorToolbar/EditorToolbar.cpp
	Modules/EditorContextMenu/ContextMenuBackendPolicy.cpp
	Modules/EditorContextMenu/EditorContextMenu.cpp
	Modules/EditorContextMenu/MenuIconPreload.cpp
	Modules/EditorContextMenu/InterceptableContextMenu.cpp
	Modules/EditorContextMenu/UIContextMenu.cpp
	Modules/CssInspector/CssInspector.cpp

	Source/EditorPanelManager.cpp
	Source/EditorDebugMetrics.cpp
	Source/Assets/EditorAssetRegistrations.cpp
	Source/Graph/NodeColorSettings.cpp
	Source/Graph/GraphNodeRegistry.cpp
	Source/Graph/AnimationGraphNodeTypes.cpp
	Source/ShaderGraph/GraphPreviewMaterialRuntime.cpp
	Source/ShaderGraph/MaterialGraphNodePreviewMaterials.cpp
	Source/ShaderGraph/MaterialGraphPreviewModel.cpp
	Source/ShaderGraph/MaterialGraphPreviews.cpp
	Source/Graph/GraphKindChrome.cpp
	Source/Graph/GraphSubgraphStore.cpp
	Source/Graph/GraphNest.cpp
	Source/Graph/GraphBlendSpace1DStore.cpp
	Source/Graph/GraphBlendSpace2DStore.cpp
	Source/Graph/GraphTransitionStore.cpp
	Source/Graph/GraphAnimationRuntimeDebug.cpp
	Source/Graph/BlendSpace1DEditor.cpp
	Source/Graph/BlendSpace2DEditor.cpp
	Source/Graph/GraphCanvas.cpp
	Source/Graph/GraphRouteSolver.cpp
	Source/Graph/GraphNode.cpp
	Source/Graph/GraphNodeIcons.cpp
	Source/Graph/GraphPort.cpp
	Source/Graph/GraphPortedNode.cpp
	Source/ShaderGraph/MaterialGraphController.cpp
	Source/ShaderGraph/MaterialGraphDiagnosticsSummary.cpp
	Source/ShaderGraph/MaterialGraphVariables.cpp
	Source/ShaderGraph/MaterialGraphNodeTypes.cpp
	Source/ShaderGraph/MaterialGraphNode.cpp
	Source/ShaderGraph/GraphPreviewAtlas.cpp
	Source/ShaderGraph/GraphPreviewAtlasLayout.cpp
	Source/Graph/GraphPortDropSlot.cpp
	Source/Graph/GraphCanvasEditKeys.cpp
	Source/Graph/GraphNodePool.cpp
	Source/Graph/GraphOverlay.cpp
	Source/Graph/GraphNodeSummary.cpp
	Source/Graph/GraphVariableValues.cpp
	Source/Graph/GraphInspectorRows.cpp
	Source/Graph/GraphPanelFactory.cpp
	Source/Panels/GraphPanel.cpp
	Source/Panels/ShaderGraphPanel.cpp
	Source/Panels/AnimationGraphPanel.cpp
	Source/Panels/GameLogicGraphPanel.cpp
		Source/ShaderGraph/MaterialGraphPreviewHost.cpp

	# VCS glue (provider-agnostic; providers live in Packages/*-vcs)
	Source/VersionControl/EditorVersionControlService.cpp
	Source/VersionControl/EditorVersionControlUi.cpp
	Source/VersionControl/SceneDiff.cpp
	Source/VersionControl/SceneDiffLiveOverlay.cpp
	Source/VersionControl/SceneDiffVcs.cpp
	Source/VersionControl/Ui/HierarchyVcsController.cpp
	Source/VersionControl/Ui/InspectorVcsController.cpp
	Source/VersionControl/Ui/VcsCommitDialog.cpp
	Source/VersionControl/Ui/VcsLogViewer.cpp

	# Scene View tools + gizmos (GridGizmo, GizmoRenderContext-backed line/mesh accumulation, etc.).
	Source/SceneView/SceneViewGizmoThickLines.cpp
	Source/SceneView/SceneViewGizmos.cpp
	Source/SceneView/SceneViewTransformTool.cpp
	Source/SceneView/SceneViewSelectionTool.cpp
	Source/SceneView/NavDebugGizmo.cpp
	Source/SceneView/PostProcessVolumeGizmo.cpp
	Source/SceneView/Gizmos/ParticleEmitterGizmo.cpp
	Source/SceneView/WindVolumeGizmo.cpp
	Source/SceneView/LocalVolumeGizmo.cpp
	Source/SceneView/ComponentGizmoRegistry.cpp
	Source/SceneView/ComponentGizmos.cpp
	Source/SceneView/ReflectionProbeGizmo.cpp
	Source/SceneView/Gizmos/DDGIVolumeGizmo.cpp
	Source/SceneView/TerrainModifierGizmo.cpp
	Source/SceneView/LightGizmo.cpp
	Source/SceneView/CameraFrameGizmo.cpp
	Source/SceneView/NavGridBrushTool.cpp
	Source/SceneView/TerrainBrushTool.cpp
	Source/SceneView/MeasureSceneGizmo.cpp
	Source/SceneView/SplineTool.cpp
	Source/SceneView/SplineOwnerQuery.cpp
	Source/SceneView/SplineStrokeCapture.cpp
	Source/SceneView/SplineSceneGizmo.cpp
	Source/SceneView/SplineDrapePolylines.cpp
	Source/SceneView/SceneViewDropPlacement.cpp
	Source/SceneView/SceneViewNoticeOverlay.cpp
	Source/SceneView/SceneViewFraming.cpp
	Source/Markups/ConvertMarkupShapeCommand.cpp
	Source/Markups/MarkupEditorBridge.cpp
	Source/Markups/MarkupRequestGate.cpp
	Source/Markups/MarkupCommentText.cpp
	Source/Markups/MarkupDrawList.cpp
	Source/Markups/MarkupEditorSurfaces.cpp
	Source/Markups/MarkupGizmo.cpp
	Source/Markups/MarkupInspector.cpp
	Source/Markups/MarkupKind.cpp
	Source/Markups/MarkupLabelOverlay.cpp
	Source/Markups/MarkupNotesEditCommand.cpp
	Source/Markups/MarkupPresentation.cpp
	Source/Markups/MarkupRenderFeature.cpp
	Source/Markups/MarkupPathPort.cpp
	Source/Markups/MarkupRegionDisplay.cpp
	Source/Markups/MarkupInspectorRows.cpp
	Source/Markups/MarkupPathInspector.cpp
	Source/Markups/MarkupRegionInspector.cpp
	Source/Markups/MarkupRegionPort.cpp
	Source/Markups/MarkupRequests.cpp
	Source/Markups/MarkupTool.cpp
	Source/Markups/MarkupsPanel.cpp
	Source/Markups/MarkupsPanelRows.cpp
	Source/SceneView/SceneViewToolStrip.cpp
	Source/SceneView/SceneViewToolStripRegistry.cpp
	Source/SceneView/BuiltInSceneViewTools.cpp
	Source/SceneView/SplineToolMenu.cpp
	Source/SceneView/SplineToolStripEntry.cpp
	Source/SceneView/TerrainBrushToolStripEntry.cpp
	Source/SceneView/TerrainPresence.cpp
	Source/SceneView/SceneViewProjection.cpp
	Source/SceneView/SceneViewPostProcessSwitches.cpp
	Source/SceneView/EditorSceneViewDropWiring.cpp
	Source/SceneView/SceneViewEntityLinks.cpp
	Source/SceneView/GizmoSettingsWiring.cpp
	Source/SceneView/SceneViewGridRenderer.cpp
	Source/SceneView/SceneViewEvents.cpp
	Source/SceneView/SceneViewOverlayShared.cpp
	Source/SceneView/SceneViewOverlaysRG.cpp
	Source/SceneView/SelectionOutlineContributors.cpp
	Source/SceneView/SceneViewRenderCoordinator.cpp
	Source/SceneView/ViewPresentationSnapshot.cpp
	Source/SceneView/ViewSnapshotTexture.cpp
	# Spline placement (tiles and fences along a spline, conformed via the editor raycast).
	Source/Placement/FenceEmission.cpp
	Source/Placement/FenceMitreVariants.cpp
	Source/Placement/FencePieceReuse.cpp
	Source/Placement/FenceRecipeLayout.cpp
	Source/Placement/FenceSpanOverrideRemap.cpp
	Source/Placement/FenceSpanOverrides.cpp
	Source/Placement/PlacementGroundGap.cpp
	Source/Placement/RecipeControllers.cpp
	Source/Placement/RecipeValidation.cpp
	Source/Placement/SplineFillRebuild.cpp
	Source/Placement/SplineChunkCommit.cpp
	Source/Placement/SplineChunkSlots.cpp
	Source/Placement/SplineExtrudeController.cpp
	Source/Placement/SplineExtrudeSanitize.cpp
	Source/Placement/SplineFenceController.cpp
	Source/Placement/SplineGroundAuthority.cpp
	Source/Placement/SplinePieceMaterial.cpp
	Source/Placement/SplinePlacementController.cpp
	Source/Placement/SplineRebuildGate.cpp
	Source/Placement/SplineSurfaceConform.cpp
	Source/Placement/SplineSweepStations.cpp
	Source/Placement/SplineWallBuild.cpp
	Source/Placement/SplineWallController.cpp
	Source/Placement/SplineWallSanitize.cpp
	# Mesh picking service (shared per-triangle CPU raycaster).
	Source/Picking/MeshPickingService.cpp
	Source/Picking/MeshBvhCache.cpp
	Source/Picking/MeshBvhDiskFormat.cpp
	Source/Picking/PickRootResolver.cpp
	Source/Picking/PrimitiveMeshes.cpp
	Source/Picking/TerrainPicking.cpp
	Source/EditorApplication_InspectorSettings.cpp
	Source/Editor/Settings/EditorHiDpiPlatformSettings.cpp
	Source/EditorApplication_Fullscreen.cpp
	Source/EditorApplication_Boot.cpp
	Source/EditorApplication_AssetPreview.cpp
	Source/EditorApplication_LayoutPresets.cpp
	Source/EditorApplication_Windows.cpp
	Source/EditorApplication_Shortcuts.cpp
	Source/Editor/Shortcuts/EditorShortcuts.cpp
	Source/EditorInputActions.cpp
	Source/DebugServer/EditorDebugServer.cpp
	Source/DebugServer/BenchSceneHandlers.cpp
	Source/DebugServer/CameraProjectionReport.cpp
	Source/DebugServer/TerrainSplitThresholdReport.cpp
	Source/DebugServer/SceneHierarchyReport.cpp
	Source/DebugServer/ComponentValueReader.cpp
	Source/DebugServer/SchemaComponentApply.cpp
	Source/DebugServer/DebugHandlers.cpp
	Source/DebugServer/DebugServerReply.cpp
	Source/DebugServer/ElementPointerTarget.cpp
	Source/DebugServer/AssistantSessionHandlers.cpp
	Source/DebugServer/GpuToolingHandlers.cpp
	Source/DebugServer/GroundQueryDebugHandlers.cpp
	Source/DebugServer/JobSystemDebugHandlers.cpp
	Source/DebugServer/JobSystemReport.cpp
	Source/DebugServer/MarkupDebugHandlers.cpp
	Source/DebugServer/SkyDebugHandlers.cpp
	Source/DebugServer/ThumbnailDebugHandlers.cpp
	Source/DebugServer/ParallaxDebugHandlers.cpp
	Source/DebugServer/ParallaxStepsViewRequest.cpp
	Source/DebugServer/InjectedInput.cpp
	Source/DebugServer/OpenAssetPath.cpp
	Source/DebugServer/PhysicsDebugHandlers.cpp
	Source/DebugServer/PhysicsRaycastRequest.cpp
	Source/DebugServer/PlayModeRequest.cpp
	Source/DebugServer/PerformDropRequest.cpp
	Source/DebugServer/PointerCoordsRequest.cpp
	Source/DebugServer/SceneDegradedReport.cpp
	Source/DebugServer/RenderGraphResourceFilter.cpp
	Source/DebugServer/ScreenshotCaptureWait.cpp
	Source/DebugServer/TerrainDebugHandlers.cpp
	Source/DebugServer/TerrainStrokeTarget.cpp
	Source/DebugServer/UITreeDump.cpp
	Source/DebugServer/ViewFocusRequest.cpp
	Source/DebugServer/RenderDocCapture.cpp
	Source/DebugServer/NsightGraphicsCapture.cpp
	Source/DebugServer/WindowPrintCapture.cpp
)

set(EDITOR_HEADERS
    Source/EditorApplication.h
    Source/EditorDockNodeJson.h
	Source/DebugServer/EditorDebugServer.h
	Source/DebugServer/BenchSceneHandlers.h
	Source/DebugServer/CameraProjectionReport.h
	Source/DebugServer/TerrainSplitThresholdReport.h
	Source/DebugServer/SceneHierarchyReport.h
	Source/DebugServer/ComponentValueReader.h
	Source/DebugServer/SchemaComponentApply.h
	Source/DebugServer/DebugHandlers.h
	Source/DebugServer/DebugServerReply.h
	Source/DebugServer/ElementPointerTarget.h
	Source/DebugServer/GpuToolingHandlers.h
	Source/DebugServer/GroundQueryDebugHandlers.h
	Source/DebugServer/JobSystemDebugHandlers.h
	Source/DebugServer/JobSystemReport.h
	Source/DebugServer/SkyDebugHandlers.h
	Source/DebugServer/ParallaxDebugHandlers.h
	Source/DebugServer/ParallaxStepsViewRequest.h
	Source/DebugServer/InjectedInput.h
	Source/DebugServer/OpenAssetPath.h
	Source/DebugServer/PhysicsDebugHandlers.h
	Source/DebugServer/PhysicsRaycastRequest.h
	Source/DebugServer/PlayModeRequest.h
	Source/DebugServer/PointerCoordsRequest.h
	Source/DebugServer/SceneDegradedReport.h
	Source/DebugServer/RenderGraphResourceFilter.h
	Source/DebugServer/TerrainDebugHandlers.h
	Source/DebugServer/TerrainStrokeTarget.h
	Source/DebugServer/UITreeDump.h
	Source/DebugServer/ViewFocusRequest.h
	Source/DebugServer/RenderDocCapture.h
	Source/DebugServer/NsightGraphicsCapture.h
	Source/DebugServer/WindowPrintCapture.h
	Source/Startup/EditorCommandLine.h
	Source/Startup/EditorPathPolicy.h
	Source/Startup/EditorSessionDescriptor.h
	Source/Startup/EditorStartup.h
	Source/Telemetry/RGCsvSidecar.h
	Source/Automation/EditorSmokeHooks.h
	Source/Automation/UiReplayRunner.h
	Include/Automation/UiReplayCommandIds.h
	Include/Editor/EditorPaths.h
	Include/Editor/Registries/EditorPluginRegistry.h
	Include/Editor/Settings/EditorHiDpiPlatformSettings.h
	Include/Editor/Settings/CurveEditorSettings.h
	Include/Editor/Settings/SettingsStore.h
    Include/EditorInputActions.h
    Include/UndoRedo/CreateTextFileCommand.h
    Include/UndoRedo/GenericEditUndo.h
    Include/UndoRedo/RenameAssetFileCommand.h
    Include/UndoRedo/SetAssetMetaValueCommand.h
    Include/UndoRedo/DeleteAssetPathsCommand.h
    Include/PlayMode/PlayModeManager.h
    Include/PlayMode/RuntimeInputConnection.h
    Include/UndoRedo/VcsRevertCommand.h
    Include/UndoRedo/TagCommands.h
    Include/UndoRedo/MultiEntityComponentSnapshot.h
    Include/Inspectors/TransformInspector.h
	Include/Inspectors/MeshRendererInspector.h
	Include/Inspectors/SkinnedMeshRendererInspector.h
	Include/Inspectors/ValueCurveInspector.h
	Include/Inspectors/CameraInspector.h
	Include/Inspectors/LightInspector.h
	Include/Inspectors/MeasureInspector.h
	Include/Inspectors/PhysicsBodyInspector.h
	Include/Inspectors/CharacterControllerInspector.h
	Include/Inspectors/PhysicsColliderInspector.h
	Include/Inspectors/MaterialInspector.h
	Include/Inspectors/ShaderInspector.h
	Include/Inspectors/ScriptInspector.h
	Include/Inspectors/RenderPipelineInspector.h
	Include/Inspectors/SkyEnvironmentInspector.h
	Include/Inspectors/DayOfYearText.h
	Include/Inspectors/SkyDayCurveRow.h
	Include/Inspectors/LuxText.h
	Include/Inspectors/SkySunCurveReference.h
	Include/Inspectors/SkySunDriveRows.h
	Include/Inspectors/SkySunIlluminanceRow.h
	Include/Inspectors/SkySunPathRows.h
	Include/Inspectors/SkySunPathCaptions.h
	Include/Inspectors/SkySunReadoutCard.h
	Include/Inspectors/SkySunReadoutText.h
	Include/Inspectors/SkyboxInspector.h
	Include/Sky/SkyEnvironmentComponentTraits.h
	Include/Sky/SkyEnvironmentCreation.h
	Include/Sky/SkySunLinkUndo.h
	Include/Sky/SkyPathGizmo.h
	Include/Sky/SkySunDayKind.h
	Include/Sky/SkySunIlluminanceSwitch.h
	Include/Sky/SkySunPathSwitch.h
	Include/Sky/SkyEnvironmentEdit.h
	Include/Inspectors/OceanWaveSpectrumInspector.h
	Include/Inspectors/Particles/ParticleInspector.h
	Include/Inspectors/Particles/Controls/ParticlePreviewControls.h
	Include/Inspectors/Particles/ParticleStackEditor.h
	Include/Inspectors/Particles/ParticleParameterFields.h
	Include/Inspectors/Particles/ParticleProcessorSearchProvider.h
	Include/Inspectors/Particles/ParticleStackInspector.h
	Include/Inspectors/Particles/ParticleEmitterStackSection.h
	Include/Inspectors/Particles/ParticleStackReference.h
	Include/Inspectors/Particles/ParticleRendererInspector.h
	Include/Inspectors/WindVolumeInspector.h
	Include/Inspectors/AudioInspector.h
	Include/Inspectors/AudioEmitterInspector.h
	Include/Inspectors/SkeletonProfileInspector.h
	Include/Inspectors/HumanoidRigInspector.h
	Include/Inspectors/RetargetMapInspector.h
	Include/UI/ActivateOnRelease.h
	Include/UI/InspectorSection.h
	Include/UI/EditorSearchBars.h
	Include/UI/UniversalSearchProvider.h
	Include/Search/UniversalSearchController.h
	Include/Packages/PackageManagerController.h
	Include/Display/HdrOutputController.h
	Include/Display/HdrOutputMatch.h
	Include/Docking/FloatingFrameHost.h
	Include/Docking/WindowDockingController.h
	Include/UI/DragDropGhostRenderers.h
	Include/UI/SyntaxHighlightSettings.h
	Include/UI/ToolbarDragDrop.h
	Include/UI/Controls/SceneViewToolbar.h
	Include/UI/Controls/LayoutPresetToolbar.h
	Include/UI/Controls/DraggableModal.h
	Include/UI/Controls/EditorTopToolbar.h
	Include/UI/Controls/SkyDayKeyGradientBar.h
	Include/UI/Controls/CurveField.h
	Include/UI/Controls/ReorderableSectionList.h
	Include/UI/Controls/InspectorActionButton.h
	Include/UI/Controls/CubicBezierField.h
	Include/UI/Controls/CurveTangents.h
	Include/UI/Controls/CurvePresets.h
	Include/UI/Controls/CurvePresetPicker.h
	Include/EditorPanelManager.h
	Include/UI/Layout/DockConfigElements.h
	Source/UI/Layout/EditorDockConfigParser.h
    Include/Assets/AssetDeletePathGuard.h
    Include/Assets/AssetPipelineCommands.h
    Include/Assets/AssetRename.h
    Include/Assets/AssetRenameController.h
    Include/Assets/AssetsBrowserController.h
    Include/Assets/EditorAssetActionsInstall.h
    Include/Assets/AssetsDataProviders.h
	Include/ExternalScriptEditorLauncher.h
	Include/ScriptEditor/SourceLocation.h
	Include/ScriptEditor/SourceLocationOpener.h
	Include/Editor/Assets/ShaderGlslOpen.h
	Include/Editor/Assets/AssetOpenRouting.h
	Include/Editor/Assets/EditorAssetActions.h
	Include/Editor/CaptureOutputDirectory.h
    Include/Panels/AssetsPanel.h
    Include/Panels/TodoPanel.h
    Include/Panels/BookmarksPanel.h
    Include/Panels/BuiltInPanelTypes.h
    Include/Panels/UndoHistoryPanel.h
		Include/Panels/HierarchyPanel.h
		Include/Editor/Entities/CopiedEntityRuntimeRefs.h
		Include/Editor/Entities/EntityDuplicate.h
		Include/Editor/Hierarchy/HierarchyEntityIcon.h
    Include/Panels/LogPanel.h
    Include/Panels/LogView.h
    Include/Panels/ScriptDiagnosticsByAssembly.h
    Include/Panels/ScriptErrorsPanel.h
    Include/Panels/SaveSceneAsModal.h
    Include/Panels/ConfirmActionModal.h
    Include/Panels/SaveSceneChangesModal.h
    Include/Panels/RestoreSceneBackupModal.h
    Include/Panels/RenameLayoutModal.h
    Include/Panels/CameraCustomAspectModal.h
    Include/Panels/AddTagModal.h
    Include/Scripting/EditorScriptMenuRegistry.h
	Include/Panels/InspectorPanel.h
	Include/Panels/ProjectFolderPickerModal.h
    Include/Panels/PlayModeChangeReviewModal.h
    Include/Panels/VideoPlayerModal.h
    Include/EditorVideoPlayerController.h
    Include/Panels/GameViewPanel.h
	Include/Panels/SceneViewPanel.h
	Include/Panels/SettingsPanel.h
	Include/Panels/CompletionPopup.h
	Include/Panels/ScriptEditorPanel.h
	Include/Panels/ScriptSyntax.h
	Include/Panels/ScriptTextArea.h
	Include/Panels/GraphKindHooks.h
	Include/Panels/GraphPanel.h
	Include/Panels/ColorPicker.h
	Include/Graph/NodeColorSettings.h
	Include/Graph/GraphNodeRegistry.h
	Include/Graph/GraphKindChrome.h
	Include/Graph/GraphSubgraphStore.h
	Include/Graph/GraphNest.h
	Include/Graph/GraphBlendSpace1DStore.h
	Include/Graph/BlendSpace1DEditor.h
	Include/Graph/GraphCanvas.h
	Include/Graph/GraphNode.h
	Include/Graph/GraphNodeIcons.h
	Include/Graph/GraphPort.h
	Include/Graph/GraphPortedNode.h
	Include/ShaderGraph/MaterialGraphController.h
	Include/ShaderGraph/MaterialGraphVariables.h
	Include/ShaderGraph/MaterialGraphNodeTypes.h
	Include/ShaderGraph/MaterialGraphNode.h
	Include/ShaderGraph/MaterialGraphPreviews.h
	Include/ShaderGraph/MaterialGraphPreviewHost.h
	Include/ShaderGraph/GraphNodePreviewBinding.h
	Include/ShaderGraph/GraphPreviewAtlas.h
	Include/ShaderGraph/GraphPreviewAtlasLayout.h
	Include/Graph/GraphPortDropSlot.h
	Include/Graph/GraphCanvasEditKeys.h
	Include/Graph/GraphNodePool.h
	Include/Graph/GraphOverlay.h
	Include/Graph/GraphNodeSummary.h
	Include/Graph/GraphInspectorRows.h
	Include/Panels/ColorPickerPopup.h
	Include/Panels/SceneCameraSettingsPopup.h
	Include/ColorPicker/ColorPickerPresenter.h
	Include/ColorPicker/ColorPickerScope.h
	Include/ColorPicker/ColorPickerWindow.h
	Include/ColorPicker/ColorPickerContext.h
	Source/ColorPicker/ColorPickerWindowInput.h
	Include/Panels/DiffPanel.h
	Include/Panels/AnimationWindowPanel.h
	Include/Panels/TimelineBarElement.h
	Include/Panels/DopeSheetView.h
	Include/Panels/CurvesGraphView.h
	Include/Panels/TimeCompositeModel.h
	Include/Panels/TimeCompositeView.h
	Include/Panels/LaneClipModel.h
	Include/Panels/LaneClipEditorView.h
	Include/Panels/UIDemoPanel.h
	Include/Panels/MissingAssetsPanel.h
	Include/MissingAssetTracker.h
	Include/Panels/WebPanel.h
    Include/Scene/DefaultSceneEntities.h
    Include/Scene/SceneBackupRecovery.h
    Include/Scene/SceneDocumentManager.h
    Include/Scene/SceneEditorController.h
    Include/Scene/SceneThumbnailCapture.h
    Include/GameViewController.h
    Include/GameViewMovieCapture.h
    Include/MovieRecorderToolbarMenu.h
	Include/SceneViewController.h
	Include/SceneView/GizmoSettingsWiring.h
	Include/SceneView/SceneViewGridRenderer.h
	Include/SceneView/SceneViewDropPlacement.h
	Include/SceneView/SceneViewNoticeOverlay.h
	Include/SceneView/SceneViewRenderCoordinator.h
	Include/SceneView/ViewPresentationSnapshot.h
	Include/SceneView/ViewSnapshotTexture.h
	Include/SceneView/ReflectionProbeGizmo.h
	Include/SceneView/ComponentGizmoRegistry.h
	Include/SceneView/ComponentGizmos.h
	Include/SceneView/PostProcessVolumeGizmo.h
	Include/SceneView/WindVolumeGizmo.h
	Include/SceneView/LocalVolumeGizmo.h
	Include/Editor/Assets/AsyncAssetHelpers.h
	Include/Editor/Entities/ComponentEnabledToggle.h
	Include/Editor/Entities/ComponentRemoval.h
	Include/Editor/Entities/EntityMaterialTextureAssign.h
	Include/Editor/Entities/SkyboxEntityFactory.h
	Include/Editor/Entities/SpriteEntityFactory.h
	Include/SceneView/EditorSceneViewDropWiring.h
	Include/SceneView/SceneViewEntityLinks.h
	Modules/EditorToolbar/EditorToolbar.h
	Modules/EditorContextMenu/ContextMenuBackendPolicy.h
	Modules/EditorContextMenu/EditorContextMenu.h
	Modules/EditorContextMenu/InterceptableContextMenu.h
	Modules/EditorContextMenu/MenuIconPreload.h
	Modules/EditorContextMenu/UIContextMenu.h
	Modules/CssInspector/CssInspector.h
        Include/UI/Controls/Widgets/CameraBookmarksWidget.h
		Source/UI/Controls/Widgets/CameraBookmarksWidget.cpp
        Include/UI/Controls/Widgets/ViewportRotationGizmo.h
		Source/UI/Controls/Widgets/ViewportRotationGizmo.cpp
        Include/UI/Controls/Widgets/SceneViewRulerOverlay.h
		Source/UI/Controls/Widgets/SceneViewRulerOverlay.cpp
        Include/UI/Controls/Widgets/SceneViewMeasureOverlay.h
		Source/UI/Controls/Widgets/SceneViewMeasureOverlay.cpp
	Include/VersionControl/EditorVersionControlService.h
	Include/VersionControl/EditorVersionControlUi.h
	Include/VersionControl/Ui/VcsCommitDialog.h
	Include/VersionControl/Ui/VcsLogViewer.h
)

# EditorSDK source list. On desktop these become a SHARED library (see the
# block below); in a single static wasm binary they compile straight into the
# executable, because every reason that library exists — one process-wide
# registry instance across plugin DLLs, headless module hosting, a curated
# export ABI — presupposes dynamic linking the target does not have.
set(EDITOR_SDK_SOURCES
	Source/Editor/Registries/BuildExporterRegistry.cpp
	Source/Editor/Registries/DebugRequestGateRegistry.cpp
	Source/Editor/Registries/EditorPluginRegistry.cpp
	Source/Editor/Entities/DDGIVolumeComponentTraits.cpp
	Source/Editor/Entities/EditorComponentTraits.cpp
	Source/Editor/Registries/EditorPanelRegistry.cpp
	Source/Editor/Registries/EditorMenuRegistry.cpp
	Source/Editor/Registries/EditorSceneCommands.cpp
	Source/Editor/Settings/EditorSettingsRegistry.cpp
	# Info-card look (background/outline/shadow/visibility): in the SDK because
	# UI/InfoCard.h reads it, and package modules build cards through that.
	Source/Editor/Settings/InfoCardAppearanceSettings.cpp
	Source/Editor/Vcs/EditorVcsProviderRegistry.cpp
	# Inspector row rules that are pure functions of a material document, and
	# the by-name hand-off of its textures to the runtime material — in the SDK
	# so a headless test target can link and assert them.
	Source/Editor/Materials/EmissiveMapAssignment.cpp
	Source/Editor/Materials/MaterialRows.cpp
	Source/Editor/Materials/MaterialUndoSnapshot.cpp
	Source/Editor/Materials/MaterialRuntimeTextures.cpp
	Source/Editor/Vcs/VcsStatusUi.cpp
	Source/Editor/Vcs/VcsSettingsUi.cpp
	# Scripting-host bridge for the unity-import package's hosted converter:
	# lives in the SDK (not the package module) because its TU compiles against
	# NETHOST-layout-sensitive scripting headers the staged-SDK module build
	# shape does not define. The header is std-only.
	Source/Editor/Assets/UnityImportHostedConverter.cpp
	Source/Editor/Assets/EditorAssetActions.cpp
	Source/Editor/CaptureOutputDirectory.cpp
	Source/EditorPaths.cpp
	Source/SettingsStore.cpp
	Source/InspectorRegistry.cpp
	Source/Picking/EditorPickProviders.cpp
	Source/Assets/AssetSearchProvider.cpp
	Source/UI/AssetBoundDockPanel.cpp
	Source/UI/ReferenceFieldBase.cpp
	Source/UI/AssetField.cpp
	Source/UI/ActivateOnRelease.cpp
	Source/UI/Controls/CollapsibleInfoCard.cpp
	Source/UI/Controls/ReorderableSectionList.cpp
	Source/UI/Controls/InspectorActionButton.cpp
	Source/UI/Controls/EntityLink.cpp
	Source/UI/EntityField.cpp
	Source/Editor/Entities/EntityDisplayName.cpp
)

# Vendored miniz (zip reader for project acquisition).
set(EDITOR_MINIZ_SOURCES
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tdef.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_tinfl.c
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/miniz_zip.c
)

# Editor/BuildCommit.h: the short commit id the title bar and the info dialog show beside the
# engine version (Source/Editor/Application/EditorVersion.cpp). The target runs on every
# build and rewrites the header only when the id changed.
find_package(Git QUIET)
set(GE_EDITOR_BUILD_COMMIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(EditorBuildCommit
	COMMAND ${CMAKE_COMMAND}
		"-DGIT=${GIT_EXECUTABLE}"
		"-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
		"-DOUTPUT=${GE_EDITOR_BUILD_COMMIT_DIR}/Editor/BuildCommit.h"
		-P "${CMAKE_SOURCE_DIR}/cmake/WriteBuildCommit.cmake"
	BYPRODUCTS "${GE_EDITOR_BUILD_COMMIT_DIR}/Editor/BuildCommit.h"
	COMMENT "Recording the editor's build commit"
	VERBATIM)

# ---------------------------------------------------------------------------
# Web Editor (wasm): the editor as one static binary on a single canvas.
#
# Everything below this block is desktop shape that has no wasm equivalent —
# a SHARED EditorSDK, MODULE package plugins, runtime-dependency and SDK
# staging built around import libs, .app bundles and codesigning. The wasm
# target therefore stops here rather than reusing that machinery with a dozen
# guards threaded through it.
#
# Surfaces the browser has no answer for are not silently dropped: they are
# implemented as web stubs at the Platform seam that log once and report
# failure (Shell, WebView, subprocess), so a caller sees "unavailable on web"
# instead of a no-op that looks like success.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
	# At -O0 the browser's ~1 MB native call stack holds about 16 Yoga layout
	# levels, so a Debug wasm editor throws RangeError inside its first frame's
	# layout pass, before anything is drawn (the mechanism is documented at the
	# root CMakeLists' GE_WASM link budget). The expensive frames are Yoga's
	# and the engine's, and yogacore comes from vcpkg at the triplet's level,
	# so no per-target optimisation flag can raise the floor: the editor is a
	# non-Debug target on wasm, and refuses here rather than in the browser.
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		add_custom_target(WebEditor
			COMMAND ${CMAKE_COMMAND} -E echo
				"WebEditor is not built in the Debug config on wasm: at -O0 the editor's first UI layout pass exhausts the browser's ~1MB call stack. Build it from the wasm-release preset instead: cmake --preset wasm-release, then cmake --build build/wasm-release --target WebEditor."
			COMMAND ${CMAKE_COMMAND} -E false
			VERBATIM)
		return()
	endif()

	set(GE_WEB_SHELL ${CMAKE_SOURCE_DIR}/Tools/Web/shell.html)
	# main.cpp is a desktop host (SEH crash dumps, per-process log file, the
	# GameEngine.Native ABI handshake, device-loss relaunch args); WebMain.cpp
	# drives the same EditorApplication from the browser's rAF instead.
	set(WEB_EDITOR_SOURCES ${EDITOR_SOURCES})
	list(REMOVE_ITEM WEB_EDITOR_SOURCES Source/main.cpp)
	list(APPEND WEB_EDITOR_SOURCES Source/WebMain.cpp)
	add_executable(WebEditor
		${WEB_EDITOR_SOURCES}
		${EDITOR_HEADERS}
		${EDITOR_SDK_SOURCES}
		${EDITOR_MINIZ_SOURCES}
		$<TARGET_OBJECTS:VCSIntegration>
		$<TARGET_OBJECTS:MeshPicking>
	)
	add_dependencies(WebEditor EditorBuildCommit)
	target_include_directories(WebEditor PRIVATE "${GE_EDITOR_BUILD_COMMIT_DIR}")
	# Tree Generator (eztree) ships as two MODULE DLLs on desktop. Wasm cannot
	# dlopen them, and this file return()s before ge_add_engine_package_module.
	# Compile the same TUs into WebEditor so the static-init registrars run
	# before EnginePluginRegistry's one-shot fan-out. EZTreeSharedOptions.cpp
	# is the editor DLL's copy of EZTreeOptions.cpp — Native already has that
	# TU, and compiling both into one binary duplicates the helpers.
	set(_GE_EZTREE_NATIVE ${CMAKE_SOURCE_DIR}/Packages/eztree/Native)
	set(_GE_EZTREE_EDITOR ${CMAKE_SOURCE_DIR}/Packages/eztree/Editor)
	file(GLOB_RECURSE _ge_eztree_native_sources CONFIGURE_DEPENDS
		"${_GE_EZTREE_NATIVE}/*.cpp")
	file(GLOB_RECURSE _ge_eztree_editor_sources CONFIGURE_DEPENDS
		"${_GE_EZTREE_EDITOR}/*.cpp")
	list(FILTER _ge_eztree_editor_sources EXCLUDE REGEX "EZTreeSharedOptions\\.cpp$")
	target_sources(WebEditor PRIVATE
		${_ge_eztree_native_sources}
		${_ge_eztree_editor_sources})
	target_include_directories(WebEditor PRIVATE
		${CMAKE_SOURCE_DIR}/Engine/Include
		${CMAKE_SOURCE_DIR}/Engine/Modules/Audio/Include
		${CMAKE_SOURCE_DIR}/Engine/Modules/VCSIntegration/Include
		${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz
		${CMAKE_CURRENT_SOURCE_DIR}/Source
		${CMAKE_CURRENT_SOURCE_DIR}/Include
		${CMAKE_CURRENT_SOURCE_DIR}/Modules
		${_GE_EZTREE_NATIVE}
		${_GE_EZTREE_EDITOR}
		${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/Include
		${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include
		$<TARGET_PROPERTY:MeshPicking,INTERFACE_INCLUDE_DIRECTORIES>
	)
	target_compile_definitions(WebEditor PRIVATE
		GE_BUILD_CONFIG="$<CONFIG>"
		GE_ENABLE_MIMALLOC=0
		$<TARGET_PROPERTY:MeshPicking,INTERFACE_COMPILE_DEFINITIONS>
	)
	target_compile_features(WebEditor PRIVATE cxx_std_20)
	target_link_libraries(WebEditor PRIVATE Engine Types Mathematics Events AssetDatabaseSqlite)
	if(TARGET nlohmann_json::nlohmann_json)
		target_link_libraries(WebEditor PRIVATE nlohmann_json::nlohmann_json)
	endif()
	if(TARGET stb::stb)
		target_link_libraries(WebEditor PRIVATE stb::stb)
		target_compile_definitions(WebEditor PRIVATE GE_HAVE_STB)
	elseif(STB_INCLUDE_DIRS)
		target_include_directories(WebEditor PRIVATE ${STB_INCLUDE_DIRS})
		target_compile_definitions(WebEditor PRIVATE GE_HAVE_STB)
	endif()
	set_target_properties(WebEditor PROPERTIES
		SUFFIX ".html"
		UNITY_BUILD OFF
		LINK_DEPENDS ${GE_WEB_SHELL})
	if(GE_WASM_SINGLE_THREAD)
		set(_ge_webeditor_env "SHELL:-s ENVIRONMENT=web")
	else()
		set(_ge_webeditor_env "SHELL:-s ENVIRONMENT=web,worker")
	endif()
	# The browser ingests WGSL, never SPIR-V, and this build compiles no
	# shaders: GLSLC_EXECUTABLE comes from the vcpkg shaderc port, which the
	# wasm triplet has no host tools for, and .shaderpkg reflection metadata is
	# produced by a host-native ShaderReflect. So the WGSL cook reads a desktop
	# build's Shaders/ directory, exactly as the Player's export does, and
	# writes the editor's set here for the preload below.
	set(GE_WEB_EDITOR_SHADER_DIR ${CMAKE_BINARY_DIR}/WebEditorShaders)
	set(GE_WEB_SHADERPKG_DIR "" CACHE PATH
		"Desktop build's Shaders/ directory, read by the web editor's WGSL cook")
	# This input is another build tree's output, so no file-level dependency can
	# catch it going missing or going stale. Both are checked here, where the
	# message can state the fix. A cache entry pointing at a deleted build
	# otherwise fails deep inside the cook, and one pointing at a *foreign*
	# checkout is worse than a failure: it silently cooks the editor's shaders
	# from a commit other than the one being linked.
	if(GE_WEB_SHADERPKG_DIR)
		if(NOT IS_DIRECTORY "${GE_WEB_SHADERPKG_DIR}")
			message(FATAL_ERROR
				"GE_WEB_SHADERPKG_DIR is '${GE_WEB_SHADERPKG_DIR}', which is not a directory. "
				"Build the desktop shader set (cmake --build <desktop build> --target CompileShaderPkgs) "
				"and reconfigure with -DGE_WEB_SHADERPKG_DIR=<that build>/Shaders.")
		endif()
		file(GLOB _ge_webeditor_shaderpkgs "${GE_WEB_SHADERPKG_DIR}/*.shaderpkg")
		if(NOT _ge_webeditor_shaderpkgs)
			message(FATAL_ERROR
				"GE_WEB_SHADERPKG_DIR '${GE_WEB_SHADERPKG_DIR}' holds no .shaderpkg files. "
				"Build the CompileShaderPkgs target in that desktop build before configuring this one.")
		endif()
		cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${GE_WEB_SHADERPKG_DIR}" NORMALIZE
			_ge_webeditor_shaderpkg_in_tree)
		if(NOT _ge_webeditor_shaderpkg_in_tree)
			message(WARNING
				"GE_WEB_SHADERPKG_DIR '${GE_WEB_SHADERPKG_DIR}' is outside this source tree "
				"(${CMAKE_SOURCE_DIR}): WebEditor will cook its shaders from another checkout, which "
				"can be a different commit than the sources it links, and which this tree does not "
				"keep alive. Point it at a desktop build of this tree unless the skew is intended.")
		endif()
	endif()
	find_program(GE_PYTHON3 NAMES python3 python)
	set(_ge_webeditor_cook_stamp ${CMAKE_BINARY_DIR}/WebEditorShaders.stamp)
	file(GLOB _ge_webeditor_desktop_shaderpkgs CONFIGURE_DEPENDS
		"${GE_WEB_SHADERPKG_DIR}/*.shaderpkg")
	# Every module root ResolveShaderSource searches, not just Rendering/Shaders:
	# a package whose sources another module owns (VolumetricClouds) would
	# otherwise cook once and never again, and that staleness surfaces only as
	# a shader whose behaviour no longer matches its GLSL.
	file(GLOB_RECURSE _ge_webeditor_shader_sources CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Engine/Modules/*/Shaders/*.vert
		${CMAKE_SOURCE_DIR}/Engine/Modules/*/Shaders/*.frag
		${CMAKE_SOURCE_DIR}/Engine/Modules/*/Shaders/*.comp
		${CMAKE_SOURCE_DIR}/Engine/Modules/*/Shaders/*.glsl)
	if(GE_WEB_SHADERPKG_DIR AND GE_PYTHON3)
		add_custom_command(OUTPUT ${_ge_webeditor_cook_stamp}
			COMMAND ${GE_PYTHON3} ${CMAKE_SOURCE_DIR}/Tools/Web/builtin_shader_cook.py
				--editor
				--shaderpkg-dir ${GE_WEB_SHADERPKG_DIR}
				--out ${GE_WEB_EDITOR_SHADER_DIR}
			COMMAND ${CMAKE_COMMAND} -E touch ${_ge_webeditor_cook_stamp}
			DEPENDS
				${CMAKE_SOURCE_DIR}/Tools/Web/builtin_shader_cook.py
				${CMAKE_SOURCE_DIR}/Tools/ShaderCook/shadercook.py
				# The desktop packages this cook transcodes. Without them the
				# stamp outlives its inputs: regenerate a package with new
				# reflection and the web build keeps shipping the old cook.
				${_ge_webeditor_desktop_shaderpkgs}
				${_ge_webeditor_shader_sources}
			COMMENT "Cooking the web editor's built-in shader set to WGSL"
			VERBATIM)
	else()
		if(NOT GE_PYTHON3)
			set(_ge_webeditor_cook_reason "no python3 on PATH")
		else()
			set(_ge_webeditor_cook_reason "GE_WEB_SHADERPKG_DIR is not set")
		endif()
		add_custom_command(OUTPUT ${_ge_webeditor_cook_stamp}
			COMMAND ${CMAKE_COMMAND} -E echo
				"WebEditor cannot cook its shaders: ${_ge_webeditor_cook_reason}. Build the desktop Shaders/ set once (cmake --build build/macos-arm64-ninja --target CompileShaderPkgs) and reconfigure this build with -DGE_WEB_SHADERPKG_DIR=<that build>/Shaders."
			COMMAND ${CMAKE_COMMAND} -E false
			VERBATIM)
	endif()
	add_custom_target(WebEditorShaders DEPENDS ${_ge_webeditor_cook_stamp})
	# The cook's output is a link input (file_packager runs at link time), so
	# the link both orders after it and re-runs when it changes.
	add_dependencies(WebEditor WebEditorShaders)
	set_property(TARGET WebEditor APPEND PROPERTY LINK_DEPENDS ${_ge_webeditor_cook_stamp})

	# Cooked material variants. This runtime ships no shader compiler, so a
	# material draws only what MaterialVariantCook --web already put in the
	# content-addressed cache the runtime probes at <workspace>/.Cache/Shaders
	# — the same cook and cache layout the Player's export_web_player.py ships.
	# Without it the editor's default scene has gizmos, UI and sky but no
	# meshes: the built-in default PBR misses on every variant. The cook is a
	# native tool, so like GE_WEB_SHADERPKG_DIR it comes from a desktop build
	# and is checked here, where the message can state the fix.
	set(GE_WEB_MATERIAL_COOK "" CACHE FILEPATH
		"Desktop build's MaterialVariantCook, cooks the web editor's material variants")
	if(GE_WEB_MATERIAL_COOK)
		if(NOT EXISTS "${GE_WEB_MATERIAL_COOK}" OR IS_DIRECTORY "${GE_WEB_MATERIAL_COOK}")
			message(FATAL_ERROR
				"GE_WEB_MATERIAL_COOK is '${GE_WEB_MATERIAL_COOK}', which is not a file. "
				"Build the tool in a desktop build of this tree (cmake --build <desktop build> "
				"--target MaterialVariantCook, with -DBUILD_TOOLS=ON) and reconfigure with "
				"-DGE_WEB_MATERIAL_COOK=<that build>/bin/<config>/Tools/MaterialVariantCook.")
		endif()
		cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${GE_WEB_MATERIAL_COOK}" NORMALIZE
			_ge_webeditor_material_cook_in_tree)
		if(NOT _ge_webeditor_material_cook_in_tree)
			message(WARNING
				"GE_WEB_MATERIAL_COOK '${GE_WEB_MATERIAL_COOK}' is outside this source tree "
				"(${CMAKE_SOURCE_DIR}): the variants will be cooked by a tool built from another "
				"checkout, which can address different cache keys than the runtime being linked. "
				"Point it at a desktop build of this tree unless the skew is intended.")
		endif()
	endif()
	set(GE_WEB_EDITOR_MATERIAL_CACHE ${CMAKE_BINARY_DIR}/WebEditorMaterialCache)
	set(_ge_webeditor_material_stamp ${CMAKE_BINARY_DIR}/WebEditorMaterialCache.stamp)
	file(GLOB_RECURSE _ge_webeditor_seed_materials CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/Assets/*.material)
	file(GLOB_RECURSE _ge_webeditor_ddgi_materials CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Tools/Web/ddgi-sponza-project/Assets/*.material)
	file(GLOB_RECURSE _ge_webeditor_eztree_materials CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/*.material)
	file(GLOB_RECURSE _ge_webeditor_eztree_shaders CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders/*)
	if(GE_WEB_MATERIAL_COOK AND GE_PYTHON3)
		add_custom_command(OUTPUT ${_ge_webeditor_material_stamp}
			# Rebuilt from nothing: the cache is content-addressed, so stale
			# siblings never collide — they ship as dead weight in the preload.
			COMMAND ${CMAKE_COMMAND} -E rm -rf ${GE_WEB_EDITOR_MATERIAL_CACHE}
			COMMAND ${GE_WEB_MATERIAL_COOK}
				--project ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project
				--scan ${CMAKE_SOURCE_DIR}/Tools/Web/ddgi-sponza-project/Assets
				--scan-alias ddgi-sponza-project
				--engine-shaders ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders
				# Package shader roots: engine-module surfaces that live outside the
				# Rendering tree. Rendering/Shaders/Surfaces/gpu_fog_particles.glsl is a
				# one-line forward to this root, and without it the include resolves to
				# nothing — the composed fragment then has no EvaluateSurface and the
				# particle material cannot cook at all.
				--package-shaders ${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders
				# CBT terrain owns its surface + vertex modifier (CBT/cbt_surface.glsl,
				# CBT/cbt_vertex_modifier.glsl) and the includes they pull by bare name.
				--package-shaders ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders
				# Grass owns its surface + vertex modifier the same way
				# (TerrainGrass/terrain_grass_surface.glsl and its vertex modifier).
				--package-shaders ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders
				# Tree Generator runtime materials attach Mask leaves + the wind
				# vertex modifier. Scanning the package .material files is not
				# enough on its own — intern keys the composed source, and those
				# files used to omit both. The cook also synthesizes the same
				# documents extraction registers (see CollectTargets).
				--scan ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets
				--package-shaders ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders
				--cache ${GE_WEB_EDITOR_MATERIAL_CACHE}
				--web --shadercook ${CMAKE_SOURCE_DIR}/Tools/ShaderCook/shadercook.py
				--verify
			COMMAND ${CMAKE_COMMAND} -E touch ${_ge_webeditor_material_stamp}
			DEPENDS
				${GE_WEB_MATERIAL_COOK}
				${CMAKE_SOURCE_DIR}/Tools/ShaderCook/shadercook.py
				${_ge_webeditor_shader_sources}
				${_ge_webeditor_seed_materials} ${_ge_webeditor_ddgi_materials}
				${_ge_webeditor_eztree_materials}
				${_ge_webeditor_eztree_shaders}
			COMMENT "Cooking the web editor's material variants (seed project + built-in default PBR)"
			VERBATIM)
	else()
		if(NOT GE_PYTHON3)
			set(_ge_webeditor_material_reason "no python3 on PATH")
		else()
			set(_ge_webeditor_material_reason "GE_WEB_MATERIAL_COOK is not set")
		endif()
		add_custom_command(OUTPUT ${_ge_webeditor_material_stamp}
			COMMAND ${CMAKE_COMMAND} -E echo
				"WebEditor cannot cook its material variants: ${_ge_webeditor_material_reason}. Without them the editor draws no meshes. Build MaterialVariantCook in a desktop build of this tree (-DBUILD_TOOLS=ON) and reconfigure this build with -DGE_WEB_MATERIAL_COOK=<that build>/bin/<config>/Tools/MaterialVariantCook."
			COMMAND ${CMAKE_COMMAND} -E false
			VERBATIM)
	endif()
	add_custom_target(WebEditorMaterialVariants DEPENDS ${_ge_webeditor_material_stamp})
	add_dependencies(WebEditor WebEditorMaterialVariants)
	set_property(TARGET WebEditor APPEND PROPERTY LINK_DEPENDS ${_ge_webeditor_material_stamp})

	# Web pack uses 512 albedo. The 1024 originals and the extra PBR maps stay
	# in the repo for desktop; they are too large to ship three times under
	# Cloudflare's 25 MiB gzip cap. Same MEMFS filenames, so lookup is unchanged.
	set(GE_WEB_TREE_ALBEDO_DIR ${CMAKE_BINARY_DIR}/WebEditorTreeAlbedo512)
	set(_ge_tree_albedo_stamp ${GE_WEB_TREE_ALBEDO_DIR}/.stamp)
	file(GLOB _ge_tree_albedo_src
		${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/leaves/*_color.png
		${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/bark/*_color_1k.jpg)
	if(GE_PYTHON3)
		add_custom_command(OUTPUT ${_ge_tree_albedo_stamp}
			COMMAND ${GE_PYTHON3} ${CMAKE_SOURCE_DIR}/Tools/Web/resize_tree_albedo.py
				--out ${GE_WEB_TREE_ALBEDO_DIR}
			COMMAND ${CMAKE_COMMAND} -E touch ${_ge_tree_albedo_stamp}
			DEPENDS
				${CMAKE_SOURCE_DIR}/Tools/Web/resize_tree_albedo.py
				${_ge_tree_albedo_src}
			COMMENT "Resizing Tree Generator albedo to 512 for the web pack"
			VERBATIM)
	else()
		add_custom_command(OUTPUT ${_ge_tree_albedo_stamp}
			COMMAND ${CMAKE_COMMAND} -E echo
				"WebEditor cannot resize Tree Generator albedo: no python3 on PATH."
			COMMAND ${CMAKE_COMMAND} -E false
			VERBATIM)
	endif()
	add_custom_target(WebEditorTreeAlbedo512 DEPENDS ${_ge_tree_albedo_stamp})
	add_dependencies(WebEditor WebEditorTreeAlbedo512)
	set_property(TARGET WebEditor APPEND PROPERTY LINK_DEPENDS ${_ge_tree_albedo_stamp})

	# Editor chrome (UI xml/css, icons, themes, default rendergraphs), the
	# cooked WGSL set, and the engine shader sources the material pipeline
	# composes against, preloaded into MEMFS at the same next-to-exe layout
	# StageEditorAssets produces on desktop. These are the application's own
	# resources, not project content: unlike the Player — whose dist is fetched
	# at runtime so one binary serves any project — an editor without its
	# chrome is not an editor.
	set(_ge_webeditor_preload
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/Assets@/Assets"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles@/Assets/SkeletonProfiles"
		# The UI faces ship cut down here, and only here: full Roboto is ten
		# faces of ~2770 codepoints each, 1.9 MB of the compressed payload,
		# where the editor's chrome draws ASCII plus about sixty symbols. The
		# subsets carry the same filenames and mount at the same path, so the
		# font resolver is none the wiser. Regenerate with
		# Tools/Web/subset-web-fonts.py. Desktop keeps the full faces.
		"SHELL:--exclude-file ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/*"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/WebFonts@/Assets/Fonts"
		# The engine's render pipeline assets. Without a project the orchestrator
		# resolves RenderPipelines/ForwardPlus.rendergraph by default, and when
		# that lookup fails nothing is drawn.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Assets/RenderPipelines@/Assets/RenderPipelines"
		"SHELL:--preload-file ${GE_WEB_EDITOR_SHADER_DIR}@/Assets/Shaders"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Adapters@/Assets/Shaders/Adapters"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes@/Assets/Shaders/Includes"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Surfaces@/Assets/Shaders/Surfaces"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Particles@/Assets/Shaders/Particles"
		# Shader-graph node library: reflection for the editor palette and the
		# helper includes a materialized graph surface pulls in.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Graph/Nodes@/Assets/Shaders/Graph/Nodes"
		# Ocean surface + vertex-modifier GLSL: the runtime composes them to
		# derive the cooked-variant cache key (same staging desktop does via the
		# Editor's ocean-shader stage step). The FFT/sim .comp kernels in the same
		# directory ride along but stay inert - their runtime compile declines on web.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Ocean@/Assets/Shaders/Ocean"
		# CBT terrain's surface + vertex-modifier GLSL and their includes, at the same
		# Assets/Shaders/CBT the desktop StageEditorAssets step produces: the runtime
		# composes them to derive the cooked-variant cache key. The cooked WGSL kernels
		# land in this same directory from the shader cook above (which writes them under
		# CBT/), so the loader's probe finds one directory holding both halves. The .comp
		# sources ride along inert — this runtime compiles nothing.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT@/Assets/Shaders/CBT"
		# Terrain grass's authored surface, same contract as CBT above: the
		# runtime composes it for the cooked-variant cache key, and an authored
		# surface never falls back to the default surface — absent staging is a
		# compose error, not a cosmetic downgrade.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass@/Assets/Shaders/TerrainGrass"
		# GPU fog / ocean-spray surfaces. Desktop StageEditorAssets copies these
		# to Shaders/GPUFogParticles/Surfaces so intern hashes the same file the
		# cook keyed through --package-shaders GPUFogParticles/Shaders. Without
		# this mount the web adapter tree only has the one-line Surfaces/
		# forwarder, intern misses every cooked smoke-particles row, and Shader
		# Errors reads "no cooked program".
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders/Surfaces@/Assets/Shaders/GPUFogParticles/Surfaces"
		# Tree Generator surfaces + wind modifier. Cook keys them through
		# --package-shaders Packages/eztree/Assets/Shaders. Staging the
		# same tree under Shaders/EZTree lets AppendStagedModuleShaderDirs
		# intern those rows when the package mount is not yet in the source
		# table (the grass/fog contract). The composed source still
		# `#include "Surfaces/ez_tree_leaves.glsl"` — intern's include-closure
		# scan looks that up against AdapterShaderDir/Surfaces, not EZTree/.
		# Without these two files at the adapter-relative paths, the scan
		# skips the leaf/wind operands, intern hashes a different key than
		# the cook, and Shader Errors reads "no cooked program".
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders@/Assets/Shaders/EZTree"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders/Surfaces/ez_tree_leaves.glsl@/Assets/Shaders/Surfaces/ez_tree_leaves.glsl"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders/VertexModifiers/ez_tree_wind.glsl@/Assets/Shaders/VertexModifiers/ez_tree_wind.glsl"
		# First-party post-effect packages, at the next-to-exe root the
		# PackageResolver probes implicitly (<exe dir>/Packages). Ship
		# exactly the packages whose stages builtin_shader_cook.py cooks for
		# web — a mounted package whose shaders cannot load is a degraded pass
		# at best.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/chromatic-aberration@/Packages/chromatic-aberration"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/film-simulation@/Packages/film-simulation"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/fidelityfx-dof@/Packages/fidelityfx-dof"
		# Tree Generator package mount (alias eztree). Pack package.json + Assets
		# only — Native/ would invite a cmake source-build of the MODULE the
		# wasm host cannot dlopen. The generator itself is compiled into
		# WebEditor above.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/eztree/package.json@/Packages/eztree/package.json"
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets@/Packages/eztree/Assets"
		# Drop the authored 1024 texture folders from the Assets tree, then
		# mount 512 albedo at the same dest names. Extra PBR maps stay out.
		"SHELL:--exclude-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/leaves/*"
		"SHELL:--exclude-file ${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/bark/*"
		"SHELL:--exclude-file *_normal_1k.jpg"
		"SHELL:--exclude-file *_ao_1k.jpg"
		"SHELL:--exclude-file *_roughness_1k.jpg"
		# Matches the editor example materials' normal maps
		# (Assets/Examples/Materials/OpenPBR, .../Water) and nothing else in the
		# packaged trees; they stay out of the payload.
		"SHELL:--exclude-file *_normal.png"
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/leaves@/Packages/eztree/Assets/Textures/EZTree/leaves"
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/bark@/Packages/eztree/Assets/Textures/EZTree/bark"
		# Stored-identity paths are lowercase (`textures/eztree/leaves/...`).
		# MEMFS is case-sensitive, so alias albedo bytes at that spelling.
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/leaves@/Packages/eztree/Assets/textures/eztree/leaves"
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/bark@/Packages/eztree/Assets/textures/eztree/bark"
		# Editor-mount albedo fallback when the package source is not mounted yet.
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/leaves@/Assets/Textures/EZTree/leaves"
		"SHELL:--preload-file ${GE_WEB_TREE_ALBEDO_DIR}/bark@/Assets/Textures/EZTree/bark"
		# LoadPreset is exe-anchored under Assets/EZTree/Presets (same path
		# StageEditorAssets writes on desktop).
		"SHELL:--preload-file ${GE_EZTREE_PRESETS_SOURCE_DIR}@/Assets/EZTree/Presets"
		# The cooked variant cache, at the root the no-project fallback
		# workspace probes (<workspace>/.Cache/Shaders with workspace = "/").
		"SHELL:--preload-file ${GE_WEB_EDITOR_MATERIAL_CACHE}@/.Cache/Shaders"
		# A project to open on a browser that has never run this editor. Unlike
		# the chrome above this is not a mount: WebMain copies it into the OPFS
		# projects tree once, and the copy is what the user edits. EngineCore
		# reads the bundled shader cache directly for every project; copying
		# it into OPFS would add unused persistent files and startup writes.
		"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project@/SeedProject/WebSmoke"
		# A desktop editor run on the seed project leaves desktop-compiled
		# programs in its .Cache; they are dead weight here and push the
		# preload over the host's 25 MiB static-asset limit.
		"SHELL:--exclude-file ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/.Cache/*"
		# The same run also writes the seed's GUID store; the web editor builds
		# its own on first open, so a desktop-written one must not ship.
		"SHELL:--exclude-file ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/AssetDatabase.assetdb"
		# Built-in project templates. installTemplatesRoot resolves to
		# <exe>/../ProjectTemplates == /ProjectTemplates on web (exe dir is "/"),
		# so the picker reads manifest.json and Basic3D/thumbnail.png from here;
		# without this the template cards fall back to the placeholder. ~164 KB.
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/ProjectTemplates@/ProjectTemplates"
		# The editor's sample videos are dead weight on this target: the web
		# build has the Null video backend, and logo.mov is referenced by
		# nothing. A third of the data payload for files that cannot play.
		"SHELL:--exclude-file *Assets/Sample/*")
	# DDGI Sponza parity project: a ~100 MB glTF seed that pushes WebEditor.data
	# well past the deploy site's 25 MiB static-asset cap. Off for shipping builds;
	# enable for local parity work with -DGE_WEB_PARITY_SEEDS=ON.
	option(GE_WEB_PARITY_SEEDS
		"Preload the DDGISponza parity project into the web data payload (local only; exceeds the deploy size cap)"
		OFF)
	if(GE_WEB_PARITY_SEEDS)
		list(APPEND _ge_webeditor_preload
			"SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Tools/Web/ddgi-sponza-project@/SeedProject/DDGISponza"
			"SHELL:--exclude-file ${CMAKE_SOURCE_DIR}/Tools/Web/ddgi-sponza-project/.Cache/*")
	endif()
	# Preloaded content is baked into WebEditor.data at LINK time, and a link
	# depends on objects, not on the files the packager reads. Edit an icon, a
	# stylesheet or a font and ninja sees nothing to do: the build succeeds and
	# ships the previous package. Listing the packaged trees as link
	# dependencies is what makes an asset edit relink; CONFIGURE_DEPENDS
	# re-globs when files are added or removed.
	file(GLOB_RECURSE _ge_webeditor_preload_files CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/Assets/*"
		"${CMAKE_CURRENT_SOURCE_DIR}/WebFonts/*"
		"${CMAKE_SOURCE_DIR}/Assets/RenderPipelines/*"
		"${CMAKE_SOURCE_DIR}/Packages/eztree/package.json"
		"${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/*"
		"${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/leaves/*"
		"${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/bark/*"
		"${GE_EZTREE_PRESETS_SOURCE_DIR}/*")
	set_property(TARGET WebEditor APPEND PROPERTY
		LINK_DEPENDS "${_ge_webeditor_preload_files}")
	# The cooked shader tree is generated, so the glob above cannot see it. Its
	# stamp stands in: recook, relink, repackage.
	set_property(TARGET WebEditor APPEND PROPERTY
		LINK_DEPENDS "${_ge_webeditor_cook_stamp}")

	target_link_options(WebEditor PRIVATE
		${_ge_webeditor_env}
		${_ge_webeditor_preload}
		# Function names in the wasm name section: stacks from the field are
		# $funcNNNNN without this, and a hang report becomes actionable only
		# when the frame names are readable. Costs binary size, no runtime.
		"SHELL:--profiling-funcs"
		# WASMFS + the forced JS library symbols: the editor keeps its projects
		# on OPFS.
		${GE_WASM_OPFS_LINK_OPTIONS}
		# ASYNCIFY for the renderer, not for storage: WebGPU device bring-up
		# pumps browser promises from a stack that has to unwind, and without it
		# the editor aborts on emscripten_sleep before its first frame.
		# Classic asyncify, NOT JSPI (ASYNCIFY=2): JSPI cannot suspend inside
		# WasmFS's OPFS proxying on this emscripten pin (SuspendError at
		# OPFSBackend::createDirectory). The asyncify<->mailbox rewind race is
		# instead closed by the checkMailbox defer patch the build applies to
		# WebEditor.js after link (see the patch step below).
		"SHELL:-s ASYNCIFY"
		"SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
		# The headless gate (Tools/Web/browser_gate.py --eval) reads MEMFS dumps
		# such as /.Cache/Shaders/Failed/* through Module.FS.
		"SHELL:-s EXPORTED_RUNTIME_METHODS=FS"
		"SHELL:-s INITIAL_MEMORY=512MB"
		"SHELL:--shell-file ${GE_WEB_SHELL}")
	# The checkMailbox defer patch (see the ASYNCIFY note above): classic
	# asyncify's rewind window must not run the pthread mailbox pump.
	# A patch revision must regenerate the linked JS before applying it.
	set_property(TARGET WebEditor APPEND PROPERTY LINK_DEPENDS
		${CMAKE_SOURCE_DIR}/Tools/Web/patch_check_mailbox.py)
	add_custom_command(TARGET WebEditor POST_BUILD
		COMMAND ${GE_PYTHON3} ${CMAKE_SOURCE_DIR}/Tools/Web/patch_check_mailbox.py
			$<TARGET_FILE_DIR:WebEditor>/WebEditor.js
		COMMENT "Patching checkMailbox against the asyncify rewind race")
	if(NOT GE_WASM_SINGLE_THREAD)
		# Match the root wasm pool. Each worker maps the whole wasm
		# SharedArrayBuffer, so the pool stays small and background work
		# shares the JobSystem rather than spawning more pthreads.
		target_link_options(WebEditor PRIVATE "SHELL:-s PTHREAD_POOL_SIZE=8")
	endif()
	return()
endif()

# Platform-specific application icon resources
set(EDITOR_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Icon")

if(WIN32)
	# Windows: embed .ico via resource file for executable icon
	set(EDITOR_RESOURCES ${EDITOR_ICON_DIR}/Editor.rc)
elseif(APPLE)
	# macOS: bundle .icns file for app icon
	set(EDITOR_MACOS_ICON ${EDITOR_ICON_DIR}/AppIcon.icns)
	set_source_files_properties(${EDITOR_MACOS_ICON} PROPERTIES
		MACOSX_PACKAGE_LOCATION Resources
	)
	set(EDITOR_RESOURCES ${EDITOR_MACOS_ICON})
else()
	# Linux: no embedded icon support, runtime loading handled in code
	set(EDITOR_RESOURCES "")
endif()

# ---------------------------------------------------------------------------
# EditorSDK — the editor extension surface native Editor-kind package modules
# link against (EditorSDK phase 1). A real SHARED library rather than
# Editor.exe exports so (a) the registries are one process-wide instance
# shared by Editor.exe and every plugin DLL, (b) headless tests can host
# editor modules without a running editor (imports bind to EditorSDK.dll, not
# to an executable's name), and (c) this source list IS the curated ABI
# surface. ge_stage_engine_sdk stages its import lib + the editor headers;
# modules link it exactly like the Engine import lib, gated by the same
# toolchain-fingerprint and ABI-digest checks.
#
# Only Engine (SHARED) and header-only INTERFACE libs may be linked here —
# never engine OBJECT modules (that would clone their registry statics into
# this DLL; see the GE_ENGINE_MODULES PRIVATE-link note in Engine/CMakeLists).
# ---------------------------------------------------------------------------
add_library(EditorSDK SHARED ${EDITOR_SDK_SOURCES})
# The VCS contract (IVCSIntegration + VCSCommandExecutor) is part of the
# EditorSDK surface: provider modules — built-in or package — implement it,
# and the provider registry dispatches through it. Spliced as objects (not a
# target link) so the OBJECT lib's engine-module link interface is not
# replayed here (see the note above about registry statics).
target_sources(EditorSDK PRIVATE $<TARGET_OBJECTS:VCSIntegration>)
ge_add_production_allocation_hook(EditorSDK)
target_include_directories(EditorSDK PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/Include
	${CMAKE_SOURCE_DIR}/Engine/Modules/VCSIntegration/Include
)
target_link_libraries(EditorSDK PUBLIC Engine Types Mathematics Events)
set_target_properties(EditorSDK PROPERTIES
	WINDOWS_EXPORT_ALL_SYMBOLS ON
	FOLDER "Apps/Editor"
)
if(MSVC)
	target_compile_options(EditorSDK PRIVATE /wd4100)
endif()

# ---------------------------------------------------------------------------
# Prebuilt engine-package native modules (Packages.cmake): the engine
# build compiles every Packages/*/ module and stages the DLLs beside the
# package in the P3 prebuilt layout, so the first project open loads them
# fingerprint-checked instead of paying a cmake+MSVC source compile per
# package. Names follow PackageAssemblyName (+".Editor" for Editor-kind).
# eztree's two modules get separate prebuilt subdirs — the engine_abi marker
# is per platform dir, and the runtime/editor digests differ by construction.
# ---------------------------------------------------------------------------
ge_add_engine_package_module(GePkgModule_Eztree
	PACKAGE eztree
	MODULE_NAME Eztree
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/eztree/Native
	KIND runtime
	PREBUILT_SUBDIR Binaries/Runtime)
ge_add_engine_package_module(GePkgModule_EztreeEditor
	PACKAGE eztree
	MODULE_NAME Eztree.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/eztree/Editor
	KIND editor
	PREBUILT_SUBDIR Binaries/Editor
	# The editor half extends its runtime half: same peer-header rule the
	# editor's package-module wiring applies to the source build.
	EXTRA_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/Packages/eztree/Native)
ge_add_engine_package_module(GePkgModule_AiAssistantEditor
	PACKAGE ai-assistant
	MODULE_NAME AiAssistant.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor
	KIND editor
	PREBUILT_SUBDIR Binaries/Editor)
ge_add_engine_package_module(GePkgModule_GitVcsEditor
	PACKAGE git-vcs
	MODULE_NAME GitVcs.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/git-vcs/Editor
	KIND editor)
ge_add_engine_package_module(GePkgModule_SvnVcsEditor
	PACKAGE svn-vcs
	MODULE_NAME SvnVcs.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/svn-vcs/Editor
	KIND editor)
ge_add_engine_package_module(GePkgModule_DiversionVcsEditor
	PACKAGE diversion-vcs
	MODULE_NAME DiversionVcs.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/diversion-vcs/Editor
	KIND editor)
ge_add_engine_package_module(GePkgModule_LoreVcsEditor
	PACKAGE lore-vcs
	MODULE_NAME LoreVcs.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/lore-vcs/Editor
	KIND editor)
ge_add_engine_package_module(GePkgModule_UnityImportEditor
	PACKAGE unity-import
	MODULE_NAME UnityImport.Editor
	SOURCE_ROOT ${CMAKE_SOURCE_DIR}/Packages/unity-import/Editor
	KIND editor)

# Create the Editor executable.
#
# - On macOS, build as a bundle so CMake generates an Info.plist and embeds
#   the AppIcon.icns into the .app bundle.
# - On Windows and other platforms, build as a console executable; the
#   Windows .rc file in EDITOR_RESOURCES still provides the executable icon.
if(APPLE)
	add_executable(Editor MACOSX_BUNDLE ${EDITOR_SOURCES} ${EDITOR_HEADERS} ${EDITOR_RESOURCES})
else()
	add_executable(Editor ${EDITOR_SOURCES} ${EDITOR_HEADERS} ${EDITOR_RESOURCES})
endif()
add_dependencies(Editor EditorBuildCommit)
target_include_directories(Editor PRIVATE "${GE_EDITOR_BUILD_COMMIT_DIR}")
ge_add_production_allocation_hook(Editor)

# Keep the Linux native executable behind a launcher so packaged builds use
# the stable descriptor-pool path unless a user explicitly opts into Vulkan
# descriptor buffers.
if(UNIX AND NOT APPLE)
	set_target_properties(Editor PROPERTIES OUTPUT_NAME "Editor.bin")
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/launch-linux-editor.sh"
		"${CMAKE_CURRENT_BINARY_DIR}/Editor"
		COPYONLY)
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${CMAKE_CURRENT_BINARY_DIR}/Editor"
			"$<TARGET_FILE_DIR:Editor>/Editor"
		COMMAND chmod +x "$<TARGET_FILE_DIR:Editor>/Editor"
		COMMENT "Staging Linux Editor launcher"
		VERBATIM)
endif()

# GE_BUILD_CONFIG: this build's own configuration name ("Debug", "DebugFast", "Release", ...)
# as a string literal for the footer build tag, from the generator's per-config $<CONFIG>.
# That is the only source of the name: _DEBUG/NDEBUG classify CRT/assert mode, they cannot
# name a config (Release, RelWithDebInfo and MinSizeRel all define NDEBUG; DebugFast defines
# neither macro). Scoped to the Editor target, the sole consumer.
target_compile_definitions(Editor PRIVATE GE_BUILD_CONFIG="$<CONFIG>")

find_path(RENDERDOC_APP_API_INCLUDE_DIR NAMES renderdoc_app.h REQUIRED)

# NVIDIA Nsight Graphics SDK — header-only, and distributed only inside the
# Nsight installer, so there is no vcpkg port and no pinnable URL to fetch. It is
# therefore discovered on the machine or skipped: found enables nsight_capture,
# absent leaves the handler reporting unavailable and changes nothing else.
# Set NSIGHT_GRAPHICS_SDK to a version directory or its include/ to override.
set(NGFX_SDK_HINTS "")
if(DEFINED ENV{NSIGHT_GRAPHICS_SDK})
	list(APPEND NGFX_SDK_HINTS "$ENV{NSIGHT_GRAPHICS_SDK}")
endif()
# find_path does not expand globs in PATHS, so enumerate the installed versions
# and offer them newest-first. NATURAL sorting keeps 2026.10 above 2026.3.
file(GLOB NGFX_SDK_CANDIDATES
	"C:/Program Files/NVIDIA Corporation/Nsight Graphics */SDKs/NsightGraphicsSDK/*/include")
list(SORT NGFX_SDK_CANDIDATES COMPARE NATURAL)
list(REVERSE NGFX_SDK_CANDIDATES)
list(APPEND NGFX_SDK_HINTS ${NGFX_SDK_CANDIDATES})

find_path(NGFX_SDK_INCLUDE_DIR
	NAMES NGFX_GraphicsCapture_Vulkan.h
	HINTS ${NGFX_SDK_HINTS}
	PATH_SUFFIXES "" "include"
	NO_DEFAULT_PATH)

if(NGFX_SDK_INCLUDE_DIR)
	target_compile_definitions(Editor PRIVATE GE_HAVE_NGFX_SDK=1)
	target_include_directories(Editor PRIVATE ${NGFX_SDK_INCLUDE_DIR})
	message(STATUS "Nsight Graphics SDK: ${NGFX_SDK_INCLUDE_DIR} (in-app capture enabled)")
else()
	message(STATUS "Nsight Graphics SDK: not found (nsight_capture will report unavailable)")
endif()

target_include_directories(Editor PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Include
	${CMAKE_SOURCE_DIR}/Engine/Modules/Audio/Include
	${RENDERDOC_APP_API_INCLUDE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/Source
	${CMAKE_CURRENT_SOURCE_DIR}/Include
	${CMAKE_CURRENT_SOURCE_DIR}/Modules
	${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz
)

# Third-party C sources; compiled quietly like the engine's other vendored libraries.
target_sources(Editor PRIVATE ${EDITOR_MINIZ_SOURCES})
if(MSVC)
	set_source_files_properties(${EDITOR_MINIZ_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
endif()

# Engine.dll bundles engine modules as OBJECT files spliced in at SHARED-link time,
# so Editor.exe must not link those modules directly. Doing so can duplicate
# singletons and, on macOS, can pull static GLFW into both Editor and libEngine.dylib.
target_link_libraries(Editor PRIVATE Engine EditorSDK)

# The SQLite asset-cache implementation lives in a separate static library so
# Engine.dll (and the standalone Player) link no SQL engine. The editor opts
# back into the cache by linking it and injecting the factory at startup via
# AssetRegistry::SetAssetDbCacheFactory.
target_link_libraries(Editor PRIVATE AssetDatabaseSqlite)

# These modules are editor-only, so their object files belong in Editor. Add the
# objects without linking the OBJECT-library targets so their transitive engine
# module link interfaces do not get replayed on the Editor executable.
#
# Graph moved out of this list when Engine.dll started using it internally
# (MaterialCompiler / CoreAssetRegistrations); it now lives in
# GE_ENGINE_MODULES so Editor.exe reaches it via Engine.dll's import lib.
# VCSIntegration (the contract) moved into EditorSDK.dll; every VCS provider
# (git, svn, diversion, lore) lives in an Editor-kind package module under
# Packages/ — the editor core carries zero provider code and packaged
# games ship no VCS code.
set(_GE_EDITOR_OBJECT_MODULES
	MeshPicking
)
foreach(_ge_editor_object_module IN LISTS _GE_EDITOR_OBJECT_MODULES)
	if(TARGET ${_ge_editor_object_module})
		target_sources(Editor PRIVATE $<TARGET_OBJECTS:${_ge_editor_object_module}>)
		target_include_directories(Editor PRIVATE
			$<TARGET_PROPERTY:${_ge_editor_object_module},INTERFACE_INCLUDE_DIRECTORIES>
		)
		target_compile_definitions(Editor PRIVATE
			$<TARGET_PROPERTY:${_ge_editor_object_module},INTERFACE_COMPILE_DEFINITIONS>
		)
	endif()
endforeach()

if(MSVC)
	target_compile_options(Editor PRIVATE /bigobj /wd4100)
endif()

# CoreGraphics for native shift-key detection at startup (before GLFW poll)
if(APPLE)
    target_link_libraries(Editor PRIVATE "-framework CoreGraphics")
endif()

# Winsock2 for EditorDebugServer (MCP debug server)
if(WIN32)
    target_link_libraries(Editor PRIVATE ws2_32 $<$<CONFIG:Debug>:dbghelp>)
endif()

# stb_image for reading image dimensions in asset browser
if(TARGET stb::stb)
    target_link_libraries(Editor PRIVATE stb::stb)
    target_compile_definitions(Editor PRIVATE GE_HAVE_STB)
elseif(STB_INCLUDE_DIRS)
    target_include_directories(Editor PRIVATE ${STB_INCLUDE_DIRS})
    target_compile_definitions(Editor PRIVATE GE_HAVE_STB)
endif()

if(ENABLE_MIMALLOC)
	# mimalloc custom memory allocator
	find_package(mimalloc CONFIG REQUIRED)

		# We only enable mimalloc when the *shared library* is available; otherwise we
		# fall back to the system allocator (regular new/delete) to avoid cross-module
		# heap mismatches (static mimalloc would create one allocator instance per module).
		if(TARGET mimalloc)
			set(_GE_MIMALLOC_IS_SHARED FALSE)
			get_target_property(_ge_mimalloc_type mimalloc TYPE)
			if(_ge_mimalloc_type STREQUAL "SHARED_LIBRARY" OR _ge_mimalloc_type STREQUAL "MODULE_LIBRARY")
				set(_GE_MIMALLOC_IS_SHARED TRUE)
			else()
				# Some package managers expose an IMPORTED target where TYPE may be UNKNOWN.
				# Fall back to checking the imported library's file extension.
				get_target_property(_ge_mimalloc_is_imported mimalloc IMPORTED)
				if(_ge_mimalloc_is_imported)
					foreach(_ge_prop IN ITEMS
							IMPORTED_LOCATION
							IMPORTED_LOCATION_DEBUG
							IMPORTED_LOCATION_RELEASE
							IMPORTED_LOCATION_RELWITHDEBINFO
							IMPORTED_LOCATION_MINSIZEREL
					)
						get_target_property(_ge_loc mimalloc ${_ge_prop})
						if(_ge_loc AND NOT _ge_loc MATCHES "NOTFOUND")
							get_filename_component(_ge_ext "${_ge_loc}" EXT)
							string(TOLOWER "${_ge_ext}" _ge_ext_lower)
							if(_ge_ext_lower STREQUAL ".dll" OR _ge_ext_lower STREQUAL ".so" OR _ge_ext_lower STREQUAL ".dylib")
								set(_GE_MIMALLOC_IS_SHARED TRUE)
								break()
							endif()
						endif()
					endforeach()
				endif()
			endif()

			if(_GE_MIMALLOC_IS_SHARED)
				# Prefer mimalloc's official override library (no custom operator new/delete TU).
				set(_GE_MIMALLOC_OVERRIDE_LIB "")
				foreach(_candidate IN ITEMS mimalloc-override mimalloc-override-shared)
					if(TARGET ${_candidate})
						set(_GE_MIMALLOC_OVERRIDE_LIB ${_candidate})
						break()
					endif()
				endforeach()

				# Link order matters for some mimalloc override/redirect variants on Windows.
				# Ensure the allocator library is linked before the override shim.
				target_link_libraries(Editor PRIVATE mimalloc)
				if(_GE_MIMALLOC_OVERRIDE_LIB)
					target_link_libraries(Editor PRIVATE ${_GE_MIMALLOC_OVERRIDE_LIB})
				else()
					# Fallback: use our own global operator new/delete overrides.
					target_sources(Editor PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/Memory/MimallocOverrides.cpp)
				endif()
				target_compile_definitions(Editor PRIVATE GE_ENABLE_MIMALLOC=1)
			else()
				message(STATUS "Editor: mimalloc target is not a shared library; disabling mimalloc (falling back to system allocator)")
				target_compile_definitions(Editor PRIVATE GE_ENABLE_MIMALLOC=0)
			endif()
		else()
			message(STATUS "Editor: mimalloc shared target not available; disabling mimalloc (falling back to system allocator)")
			target_compile_definitions(Editor PRIVATE GE_ENABLE_MIMALLOC=0)
		endif()
else()
	target_compile_definitions(Editor PRIVATE GE_ENABLE_MIMALLOC=0)
endif()

# Ensure the Editor build always produces up-to-date SPIR-V before staging/copying.
# Without an explicit dependency, building only the `Editor` target may skip the
# `CompileShaders` target (it is in ALL, but not a dependency of Editor).
if(TARGET CompileShaders)
	add_dependencies(Editor CompileShaders)
endif()
if(TARGET CompileShaderPkgs)
	add_dependencies(Editor CompileShaderPkgs)
endif()

if(TARGET GameEngine.Native)
    target_link_libraries(Editor PRIVATE GameEngine.Native)
endif()

set_target_properties(Editor PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Apps/Editor"
)

# macOS: configure app bundle properties. CMake generates a complete Info.plist
# automatically from these MACOSX_BUNDLE_* properties - no custom template needed.
if(APPLE)
	set_target_properties(Editor PROPERTIES
	    MACOSX_BUNDLE TRUE
	    MACOSX_BUNDLE_BUNDLE_NAME "Open Engine Editor"
	    MACOSX_BUNDLE_ICON_FILE AppIcon.icns
	    MACOSX_BUNDLE_GUI_IDENTIFIER "com.gameengine.editor"
	    MACOSX_BUNDLE_BUNDLE_VERSION "${GE_ENGINE_VERSION_NUMERIC}"
	    MACOSX_BUNDLE_SHORT_VERSION_STRING "${GE_ENGINE_VERSION_NUMERIC}"
	    MACOSX_BUNDLE_INFO_STRING "Open Engine Editor ${GE_ENGINE_VERSION}"
	)

	# Standard macOS bundle runtime search path: load bundled dylibs from Contents/Frameworks.
	# We stage runtime dependencies into Contents/Frameworks via ge_stage_runtime_dependencies.
	# BUILD_WITH_INSTALL_RPATH keeps dev builds (from Finder) consistent with the staged layout.
	set_target_properties(Editor PROPERTIES
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "@executable_path/../Frameworks"
		INSTALL_RPATH_USE_LINK_PATH FALSE
	)

	# Keep Xcode build setting and Info.plist CFBundleIdentifier consistent to avoid warnings.
	# (CMake sets the Info.plist key via MACOSX_BUNDLE_GUI_IDENTIFIER, but Xcode also expects
	# PRODUCT_BUNDLE_IDENTIFIER to be set.)
	set_target_properties(Editor PROPERTIES
	    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.gameengine.editor"
	)

	# macOS signing ordering:
	# Xcode's built-in CodeSign phase can run at a point where subsequent post-build
	# staging steps (runtime deps, managed resources, install_name_tool fixups, etc.)
	# still modify the bundle. Finder/Dock launches are strict about signature validity.
	#
	# We disable Xcode's automatic signing for the Editor target and rely on the
	# explicit final POST_BUILD re-sign step (CodesignMacAppBundle.cmake) so signing
	# is always the very last mutation of the .app bundle.
	set_target_properties(Editor PROPERTIES
		XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
		XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
	)
endif()

# macOS packaging best practice:
# Use CMake's BundleUtilities at *install* time to produce a portable, relocatable
# Editor.app with fixed-up dylib dependencies and verified prerequisites.
# (BundleUtilities recommends using install(CODE)/install(SCRIPT), not configure time.)
if(APPLE)
	option(GE_INSTALL_EDITOR_BUNDLE "Install and fix up Editor.app using BundleUtilities at install time" ON)
	if(GE_INSTALL_EDITOR_BUNDLE)
		set(_ge_editor_install_fixup_in "${CMAKE_SOURCE_DIR}/cmake/InstallFixupEditorBundle.cmake.in")
		set(_ge_editor_install_fixup "${CMAKE_BINARY_DIR}/cmake/InstallFixupEditorBundle.cmake")
		configure_file("${_ge_editor_install_fixup_in}" "${_ge_editor_install_fixup}" @ONLY)

		# Install the built .app bundle to the install prefix (default: <prefix>/Editor.app).
		install(TARGETS Editor BUNDLE DESTINATION ".")

		# Fix up and verify the installed bundle in-place.
		install(SCRIPT "${_ge_editor_install_fixup}")

		# Convenience: a build target that runs the install+fixup step into the build tree.
		# This keeps the best-practice BundleUtilities flow (install-time), while allowing
		# a one-command "build+bundle" workflow:
		#   cmake --build <build> --target EditorBundle --config Debug
		#
		# Output:
		#   <build>/dist/<Config>/Editor.app
		option(GE_ADD_EDITOR_BUNDLE_TARGET "Add EditorBundle target (runs cmake --install to build/dist/<Config>)" ON)
		if(GE_ADD_EDITOR_BUNDLE_TARGET)
			add_custom_target(EditorBundle
				COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
						--config $<CONFIG>
						--prefix "${CMAKE_BINARY_DIR}/dist/$<CONFIG>"
				DEPENDS Editor
				COMMENT "Installing + fixing up Editor.app into ${CMAKE_BINARY_DIR}/dist/$<CONFIG>"
				VERBATIM
			)
			set_target_properties(EditorBundle PROPERTIES FOLDER "Apps/Editor")
		endif()
	endif()
endif()

# Unity builds are enabled by the `*-fast` presets globally. The Editor target
# currently has many file-local helpers (especially in inspectors) that are not
# unity-safe and can collide when compiled into the same translation unit.
# Disable unity builds for the Editor target to keep `vs2022-*-local-fast` usable.
set_target_properties(Editor PROPERTIES UNITY_BUILD OFF)

# Output layout: bin/<Config>/Apps/Editor
ge_set_output_app(Editor "Editor")

# Stage runtime DLL dependencies (e.g. GameEngine.Native.dll, vcpkg DLLs) next to Editor.exe
ge_stage_runtime_dependencies(Editor)
ge_register_application_directory(Editor)

# Prebuilt engine-package modules must exist before Editor's POST_BUILD
# staging overlays them onto <exe dir>/Packages.
add_dependencies(Editor EnginePackagePrebuilts)

# First-party packages (Packages/eztree, ...) staged next to Editor.exe;
# the package resolver's implicit engine-package scan mounts them per project.
ge_stage_packages(Editor)

# On Linux, the staging step above copies libGameEngine.Native.so into the exe
# directory, but the dynamic linker loads it from cmake-build/lib/ via RPATH.
# Those are two different files (different inodes), so when CoreCLR later calls
# dlopen() with the exe-directory path, Linux treats it as a second load and
# runs Jolt's static initializers a second time, tripping the SetEmbedded()
# assert in Reference.h. Replace the staged copy with a hard link so both paths
# resolve to the same canonical file and dlopen() returns the already-loaded
# handle without re-running any initializers. A hard link also remains valid
# when a container-built output directory is launched from the host.
if(UNIX AND NOT APPLE AND TARGET GameEngine.Native)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND ln -f "$<TARGET_FILE:GameEngine.Native>"
                      "$<TARGET_FILE_DIR:Editor>/$<TARGET_FILE_NAME:GameEngine.Native>"
        COMMENT "Hard-linking libGameEngine.Native.so in Editor dir (prevents double dlopen)"
        VERBATIM
    )
endif()

# Directory trees staged next to the Editor are mirrors: copy_directory_if_different,
# then the orphan prune (cmake/PruneStagedMirrorOrphans.cmake). The copy never
# deletes, so without the prune a file deleted or renamed in its source keeps
# being served from the staged tree — and, on macOS, from the user copy seeded
# from it — until someone wipes the build output. The prune removes only files
# the same mirror staged before, so build products and runtime state written
# into the same tree, and the other mirrors' files, are never candidates. Two
# mirrors must not provide the same relative path: each would record it, and
# deleting it from one source would prune the other's copy. Records live under
# the build tree, not in the shipped output.
set(_GE_EDITOR_MIRROR_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/$<CONFIG>/mirror-manifests")
set(_GE_EDITOR_MIRROR_PRUNE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake")
macro(_ge_editor_mirror_commands out_list src dst)
	list(APPEND ${out_list}
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${src}" "${dst}"
		COMMAND ${CMAKE_COMMAND} "-DSRC=${src}" "-DDST=${dst}"
		        "-DMANIFEST_DIR=${_GE_EDITOR_MIRROR_MANIFEST_DIR}"
		        -P "${_GE_EDITOR_MIRROR_PRUNE_SCRIPT}")
endmacro()

# Stage managed assemblies next to Editor.exe (CoreBridge/HotReload/etc).
# These are built in their default `Managed/*/bin/<Config>/net10.0/` locations and
# then copied into the Editor's isolated output folder.
if(ENABLE_SCRIPTING)
	if(APPLE)
		# The central managed build setup calls this after defining its output
		# stamps. Staging is therefore a normal output command instead of an Xcode
		# POST_BUILD script, and managed-only edits no longer force an Editor relink.
		function(ge_add_editor_managed_staging)
			set(_editor_bundle
				"${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Apps/Editor/Editor.app")
			set(_stage_stamp
				"${_editor_bundle}/Contents/Resources/Managed/.stage-managed.stamp")
			add_custom_command(OUTPUT "${_stage_stamp}"
				COMMAND ${CMAKE_COMMAND}
					"-DGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
					"-DGE_MANAGED_DEST=${_editor_bundle}/Contents/Resources/Managed"
					"-DGE_CONFIG=$<CONFIG>"
					-P "${CMAKE_SOURCE_DIR}/cmake/StageEditorManagedAssemblies.cmake"
				COMMAND ${CMAKE_COMMAND} -E touch "${_stage_stamp}"
				DEPENDS ${ARGN}
					"${CMAKE_SOURCE_DIR}/cmake/StageEditorManagedAssemblies.cmake"
				COMMENT "Staging managed assemblies into Editor.app Resources"
				VERBATIM)
			add_custom_target(StageEditorManaged DEPENDS "${_stage_stamp}")
			set_target_properties(StageEditorManaged PROPERTIES FOLDER "Apps/Editor")

			# Resource staging targets can execute in parallel. Gate Editor on one
			# final signature stamp so a managed-, asset-, or shader-only update
			# leaves an existing app launchable. A clean build has no executable
			# yet and is signed by Editor's normal final POST_BUILD command.
			set(_sign_stamp
				"${_editor_bundle}/Contents/Resources/.stage-resources-signed.stamp")
			add_custom_command(OUTPUT "${_sign_stamp}"
				COMMAND ${CMAKE_COMMAND} -E touch "${_sign_stamp}"
				COMMAND ${CMAKE_COMMAND}
					"-DGE_APP_BUNDLE=${_editor_bundle}"
					"-DGE_APP_EXECUTABLE=${_editor_bundle}/Contents/MacOS/Editor"
					-P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
				DEPENDS "${_stage_stamp}"
					"${_editor_bundle}/Contents/Resources/Assets/.stage-assets.stamp"
					"${_editor_bundle}/Contents/Resources/Assets/Shaders/.stage-shaders.stamp"
					"${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
				COMMENT "Signing Editor.app after stamped resource staging"
				VERBATIM)
			add_custom_target(StageEditorBundleResources DEPENDS "${_sign_stamp}")
			add_dependencies(StageEditorBundleResources
				StageEditorManaged StageEditorAssets StageEditorShaders)
			set_target_properties(StageEditorBundleResources PROPERTIES FOLDER "Apps/Editor")
			add_dependencies(Editor StageEditorBundleResources)
		endfunction()
	else()
		# Non-mac platforms: also stage CompileServerHost next to Editor.exe so the
		# native client can discover it relative to the working directory.
		set(_ge_compile_server_host_mirror "")
		_ge_editor_mirror_commands(_ge_compile_server_host_mirror
			"${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/bin/$<CONFIG>/net10.0"
			"$<TARGET_FILE_DIR:Editor>/Managed/CompileServerHost/bin/$<CONFIG>/net10.0")
		add_custom_command(TARGET Editor POST_BUILD
			# CoreBridge (runtimeconfig/deps are used for CoreCLR initialization)
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0"
			        "$<TARGET_FILE_DIR:Editor>"
			# HotReload (may include additional dependencies like Roslyn)
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0"
			        "$<TARGET_FILE_DIR:Editor>"
			# ABI surfaces and editor managed integration
			# Use copy_if_different for Scripting.ABI so the staged DLL timestamp
			# only changes when its content changes — the ScriptManager compares
			# Scripts.dll mtime against this file to detect stale assemblies.
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/bin/$<CONFIG>/GameEngine.Scripting.ABI.dll"
			        "$<TARGET_FILE_DIR:Editor>/GameEngine.Scripting.ABI.dll"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/bin/$<CONFIG>/GameEngine.Scripting.ABI.deps.json"
			        "$<TARGET_FILE_DIR:Editor>/GameEngine.Scripting.ABI.deps.json"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Input.ABI/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Physics.ABI/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Platform.ABI/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Editor.Scripting.ABI/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/ECS.ABI/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Editor.Managed/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			# Scripting.Runtime (GameSystemRunner + exports, ships in editor AND builds)
			COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>"
			        "$<TARGET_FILE_DIR:Editor>"
			# EntitySystemGenerator (source generator for IEntitySystem, loaded by CompileServerHost)
			COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/SourceGenerators"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			        "${CMAKE_SOURCE_DIR}/Managed/SourceGenerators/EntitySystemGenerator/bin/$<CONFIG>/netstandard2.0/EntitySystemGenerator.dll"
			        "$<TARGET_FILE_DIR:Editor>/SourceGenerators/EntitySystemGenerator.dll"
			# CompileServerHost (Roslyn server used for script compilation in the Editor).
			# Canonical output lives under the repo root at
			#   Managed/CompileServerHost/bin/<Config>/net10.0/GameEngine.CompileServerHost.dll
			# Mirrored under the Editor's output so native clients can discover the
			# host relative to the working directory. The running host writes
			# CompileServerHost.log into the staged tree; the prune never touches a
			# file it did not stage, so the log survives a rebuild while an assembly
			# dropped from the build is removed. A log left in the source tree by
			# running the host in place is dropped first so it is never mirrored.
			COMMAND ${CMAKE_COMMAND} -E rm -f
			        "${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/bin/$<CONFIG>/net10.0/CompileServerHost.log"
			${_ge_compile_server_host_mirror}
			COMMENT "Staging managed assemblies (CoreBridge/HotReload/ABI) and CompileServerHost next to Editor.exe"
			VERBATIM
		)
	endif()

	# The managed assemblies are runtime deps — CoreCLR loads them at startup, so
	# they must exist on disk before the editor launches, and the staging above
	# only runs once every dotnet build has completed.
	set(_ge_editor_managed_deps
		HotReloadAssembly CoreBridgeAssembly ScriptingAbiAssembly InputAbiAssembly
		EcsAbiAssembly PhysicsAbiAssembly PlatformAbiAssembly ScriptingRuntimeAssembly
		EntitySystemGeneratorAssembly EditorManagedAssembly CompileServerHost)
	add_dependencies(Editor ${_ge_editor_managed_deps})

	# Bundle staging consumes the same stamps as file dependencies. On macOS
	# this avoids forcing a native Editor relink for a managed-only edit.
	set(_ge_editor_managed_stamps "")
	foreach(_dep IN LISTS _ge_editor_managed_deps)
		list(APPEND _ge_editor_managed_stamps "${_GE_MANAGED_STAMP_DIR}/${_dep}.$<CONFIG>.stamp")
	endforeach()
	if(APPLE)
		ge_add_editor_managed_staging(${_ge_editor_managed_stamps})
		add_dependencies(StageEditorManaged ${_ge_editor_managed_deps})
	else()
		# Other platforms stage managed files in the Editor POST_BUILD step above.
		# LINK_DEPENDS forces a relink — and so a re-stage — on a managed-only
		# change. Ninja and Makefile generators only; the Visual Studio generator
		# ignores the property.
		set_property(TARGET Editor APPEND PROPERTY LINK_DEPENDS ${_ge_editor_managed_stamps})
	endif()
endif()

# The staging stamps live inside the staged Assets root on every platform, so
# wiping that root (or the whole bundle) is enough to have the next build
# repair the complete tree. A custom command's OUTPUT may not use a
# target-dependent generator expression, so the stamp directory spells out the
# path $<TARGET_FILE_DIR:Editor> resolves to (ge_set_output_app puts the
# executable under bin/<Config>/Apps/Editor).
if(APPLE)
	set(_GE_EDITOR_ASSETS_DEST "$<TARGET_FILE_DIR:Editor>/../Resources/Assets")
	set(_GE_EDITOR_NOTICES_DEST "$<TARGET_FILE_DIR:Editor>/../Resources/ThirdPartyNotices")
	set(_GE_EDITOR_TEMPLATES_DEST "$<TARGET_FILE_DIR:Editor>/../Resources/ProjectTemplates")
	set(_GE_EDITOR_STAGE_STAMP_DIR
		"${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Apps/Editor/Editor.app/Contents/Resources/Assets")
else()
	set(_GE_EDITOR_ASSETS_DEST "$<TARGET_FILE_DIR:Editor>/Assets")
	set(_GE_EDITOR_NOTICES_DEST "$<TARGET_FILE_DIR:Editor>/ThirdPartyNotices")
	set(_GE_EDITOR_TEMPLATES_DEST "$<TARGET_FILE_DIR:Editor>/ProjectTemplates")
	set(_GE_EDITOR_STAGE_STAMP_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Apps/Editor/Assets")
endif()

if(DEFINED VCPKG_INSTALLED_DIR)
	set(_GE_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
elseif(DEFINED _VCPKG_INSTALLED_DIR)
	set(_GE_VCPKG_INSTALLED_DIR "${_VCPKG_INSTALLED_DIR}")
else()
	message(FATAL_ERROR "Third-party notice staging requires VCPKG_INSTALLED_DIR")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
	message(FATAL_ERROR "Third-party notice staging requires VCPKG_TARGET_TRIPLET")
endif()

# StageEditorAssets: stage source assets into the build output.
# Can be run independently without recompiling Editor:
#   cmake --build --preset <preset> --target StageEditorAssets
# Editor depends on this target so it also runs automatically after each build.
#
# The stamp lives inside the staged Assets directory (see _GE_EDITOR_STAGE_STAMP_DIR),
# so wiping that directory repairs the complete asset tree on the next build.
# CONFIGURE_DEPENDS picks up added and deleted source assets;
# the glob is also written to a list file the recipe depends on, because the
# inputs alone cannot signal a delete — the stamp stays newer than every input
# that remains — and the deleted file would keep being served until something
# else re-fired the staging. The installed triplet's share tree is globbed too,
# so an added or removed port restages the third-party notices.
file(GLOB_RECURSE _GE_STAGE_INPUTS CONFIGURE_DEPENDS
	"${_GE_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*"
	"${CMAKE_SOURCE_DIR}/Managed/licenses/*"
	"${CMAKE_CURRENT_SOURCE_DIR}/Assets/*"
	"${CMAKE_CURRENT_SOURCE_DIR}/ProjectTemplates/*"
	"${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles/*"
	"${CMAKE_SOURCE_DIR}/Assets/Materials/*"
	"${CMAKE_SOURCE_DIR}/Assets/RenderPipelines/*"
	"${CMAKE_SOURCE_DIR}/Assets/Textures/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Adapters/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Surfaces/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Particles/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Ocean/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Slang/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Graph/Nodes/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/*"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/*"
	"${CMAKE_SOURCE_DIR}/Assets/UI/UIDemoPanel.xml"
	"${CMAKE_SOURCE_DIR}/Assets/UI/UIDemoPanel.css"
)
list(FILTER _GE_STAGE_INPUTS EXCLUDE REGEX "/(\\.DS_Store|\\._[^/]+)$")
set(_GE_STAGE_INPUTS_LIST "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/editor-assets.list")
file(CONFIGURE OUTPUT "${_GE_STAGE_INPUTS_LIST}" CONTENT "${_GE_STAGE_INPUTS}" @ONLY)
list(APPEND _GE_STAGE_INPUTS
	"${_GE_STAGE_INPUTS_LIST}"
	"${_GE_EDITOR_MIRROR_PRUNE_SCRIPT}"
	"${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake"
	"${CMAKE_SOURCE_DIR}/Tools/Web/licenses/toolchain-notices.md"
	"${CMAKE_SOURCE_DIR}/Tools/Web/licenses/rust-dependency-notices.md"
	"${CMAKE_SOURCE_DIR}/Managed/THIRD_PARTY_NOTICES.md"
	"${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/NOTICE.md"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricFog/ThirdParty/diharaw-volumetric-fog/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/HeightFog/ThirdParty/SanielX-Height-Fog/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/ThirdParty/MirzaBeig-GPU-Fog-Particles/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Ocean/NOTICE"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Graph/ThirdParty/visual-shader-upstream/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/BakingLabAces/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/BakingLabAces/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/KhronosPbrNeutral/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/KhronosPbrNeutral/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/MinimalAgX/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/MinimalAgX/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/OpenColorIO/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/OpenColorIO/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/SpeedballGi/LICENSE.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/SpeedballGi/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/LICENSE"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/VENDORED.md"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/LICENSE"
	"${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/GT7ToneMapper/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/GT7ToneMapper/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/GTAO/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/GTAO/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/metal-cpp/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/metal-cpp/VENDORED.md"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/ufbx/LICENSE"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/ufbx/UFBX_VERSION.txt"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/fbtBlend/LICENSE"
	"${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/fbtBlend/UPDATE-NOTES.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/PunikontaVhs/LICENSE.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/PunikontaVhs/UPSTREAM.md"
	"${CMAKE_SOURCE_DIR}/ThirdParty/BendScreenSpaceShadows/LICENSE.txt"
	"${CMAKE_SOURCE_DIR}/ThirdParty/BendScreenSpaceShadows/UPSTREAM.md"
)

# EZ-Tree preset JSONs come from the vcpkg build input and must ship with the
# editor: LoadPreset resolves them exe-anchored under Assets/EZTree/Presets.
if(NOT DEFINED GE_EZTREE_PRESETS_SOURCE_DIR OR NOT IS_DIRECTORY "${GE_EZTREE_PRESETS_SOURCE_DIR}")
	message(FATAL_ERROR "GE_EZTREE_PRESETS_SOURCE_DIR is unset/missing (see cmake/Packages.cmake)")
endif()

# The mirrors, in staging order. Vertex modifiers ship with their owning
# packages (e.g. Packages/eztree), not from the engine shader tree.
set(_GE_EDITOR_MIRROR_COMMANDS "")
# Editor-owned assets: icons, fonts, themes, UI, default graphs and scenes.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Assets" "${_GE_EDITOR_ASSETS_DEST}")
# Project templates (manifest + payload folders), a sibling of Assets so
# template content never enters the editor asset mount.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_CURRENT_SOURCE_DIR}/ProjectTemplates" "${_GE_EDITOR_TEMPLATES_DEST}")
# Runtime humanoid profiles are shared by Editor and Player.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles" "${_GE_EDITOR_ASSETS_DEST}/SkeletonProfiles")
# Engine-level Materials (Material v2 surfaces, templates, etc.) so `.material`
# assets can reference surface shaders that live under the engine Assets tree.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Assets/Materials" "${_GE_EDITOR_ASSETS_DEST}/Materials")
# Engine-level textures used by built-in materials and native modules.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Assets/Textures" "${_GE_EDITOR_ASSETS_DEST}/Textures")
# Default render pipelines the Editor loads when the project names none.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Assets/RenderPipelines" "${_GE_EDITOR_ASSETS_DEST}/RenderPipelines")
# Shader adapter templates (Adapters, Includes, Surfaces) for runtime material
# compilation via MaterialBuildService / ShaderComposer.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Adapters" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Adapters")
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Includes")
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Surfaces" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Surfaces")
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Particles" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Particles")
# GPU fog particle surface implementation (included by Shaders/Surfaces/gpu_fog_particles.glsl).
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders/Surfaces" "${_GE_EDITOR_ASSETS_DEST}/Shaders/GPUFogParticles/Surfaces")
# Terrain grass sources, compiled at runtime via shaderc.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass" "${_GE_EDITOR_ASSETS_DEST}/Shaders/TerrainGrass")
# Ocean sources, compiled at runtime via shaderc through the material adapter
# pipeline; ocean_common.glsl is shared by the vertex modifier + surface.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Ocean" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Ocean")
# Slang-lane spike sources, compiled at runtime via slangc when
# GE_SHADER_SLANG_LANE=1 (see ShaderCompileService's SlangCompileLane).
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Slang" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Slang")
# CBT terrain sources. The vertex modifier, the surface and the GPU bake kernel
# (terrain_height_bake.comp, TerrainRenderFeature::EnsureGpuBakePipeline) are
# composed at runtime via shaderc and #include their siblings — cbt_layout,
# cbt_domain, cbt_atlas, cbt_sculpt, cbt_analytic, terrain_surface_rules — so the
# whole directory ships as one tree. The compiled compute kernel is a build
# product and is staged beside them by StageEditorShaders.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT" "${_GE_EDITOR_ASSETS_DEST}/Shaders/CBT")
# Shader-graph node helper libraries for editor palette reflection.
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Graph/Nodes" "${_GE_EDITOR_ASSETS_DEST}/Shaders/Graph/Nodes")
_ge_editor_mirror_commands(_GE_EDITOR_MIRROR_COMMANDS
	"${GE_EZTREE_PRESETS_SOURCE_DIR}" "${_GE_EDITOR_ASSETS_DEST}/EZTree/Presets")

set(_GE_PRUNE_STAGED_METADATA_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/PruneStagedEditorMetadata.cmake")
file(WRITE "${_GE_PRUNE_STAGED_METADATA_SCRIPT}" [=[
if(NOT DEFINED GE_STAGE_ROOT OR GE_STAGE_ROOT STREQUAL "" OR NOT IS_DIRECTORY "${GE_STAGE_ROOT}")
	return()
endif()

set(_ge_stage_dirs "${GE_STAGE_ROOT}")
file(GLOB_RECURSE _ge_stage_children LIST_DIRECTORIES true "${GE_STAGE_ROOT}/*")
foreach(_ge_stage_child IN LISTS _ge_stage_children)
	if(IS_DIRECTORY "${_ge_stage_child}")
		list(APPEND _ge_stage_dirs "${_ge_stage_child}")
	endif()
endforeach()

foreach(_ge_stage_dir IN LISTS _ge_stage_dirs)
	file(REMOVE "${_ge_stage_dir}/.DS_Store")
	file(GLOB _ge_appledouble_files LIST_DIRECTORIES false "${_ge_stage_dir}/._*")
	foreach(_ge_appledouble_file IN LISTS _ge_appledouble_files)
		file(REMOVE "${_ge_appledouble_file}")
	endforeach()
endforeach()
]=])

set(_GE_EDITOR_ASSETS_STAMP "${_GE_EDITOR_STAGE_STAMP_DIR}/.stage-assets.stamp")
add_custom_command(OUTPUT "${_GE_EDITOR_ASSETS_STAMP}"
	${_GE_EDITOR_MIRROR_COMMANDS}
	# Shared UI demo assets that live under the engine-level Assets/UI directory,
	# staged as editor assets. Named files, not a mirror: the directory holds
	# more than ships.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	        "${CMAKE_SOURCE_DIR}/Assets/UI/UIDemoPanel.xml"
	        "${_GE_EDITOR_ASSETS_DEST}/UI/UIDemoPanel.xml"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	        "${CMAKE_SOURCE_DIR}/Assets/UI/UIDemoPanel.css"
	        "${_GE_EDITOR_ASSETS_DEST}/UI/UIDemoPanel.css"
	# Stage third-party notices required by bundled engine modules/assets.
	COMMAND ${CMAKE_COMMAND}
	        "-DGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
	        "-DGE_NOTICES_DEST=${_GE_EDITOR_NOTICES_DEST}"
	        "-DGE_VCPKG_INSTALLED_DIR=${_GE_VCPKG_INSTALLED_DIR}"
	        "-DGE_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
	        -P "${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake"
	COMMAND ${CMAKE_COMMAND}
	        "-DGE_STAGE_ROOT=${_GE_EDITOR_ASSETS_DEST}"
	        -P "${_GE_PRUNE_STAGED_METADATA_SCRIPT}"
	COMMAND ${CMAKE_COMMAND} -E touch "${_GE_EDITOR_ASSETS_STAMP}"
	DEPENDS ${_GE_STAGE_INPUTS}
	COMMENT "Staging Editor install Assets to output/bundle"
	VERBATIM
)
add_custom_target(StageEditorAssets DEPENDS "${_GE_EDITOR_ASSETS_STAMP}")
set_target_properties(StageEditorAssets PROPERTIES FOLDER "Apps/Editor")
add_dependencies(Editor StageEditorAssets)

# StageSteamDeckTools (macOS): the macOS Steam Deck export runs these scripts
# from the editor's Resources/Tools/SteamDeck (EditorGlobalPaths::installToolsRoot)
# instead of from an engine checkout. engine-source.fingerprint identifies the
# engine sources this editor was built from; a cached Deck runtime is used only
# when it was built from the same sources (Tools/Scripts/deck_template.py). The
# target runs on every build because the fingerprint covers the whole engine
# tree; the stamp is rewritten only when it changes.
if(APPLE)
	set(_GE_STEAMDECK_TOOLS_DEST "$<TARGET_FILE_DIR:Editor>/../Resources/Tools/SteamDeck")
	set(_GE_STEAMDECK_TOOLS_COMMANDS
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_STEAMDECK_TOOLS_DEST}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		        "${CMAKE_SOURCE_DIR}/Tools/Scripts/steamdeck-launch.sh"
		        "${CMAKE_SOURCE_DIR}/Tools/Scripts/deck_template.py"
		        "${CMAKE_SOURCE_DIR}/Tools/Scripts/build_deck.py"
		        "${CMAKE_SOURCE_DIR}/Tools/Scripts/publish_game.py"
		        "${_GE_STEAMDECK_TOOLS_DEST}")
	find_package(Python3 3.10 COMPONENTS Interpreter)
	if(Python3_FOUND)
		list(APPEND _GE_STEAMDECK_TOOLS_COMMANDS
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/Scripts/deck_template.py" stamp
			        --repo "${CMAKE_SOURCE_DIR}"
			        --output "${_GE_STEAMDECK_TOOLS_DEST}/engine-source.fingerprint")
	else()
		message(STATUS "Python 3.10+ not found: the macOS Steam Deck export will rebuild its runtime every time")
	endif()
	add_custom_target(StageSteamDeckTools
		${_GE_STEAMDECK_TOOLS_COMMANDS}
		COMMENT "Staging Steam Deck export tools"
		VERBATIM)
	set_target_properties(StageSteamDeckTools PROPERTIES FOLDER "Apps/Editor")
	add_dependencies(Editor StageSteamDeckTools)
endif()

# Stage compiled .shaderpkg and .spv into the same Assets/Shaders directory as the
# adapter templates. Depend directly on every generated package AND every raw
# .spv output so shader-only changes refresh the bundle without running the
# staging command on no-op builds — depending on the packages alone left the
# staged .spv stale when an edit changed only the raw SPIR-V (a shader fix
# would silently not reach the running editor).
get_property(_GE_EDITOR_SHADERPKG_OUTPUTS GLOBAL PROPERTY GE_SHADERPKG_OUTPUTS)
get_property(_GE_EDITOR_SHADER_SPV_OUTPUTS GLOBAL PROPERTY GE_SHADER_SPV_OUTPUTS)
# The CBT compute kernel (CBTCompileShaders, outside GE_SHADER_SPV_OUTPUTS) is
# loaded from Assets/Shaders/CBT beside its GLSL sources
# (CBTRenderFeature::FindCBTShaderDir), not from the flat Shaders/ the packages
# land in. Staged here with the other build products, so a kernel rebuild
# reaches the editor without an Editor relink.
set(_GE_EDITOR_CBT_KERNEL_COMMANDS "")
set(_GE_EDITOR_CBT_KERNEL_DEPENDS "")
if(TARGET CBTCompileShaders)
	# Both heap arms: CBTKernelSet opens the narrow one on a device without
	# 64-bit integers (and under GE_FORCE_COMPAT), the wide one everywhere else.
	set(_GE_EDITOR_CBT_KERNEL_DEPENDS
		"${CMAKE_BINARY_DIR}/Shaders/cbt_kernels.comp.spv"
		"${CMAKE_BINARY_DIR}/Shaders/cbt_kernels_heap32.comp.spv")
	set(_GE_EDITOR_CBT_KERNEL_COMMANDS
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		        "${CMAKE_BINARY_DIR}/Shaders/cbt_kernels.comp.spv"
		        "${_GE_EDITOR_ASSETS_DEST}/Shaders/CBT/cbt_kernels.comp.spv"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		        "${CMAKE_BINARY_DIR}/Shaders/cbt_kernels_heap32.comp.spv"
		        "${_GE_EDITOR_ASSETS_DEST}/Shaders/CBT/cbt_kernels_heap32.comp.spv")
endif()
set(_GE_EDITOR_SHADERS_STAMP "${_GE_EDITOR_STAGE_STAMP_DIR}/Shaders/.stage-shaders.stamp")
add_custom_command(OUTPUT "${_GE_EDITOR_SHADERS_STAMP}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_EDITOR_ASSETS_DEST}/Shaders"
	COMMAND ${CMAKE_COMMAND}
			-DSRC_DIR=${CMAKE_BINARY_DIR}/Shaders
			-DDST_DIR=${_GE_EDITOR_ASSETS_DEST}/Shaders
			-P "${CMAKE_SOURCE_DIR}/cmake/StageShaderPkgs.cmake"
	${_GE_EDITOR_CBT_KERNEL_COMMANDS}
	COMMAND ${CMAKE_COMMAND} -E touch "${_GE_EDITOR_SHADERS_STAMP}"
	DEPENDS ${_GE_EDITOR_SHADERPKG_OUTPUTS} ${_GE_EDITOR_SHADER_SPV_OUTPUTS}
			${_GE_EDITOR_CBT_KERNEL_DEPENDS}
			"${CMAKE_SOURCE_DIR}/cmake/StageShaderPkgs.cmake"
	COMMENT "Staging compiled shaders (.shaderpkg/.spv) to Assets/Shaders"
	VERBATIM
)
add_custom_target(StageEditorShaders DEPENDS "${_GE_EDITOR_SHADERS_STAMP}")
set_target_properties(StageEditorShaders PROPERTIES FOLDER "Apps/Editor")
add_dependencies(StageEditorShaders StageEditorAssets)
if(TARGET CompileShaderPkgs)
	add_dependencies(StageEditorShaders CompileShaderPkgs)
endif()
if(TARGET CBTCompileShaders)
	add_dependencies(StageEditorShaders CBTCompileShaders)
endif()
add_dependencies(Editor StageEditorShaders)

# macOS: stage MoltenVK (Vulkan-on-Metal) into the Editor.app bundle at *build time*
# so Finder/Dock launches succeed even when VULKAN_SDK wasn't exported during CMake configure.
if(APPLE)
	option(GE_STAGE_MOLTENVK_IN_BUNDLE "Stage MoltenVK into Editor.app for Finder/Dock launches" ON)
	# Temporary bridge for local MoltenVK patches such as drawIndirectCount support.
	# Leave these empty for normal SDK/Homebrew discovery; remove the override once
	# official MoltenVK releases expose the required support.
	# Upstream PR: https://github.com/KhronosGroup/MoltenVK/pull/2740
	set(GE_MOLTENVK_SOURCE_LIB "" CACHE FILEPATH "Explicit libMoltenVK.dylib to stage into Editor.app before auto-detected SDK copies")
	set(GE_MOLTENVK_SOURCE_ICD "" CACHE FILEPATH "Explicit MoltenVK_icd.json to stage into Editor.app before auto-detected SDK copies")
	if(GE_STAGE_MOLTENVK_IN_BUNDLE)
		add_custom_command(TARGET Editor POST_BUILD
			COMMAND ${CMAKE_COMMAND}
					-DGE_APP_EXE=$<TARGET_FILE:Editor>
					-DGE_REQUIRE_MOLTENVK=ON
					-DGE_MOLTENVK_VERBOSE=OFF
					-DGE_MOLTENVK_SOURCE_LIB=${GE_MOLTENVK_SOURCE_LIB}
					-DGE_MOLTENVK_SOURCE_ICD=${GE_MOLTENVK_SOURCE_ICD}
					-P "${CMAKE_SOURCE_DIR}/cmake/StageMoltenVkAppBundle.cmake"
			COMMENT "Staging MoltenVK into Editor.app bundle (macOS)"
			VERBATIM
		)
	endif()

	# Ensure MoltenVK ICD JSON is present even if only the dylib was staged by older build logic.
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND}
				-DOUTPUT="$<TARGET_FILE_DIR:Editor>/../Resources/vulkan/icd.d/MoltenVK_icd.json"
				-DINPUT=${GE_MOLTENVK_SOURCE_ICD}
				-P "${CMAKE_SOURCE_DIR}/cmake/WriteMoltenVkIcdJson.cmake"
		COMMENT "Ensuring MoltenVK_icd.json exists in Editor.app bundle (macOS)"
		VERBATIM
	)

	# GLFW (especially < 3.4) may dlopen libvulkan.1.dylib / libvulkan.dylib by name.
	# Ensure these aliases exist inside Contents/Frameworks when vcpkg staged only a fully-versioned dylib.
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND}
				-DGE_APP_EXE=$<TARGET_FILE:Editor>
				-P "${CMAKE_SOURCE_DIR}/cmake/FixupVulkanLoaderNames.cmake"
		COMMENT "Fixing Vulkan loader dylib aliases in Editor.app bundle (macOS)"
		VERBATIM
	)
endif()

# Remove legacy/stale Shaders directories that can linger across incremental
# builds from older layouts. These shadow the current Assets/Shaders/ staging
# because ResolveShaderPkgPath may find them via the CWD before the asset
# resolver runs.
if(APPLE)
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND /bin/sh -c "rm -rf \"$<TARGET_FILE_DIR:Editor>/Assets\" \"$<TARGET_FILE_DIR:Editor>/Shaders\" || true"
		COMMAND /bin/sh -c "rm -rf \"$<TARGET_FILE_DIR:Editor>/../Resources/Shaders\" || true"
		COMMENT "Removing legacy/stale bundle directories (Contents/MacOS/{Assets,Shaders}, Contents/Resources/Shaders)"
		VERBATIM
	)
else()
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Editor>/Shaders"
		COMMENT "Removing legacy Shaders/ directory (shaders now staged in Assets/Shaders/)"
	)
endif()

# macOS: validate the produced Editor.app bundle layout and linkage.
# This runs as a final post-build check to catch non-portable dependency paths,
# missing Resources/Frameworks staging, or bad rpaths before shipping/running from Finder.
if(APPLE)
	# Best practice is to do bundle fixups/verification at install/package time.
	# Keep these post-build steps opt-in for developer convenience.
	# Validation is cheap compared to debugging Finder-only failures; enable by default.
	option(GE_VALIDATE_MAC_BUNDLE "Validate macOS app bundle layout after build" ON)
	option(GE_FIXUP_MAC_BUNDLE "Fix up macOS app bundle dylib install names/rpaths after build" ON)
	if(GE_FIXUP_MAC_BUNDLE)
		add_custom_command(TARGET Editor POST_BUILD
			COMMAND ${CMAKE_COMMAND}
					-DGE_APP_EXE=$<TARGET_FILE:Editor>
					-DGE_FIXUP_VERBOSE=OFF
					-P "${CMAKE_SOURCE_DIR}/cmake/FixupMacAppBundle.cmake"
			COMMENT "Fixing up Editor.app dylib install names/rpaths (macOS)"
			VERBATIM
		)
	endif()
	if(GE_VALIDATE_MAC_BUNDLE)
		add_custom_command(TARGET Editor POST_BUILD
			COMMAND ${CMAKE_COMMAND}
					-DGE_APP_EXE=$<TARGET_FILE:Editor>
					-DGE_VALIDATE_VERBOSE=OFF
					-P "${CMAKE_SOURCE_DIR}/cmake/ValidateMacAppBundle.cmake"
			COMMENT "Validating Editor.app bundle layout (macOS)"
			VERBATIM
		)
	endif()
endif()

# Linux: stage icon PNG for runtime loading via glfwSetWindowIcon.
# On Windows/macOS the embedded resource/bundle icon is used, but Linux has
# no standard way to embed icons in ELF executables.
if(NOT WIN32 AND NOT APPLE)
	add_custom_command(TARGET Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${EDITOR_ICON_DIR}/AppIcon.png"
				"$<TARGET_FILE_DIR:Editor>/AppIcon.png"
		COMMENT "Staging application icon for Linux runtime loading"
	)
endif()

# Stage Engine SDK alongside Editor for the build pipeline.
# The SDK contains headers, static libs, cmake config, and Player template sources
# so the Editor can compile Player executables without access to the engine source tree.
include(${CMAKE_SOURCE_DIR}/cmake/GameEngineSDK.cmake)
ge_stage_engine_sdk(Editor "$<TARGET_FILE_DIR:Editor>/SDK")

# macOS: ensure the final on-disk Editor.app is codesigned *after* every
# post-build mutation, including Engine SDK staging above and the engine_abi
# markers below. Finder/Dock launches and strict verification reject bundles
# whose sealed contents change later. The markers hash the SDK libraries' size
# and modification time, and a library's first signature changes its size, so
# those libraries are signed before the markers are written; the final signature
# with the same identity keeps that size.
if(APPLE)
	option(GE_CODESIGN_MAC_BUNDLE_POST_BUILD "Re-sign Editor.app after post-build staging so Finder/Dock launches work" ON)
	set(GE_MAC_CODESIGN_IDENTITY "" CACHE STRING "Stable macOS codesign identity for Editor.app post-build signing; empty uses Xcode identity or ad-hoc")
	if(GE_CODESIGN_MAC_BUNDLE_POST_BUILD)
		add_custom_command(TARGET Editor POST_BUILD
			COMMAND ${CMAKE_COMMAND}
					-DGE_APP_BUNDLE=$<TARGET_BUNDLE_DIR:Editor>
					-DGE_CODESIGN_IDENTITY=${GE_MAC_CODESIGN_IDENTITY}
					-DGE_CODESIGN_SDK_LIBRARIES=ON
					-P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
			COMMENT "Signing the Editor SDK's libraries before the engine_abi markers (macOS)"
			VERBATIM
		)
	endif()
endif()

# engine_abi markers for the prebuilt engine-package modules — after BOTH the
# packages (DLLs) and the SDK (digest inputs) are staged and, on macOS, the SDK
# libraries signed; POST_BUILD commands run in call order.
ge_stage_engine_package_markers(Editor)

if(APPLE)
	if(GE_CODESIGN_MAC_BUNDLE_POST_BUILD)
		add_custom_command(TARGET Editor POST_BUILD
			COMMAND ${CMAKE_COMMAND}
					-DGE_APP_BUNDLE=$<TARGET_BUNDLE_DIR:Editor>
					-DGE_CODESIGN_IDENTITY=${GE_MAC_CODESIGN_IDENTITY}
					-P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
			COMMENT "Re-signing Editor.app after all post-build staging (macOS)"
			VERBATIM
		)
	endif()
endif()

# Editor-side unit tests (device-loss surfacing today). Uses the global switch.
if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(Tests)
endif()
