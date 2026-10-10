#pragma once

#include <cstdint>

namespace GameEngine {
namespace UiReplayCommandIds {

// Assets browser navigation commands (sandbox paths for deterministic UIReplay runs).
inline constexpr std::uint32_t AssetsNavSandboxRoot = 0xF0A10001u;
inline constexpr std::uint32_t AssetsNavSandboxSrc = 0xF0A10002u;
inline constexpr std::uint32_t AssetsNavSandboxDst = 0xF0A10003u;

// Assets panel view mode commands.
inline constexpr std::uint32_t AssetsViewGrid = 0xF0A10101u;
inline constexpr std::uint32_t AssetsViewList = 0xF0A10102u;

// Tear-out regression commands.
inline constexpr std::uint32_t UndockHierarchy = 4042000001u;
inline constexpr std::uint32_t CloseLastFloatingWindow = 4042000002u;
inline constexpr std::uint32_t UndockSceneView = 4042000003u;
inline constexpr std::uint32_t UndockGameView = 4042000004u;
inline constexpr std::uint32_t ReattachLastFloatingWindow = 4042000005u;
inline constexpr std::uint32_t UndockInspector = 4042000006u;
inline constexpr std::uint32_t UndockNodeGraph = 4042000007u;
inline constexpr std::uint32_t UndockAssets = 4042000008u;

// Asset pipeline commands.
inline constexpr std::uint32_t ReimportProjectLODs = 0xF0B10001u;
inline constexpr std::uint32_t BakeHlodProject = 0xF0B10002u;

// Scene document commands.
inline constexpr std::uint32_t SaveScene = 0xF0B10003u;

// Play mode action commands.
inline constexpr std::uint32_t PlayEnter = 4042000010u;
inline constexpr std::uint32_t PlayTogglePause = 4042000011u;
inline constexpr std::uint32_t PlayStop = 4042000012u;

// Play mode assertion commands.
inline constexpr std::uint32_t AssertPlayModeEdit = 4042000020u;
inline constexpr std::uint32_t AssertPlayModePlay = 4042000021u;
inline constexpr std::uint32_t AssertPlayModePaused = 4042000022u;
inline constexpr std::uint32_t AssertPlayModeNotRunning = 4042000023u;

// Panel/runtime assertion commands.
inline constexpr std::uint32_t AssertGameViewNoCameraOverlayVisible = 4042000030u;
inline constexpr std::uint32_t AssertFloatingHierarchyWindow = 4042000031u;
inline constexpr std::uint32_t AssertFloatingSceneViewWindow = 4042000032u;
inline constexpr std::uint32_t AssertFloatingGameViewWindow = 4042000033u;
inline constexpr std::uint32_t AssertFloatingInspectorWindow = 4042000034u;
inline constexpr std::uint32_t AssertFloatingAssetsWindow = 4042000035u;

} // namespace UiReplayCommandIds
} // namespace GameEngine
