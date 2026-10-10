#pragma once

#include <cstdint>
#include <functional>

namespace GameEngine
{

struct TerrainMaterialEntry;
class TerrainMaterialLibraryAsset;
namespace Editor { class UndoRedoService; }

void RegisterTerrainMaterialLibraryInspector();

// The two halves of the colour contract between the picker and a material's tint. The picker and
// every CSS colour are sRGB BYTES; a tint is LINEAR. Encoding on the way out and decoding on the
// way in is what keeps the swatch showing the colour the terrain actually shades with, so the two
// are declared together — changing one without the other is the mismatch they exist to prevent.

// The swatch colour for a material's linear tint.
std::uint32_t TerrainMaterialTintToSwatchArgb(const TerrainMaterialEntry& entry);

// Write a picker's sRGB byte colour into the material holding `slotId` as a linear tint, as one
// undoable whole-document edit. No-op when the slot holds no material.
void ApplyTerrainMaterialTintFromPicker(TerrainMaterialLibraryAsset* library,
                                        Editor::UndoRedoService* undo,
                                        const std::function<void()>& requestRefresh,
                                        std::uint8_t slotId, std::uint32_t argb);

} // namespace GameEngine
