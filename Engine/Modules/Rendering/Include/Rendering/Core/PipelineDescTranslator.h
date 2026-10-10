// PipelineDescTranslator.h — single owner of `PipelineDesc` →
// `GraphicsPipelineDesc` / `ComputePipelineDesc` + `PipelineFormatKey`
// translation.
//
// The legacy `PipelineDesc` co-mingles *what the pipeline is* (shaders,
// state, descriptor layouts) with *what render-pass formats it binds to*
// (color/depth attachment formats, sample count). The new typed descs split
// these so format-bearing attributes live in `PipelineFormatKey` and don't
// participate in the desc's identity hash.
//
// Multiple call sites need to convert a builder-style `PipelineDesc` into
// the typed form (editor overlays, render-graph nodes, terrain features,
// sky/UI passes, the device's `CreatePipeline` convenience). Centralizing
// the translation here removes ten near-identical inline copies and makes
// it impossible to forget a field (e.g. `pushConstantRanges`, the named
// push-constant ranges that must flow into `NamedPushConstantRanges`).

#pragma once

#include "Rendering/Core/PipelineIdentifiers.h"

namespace GameEngine::Rendering
{

class IDevice;
struct PipelineDesc;

namespace PipelineDescTranslator
{

// Translate a graphics `PipelineDesc` into the typed desc and return its
// interned `GraphicsPipelineId`. Descriptor-set layouts are interned via
// `device.InternDescriptorSetLayout`. Formats are NOT consumed here —
// callers that go through the concrete cache should pair this with
// `BuildFormatKey`.
GraphicsPipelineId InternGraphics(IDevice& device, const PipelineDesc& base);

// Compute analogue. Compute pipelines have no format dimension.
ComputePipelineId InternCompute(IDevice& device, const PipelineDesc& base);

// Extract attachment formats and sample count into a `PipelineFormatKey`.
PipelineFormatKey BuildFormatKey(const PipelineDesc& base);

} // namespace PipelineDescTranslator

} // namespace GameEngine::Rendering
