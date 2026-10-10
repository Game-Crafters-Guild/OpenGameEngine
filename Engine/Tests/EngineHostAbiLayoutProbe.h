#pragma once

// Layout of the engine types a host reads through inline accessors, measured in
// a translation unit whose NDEBUG state the probe reports alongside the sizes.
//
// Two translation units implement this: one compiled with NDEBUG defined, one
// with it undefined. Engine.dll is compiled with exactly one of those states and
// a packaged game routinely with the other, so any engine type whose layout
// moves between them is a type the two sides disagree about — the host reads
// every member past the first difference at an offset the engine does not use.

#include <cstddef>

namespace GameEngine::Testing
{

struct EnginePublicTypeLayout
{
    /// NDEBUG state of the translation unit that produced the sizes below.
    /// Reported so the comparison can prove the two probes actually differ:
    /// two probes that happened to agree on NDEBUG would compare a value with
    /// itself and pass having measured nothing.
    bool NDebugDefined;

    std::size_t RenderServices;
    std::size_t MeshGPURegistry;
    std::size_t MaterialBinder;
    std::size_t PipelineVariantCache;
    std::size_t RenderExtractionSystem;
    std::size_t WorkStealingThreadPool;
    std::size_t TaskDependencyGraph;
    std::size_t ApplicationConfig;
};

/// Measured with NDEBUG defined (a shipping game's configuration).
EnginePublicTypeLayout MeasureEnginePublicTypeLayoutWithNDebug();

/// Measured with NDEBUG undefined (the engine's Debug and DebugFast configurations).
EnginePublicTypeLayout MeasureEnginePublicTypeLayoutWithoutNDebug();

} // namespace GameEngine::Testing
