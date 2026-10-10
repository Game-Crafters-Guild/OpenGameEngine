#pragma once

#include "AssetCore/GUID.h"
#include "Components/Terrain/Terrain.h"
#include "UI/Controls/Dropdown.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Foldout;
class TerrainMaterialLibraryAsset;
class UIElement;
struct TerrainMaterialEntry;
namespace Editor { class UndoRedoService; }
} // namespace GameEngine

// The terrain panel's view of a material library, and the pieces of it the paint-layer effect's
// picker shares: what a channel role is called, what a role may be bound to, how a material is
// added, and the undoable library edit under all of it. A second copy of any of them is a second
// answer to "what is role 1 called" or "which slot does a new material take".
namespace GameEngine::Editor::TerrainRoleMaterials
{

// The value a picker option carries for "+ Create new material…". A slot ID can never be this,
// so the handler tells the two apart without a parallel flag.
inline constexpr const char* kCreateMaterialOptionValue = "create";

// The library a terrain shades from, or nullptr when it binds none, the GUID does not resolve, or
// the asset is not resident yet. Resolved through the asset manager, so the editor reads the same
// document the library inspector writes.
TerrainMaterialLibraryAsset* ResolveLibrary(const Components::Terrain& terrain);

// Applies one edit to the library, writes it, and records a single undo entry over the whole
// document. A library is a list, so a per-field entry would have to describe a row a later remove
// may have deleted; the document is the one snapshot that always means something.
//
// The terrain does not repaint until the file watcher reloads the asset: extraction shades from a
// cached PARSE of the library, which the reload event drops. That is the same latency a .material
// edit has, and it is why the mutation is written rather than only applied in memory.
void EditLibraryUndoable(TerrainMaterialLibraryAsset* library, UndoRedoService* undo,
                         const std::string& label, const std::function<void()>& requestRefresh,
                         const std::function<void(std::vector<TerrainMaterialEntry>&)>& mutate);

// What a role is called in a picker: the bound material's name when one resolves, otherwise the
// role's semantic name (Grass / Rock / Dirt / Snow), which is what an unmigrated terrain shows.
std::string RoleLabel(const Components::Terrain& terrain,
                      const TerrainMaterialLibraryAsset* library, std::uint32_t role);

// The albedo texture a role shades with, or a null GUID when it shades with none.
//
// Mirrors RoleLabel's authority rule, and the runtime resolve behind it: with a library the
// role's slot picks the entry and the ENTRY's albedo answers; without one the terrain's own
// per-layer field does, which is what an unmigrated terrain shades from
// (TerrainMaterialAuthoring.cpp AuthorLegacyTerrainMaterialRecord). Null for an out-of-range
// role, a slot no entry holds, and a material that binds no albedo — three different reasons
// for the same answer, because a surface that shades from a tint has no texture to show.
GUID RoleAlbedoTexture(const Components::Terrain& terrain,
                       const TerrainMaterialLibraryAsset* library, std::uint32_t role);

// The four roles, in channel order, each labelled by RoleLabel. Option values are the role
// index, so a handler reads one back with std::stoi.
std::vector<Dropdown::Option> BuildRoleOptions(const Components::Terrain& terrain,
                                               const TerrainMaterialLibraryAsset* library);

// Every material a role may be bound to — the library's non-retired entries, with the slot ID as
// the option value — followed by "+ Create new material…". Retired materials are omitted: a
// tombstone keeps painted ground shading and disappears from pickers, so offering one here would
// be re-binding to a material the author retired.
//
// A material is labelled by its NAME alone, and carries a " (slot N)" suffix only when another
// live material answers to the same name. Names are not unique — two may share one, and the slot
// ID is what painted ground stores — but a suffix on every row puts a number the author never
// typed on the closed control, which shows the selected option's label.
//
// The create entry is omitted once all 256 slot IDs are spoken for, because a create that cannot
// allocate an ID has nothing to do. Dropdown options carry no disabled state, so the reason is
// said in a note beside the picker rather than on a greyed row.
std::vector<Dropdown::Option> BuildMaterialOptions(const TerrainMaterialLibraryAsset* library);

// Appends a material to the library, taking the lowest free slot ID, as one undoable edit.
// Returns the slot the new material took, or kInvalidTerrainMaterialSlotId when all 256 are
// spoken for and nothing was added.
//
// The slot is resolved against the list being mutated rather than read beforehand: a value read
// earlier names whatever was free then, and an edit in between would land this material on a slot
// another material already holds.
std::uint32_t AddLibraryMaterial(TerrainMaterialLibraryAsset* library, UndoRedoService* undo,
                                 const std::function<void()>& requestRefresh);

// Index into BuildMaterialOptions of the material the role currently binds, or -1 when its slot
// holds no entry — a role left over from a removed material, which shades the built-in one.
int SelectedMaterialOption(const Components::Terrain& terrain,
                           const TerrainMaterialLibraryAsset* library, std::uint32_t role);

// A bound terrain's materials, as the library holds them: a wrapping grid of cards, one per live
// material, each a tint swatch over the material's name, followed by a tile that adds one. The
// materials are what the author names and reuses, so they are what the panel shows; the four
// channel roles are a binding over them, which AddLayerRoleBlock keeps.
//
// Retired materials are omitted, the same rule the pickers follow: a tombstone keeps painted
// ground shading, and the library inspector is the one surface that can bring it back.
//
// A card activates on DOUBLE click, the asset browser's idiom for opening a card, and hands the
// LIBRARY's path to `openAsset` — the editor's navigate-to-and-select-asset callback. The add tile
// acts on a single click, because a "+" that ignores the first one reads as dead, and is omitted
// when no slot is free.
//
// `library` is read while this builds and never held afterwards. A handler outlives the build, and
// the asset manager's loaded map is the only strong owner of a library, so opening another project
// ejects the very asset these cards were built from. `onAdd` therefore belongs to the caller,
// which re-resolves the library by GUID at click time — the rule the role pickers' commit follows.
//
// Returns the grid container. Cards are addressable as terrain-material-<slot>-card, their
// swatches as -swatch and their names as -name; the add tile is terrain-material-add and the grid
// itself terrain-material-grid. Builds nothing (and returns nullptr) without a library.
UIElement* AddMaterialGrid(UIElement* parent, const TerrainMaterialLibraryAsset* library,
                           const std::function<void()>& onAdd,
                           const std::function<void(const std::filesystem::path&)>& openAsset);

// The channel→material binding: one plain label-and-picker row per role, in channel
// order, inside a block that starts open. The roles are the splat's four fixed
// channels — transitional plumbing the grid's materials outlive — but while they exist the
// binding is load-bearing information, so it is visible by default.
//
// The role name is the row's LABEL because a role and the material bound to it are two identities,
// not one: role "Snow" may shade with a material called "Packed Snow", and a row carrying only the
// material cannot say which role it is. Role names are a fixed naming convention over the four
// channels, NOT a claim about what puts weight in them — an authored rule is free to put rock in
// the channel named "Grass" — so the label is a label, not a field.
//
// Returns the block and its pickers indexed by role, for the caller to wire: binding writes the
// terrain component, which is the panel's job, not this TU's. Rows are addressable as
// terrain-role-<N>-row, their names as -name, their pickers as -material; the block is
// terrain-layer-roles. Builds nothing without a library — with no materials to choose from there is
// no binding to author, and an unmigrated terrain authors its channels through the per-layer cards.
struct LayerRoleBlock
{
    Foldout* Block = nullptr;
    std::vector<Dropdown*> Pickers;
};

LayerRoleBlock AddLayerRoleBlock(UIElement* parent, const Components::Terrain& terrain,
                                   const TerrainMaterialLibraryAsset* library);

// The route from a terrain to the library it shades from: a row whose button hands the library's
// path to `openAsset` — the editor's navigate-to-and-select-asset callback, and selecting an asset
// is what shows it in the Inspector. Builds nothing (and returns nullptr) without a resolved
// library, a path, or that callback, so the button never appears unless it can do its job.
Button* AddOpenLibraryRow(UIElement* parent, const TerrainMaterialLibraryAsset* library,
                          const std::function<void(const std::filesystem::path&)>& openAsset);

} // namespace GameEngine::Editor::TerrainRoleMaterials
