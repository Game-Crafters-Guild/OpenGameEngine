#pragma once

// MeshNameRegistry: recovers the source-node name behind a
// MeshRenderer::MeshNameId.
//
// `MeshNameId` is an FNV hash (Components::HashMeshName) and a hash is one-way,
// so the component alone cannot say which submesh it selects. This table holds
// the other half: the string each live id was derived from, so a save can write
// the name instead of the number and a resolver miss can say which mesh it
// looked for.
//
// Responsibilities:
//   - Intern a submesh name and hand back the id that selects it. Interning is
//     the ONLY way an id is produced from a name, so every id in circulation
//     can be turned back into text.
//   - Answer id -> name for the scene serializer and the resolver diagnostic.
//
// Ownership:
//   - One table per process, inside Engine.dll, reached through these free
//     functions. Entries are never evicted: the vocabulary is the distinct
//     submesh names of the content that has been loaded, and an id may outlive
//     the world whose scene introduced it.
//   - Deliberately NOT owned by RenderServices or by a World: scene load and
//     save must work headless, where neither exists.
//
// Thread safety:
//   - Both calls are serialized by an internal mutex. Neither sits on a hot
//     path: names are interned at scene load and at first bind, and read at
//     save and on the once-per-(model, id) miss warning.

#include "Types/StringId.h"

#include <string>
#include <string_view>

namespace GameEngine
{
namespace Engine
{
namespace Renderer
{

/// Record @p name and return the MeshRenderer::MeshNameId that selects it.
/// Equivalent to Components::HashMeshName(name) plus the reverse mapping, and
/// the id is re-derived from the name on every load — so changing the hash
/// function re-keys the table instead of orphaning a stored binding.
/// An empty name interns nothing and returns 0 ("no selector").
StringId InternMeshName(std::string_view name);

/// The name @p id was interned from, or an empty string when the id never came
/// through InternMeshName (a scene that carried only the raw `meshNameHash`).
std::string FindMeshName(StringId id);

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
