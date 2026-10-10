#pragma once

#include "EditorApplication.h"
#include <cstdint>

namespace GameEngine
{
struct ColorPickerContext;

// Helper functions that keep ColorPicker-specific wiring out of EditorApplication.cpp.
// All color picker visuals are now rendered as shader gradients -- no external textures
// are needed. This module manages per-window ColorPickerContext lifetime (callbacks,
// eyedropper state).
namespace ColorPickerEditorIntegration
{
// Id-based accessors (preferred).
ColorPickerContext* GetOrCreateContext(uint64_t windowId);
ColorPickerContext* GetContext(uint64_t windowId);
void Cleanup(uint64_t windowId);

// Ensure the window has an associated ColorPickerContext and return it.
// The context lifetime is managed by this module (not stored on EditorWindowContext).
ColorPickerContext* GetOrCreateContext(EditorApplication::EditorWindowContext* ctx);

// Get the context if it exists; returns nullptr otherwise.
ColorPickerContext* GetContext(EditorApplication::EditorWindowContext* ctx);

// Clean up the color picker context for the given window.
void Cleanup(EditorApplication::EditorWindowContext* ctx);
} // namespace ColorPickerEditorIntegration
} // namespace GameEngine
