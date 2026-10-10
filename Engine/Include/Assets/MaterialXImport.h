#pragma once

#include "Rendering/Materials/MaterialDocument.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

class AssetRegistry;

// Import a MaterialX (.mtlx) OpenPBR-surface document into our native MaterialDocument
// backed by Surfaces/standard_pbr.glsl. Our StandardPBR is an OpenPBR implementation, so
// the OpenPBR Surface inputs map ~1:1 onto the .material property set; the produced doc is
// an ordinary StandardPBR material (one material type) with the shader locked.
//
// Scope: a single open_pbr_surface node. Constant value= inputs map directly; texture inputs
// driven by an <image> graph are traced to their backing file (relative to @p mtlxPath),
// resolved against @p registry to a GUID, and stored in the matching texture slot. When the
// file is unknown to the asset DB the path is stored as a fallback; @p registry may be null
// (headless/unit-test parsing) in which case the resolved path is stored verbatim. Arbitrary
// non-texture node graphs are still skipped and logged, never fatal.
//
// Returns false only on a malformed document (not XML / no open_pbr_surface node); a doc
// that parses but uses unsupported inputs still succeeds with those inputs degraded.
bool ParseMaterialX(const std::string& xml,
                    const std::filesystem::path& mtlxPath,
                    AssetRegistry* registry,
                    MaterialDocument& outDoc,
                    std::vector<std::string>& errors);

// Extract the authored image-file paths referenced by the texture-driven inputs of a .mtlx's
// open_pbr_surface (relative to the .mtlx). For dependency extraction — no GUID resolution; the
// caller resolves each path against the .mtlx directory. Returns empty for a non-MaterialX doc.
std::vector<std::string> CollectMaterialXTextureFiles(const std::string& xml);

// Write the supported (mapped) MaterialDocument properties back into an existing .mtlx file,
// preserving the rest of the document (unsupported nodes, graphs, comments). Only constant-value
// open_pbr_surface inputs are updated/created; graph-driven inputs (textures) are left untouched.
// This is how edits to a shader-locked (imported) material round-trip in MaterialX's own format.
// Returns false if the file can't be read/parsed/written.
bool WriteMaterialX(const std::string& mtlxPath, const MaterialDocument& doc);

} // namespace GameEngine
