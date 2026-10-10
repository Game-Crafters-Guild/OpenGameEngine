#include "Assets/AuthoredLodImport.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MeshLODGenerator.h" // MeshLODConfig::kMaxLODs
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h" // HashMeshName / FoldMeshLodSuffix
#include "Logger/Logger.h"
#include "Scene/SceneValue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {

constexpr uint32 kMaxExtraLods = MeshLODConfig::kMaxLODs - 1u; // LOD0 + up to 3

// A trailing `_LOD<N>` suffix parsed off a submesh name. `HasSuffix` is false for
// a bare name (which is then treated as LOD0 of its own base).
struct LodSuffix {
    std::string_view Base; // name with the suffix stripped
    uint32 Level = 0;
    bool HasSuffix = false;
};

LodSuffix ParseLodSuffix(std::string_view name)
{
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    size_t digitsBegin = name.size();
    while (digitsBegin > 0 && name[digitsBegin - 1] >= '0' && name[digitsBegin - 1] <= '9')
        --digitsBegin;
    // Need at least one digit and a preceding "_LOD".
    if (digitsBegin < name.size() && digitsBegin >= 4 &&
        name[digitsBegin - 4] == '_' &&
        lower(name[digitsBegin - 3]) == 'l' &&
        lower(name[digitsBegin - 2]) == 'o' &&
        lower(name[digitsBegin - 1]) == 'd')
    {
        uint32 level = 0;
        for (size_t i = digitsBegin; i < name.size(); ++i)
            level = level * 10u + static_cast<uint32>(name[i] - '0');
        return {name.substr(0, digitsBegin - 4), level, true};
    }
    return {name, 0u, false};
}

// RGBA is supported per owned level. Other optional streams still lack an
// aligned per-level payload and must not be silently discarded.
bool HasOptionalStreams(const Mesh& mesh)
{
    return mesh.HasTexCoords1() || mesh.IsSkinned() || mesh.HasMorphTargets() ||
           !mesh.ExtraTexCoords.empty();
}

bool CompatibleColours(const Mesh& base, const Mesh& level)
{
    return base.HasValidLODColor0() && level.HasValidLODColor0() &&
           base.Color0.empty() == level.Color0.empty();
}

// Authored chains have no meshopt error; threshold derivation keys on explicit
// provenance (Mesh::HasAuthoredLODs() -> DeriveLODThresholds' authoredChain) to
// reproduce the default descending table (amendment #13). Keep the parallel
// error/sloppy arrays filled with descriptive markers (no metric, not meshopt-
// simplified quality) so every ExtraLODs consumer sees aligned arrays.
void SetAuthoredDefaultThresholds(Mesh& mesh)
{
    const size_t levels = mesh.ExtraLODs.size();
    mesh.ExtraLODErrors.assign(levels, 0.0f);
    mesh.ExtraLODSloppy.assign(levels, uint8{1});
    // Every authored-chain assembly funnels through here — stamp the explicit
    // provenance bit (generated own-vertex shells never take this path).
    mesh.AuthoredLODs = true;
}

} // namespace

uint32 ConsumeLodSuffixFamilies(Vector<Mesh>& meshes)
{
    // Group submeshes by case-folded base name, preserving discovery order so the
    // assembled model keeps a stable submesh layout.
    struct Family {
        std::optional<size_t> Base;                  // index of LOD0 (suffixless or _LOD0)
        std::unordered_map<uint32, size_t> Siblings; // level (>=1) -> mesh index
        bool SawDuplicateBase = false;
        std::string BaseName;                        // for the orphan-warn message
    };
    std::unordered_map<StringId, Family> families;
    std::vector<StringId> order;

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        const LodSuffix suffix = ParseLodSuffix(meshes[i].Name);
        const StringId baseKey = Components::HashMeshName(suffix.Base);
        auto it = families.find(baseKey);
        if (it == families.end())
        {
            it = families.emplace(baseKey, Family{}).first;
            it->second.BaseName = std::string(suffix.Base);
            order.push_back(baseKey);
        }
        Family& fam = it->second;
        if (!suffix.HasSuffix || suffix.Level == 0u)
        {
            if (fam.Base) fam.SawDuplicateBase = true;
            else fam.Base = i;
        }
        else
        {
            // First sibling at a given level wins; a duplicate is ignored (stays
            // standalone), which the removal set below leaves untouched.
            fam.Siblings.emplace(suffix.Level, i);
        }
    }

    std::vector<char> consumed(meshes.size(), 0);
    uint32 consumedCount = 0;

    for (StringId baseKey : order)
    {
        Family& fam = families[baseKey];
        if (!fam.Base)
        {
            if (!fam.Siblings.empty())
                Logger::Log::Warning(
                    "AuthoredLOD: _LOD sibling(s) for base '{}' have no LOD0/base submesh; "
                    "leaving them standalone.", fam.BaseName);
            continue;
        }
        if (fam.Siblings.empty())
            continue; // base with no siblings — nothing to consume

        Mesh& base = meshes[*fam.Base];

        // Dual-tag priority (amendment #10): a higher-priority source (glTF
        // MSFT_lod) runs first and may already have assembled this base's chain.
        // The accepted resolution is MSFT_lod wins — never clobber an existing
        // authored chain with the _LOD suffix pass. Its consumed siblings are
        // already gone; any that remain are left standalone.
        if (base.HasAuthoredLODs())
        {
            Logger::Log::Warning(
                "AuthoredLOD: '{}' already has an authored LOD chain (higher-priority "
                "source); leaving its _LOD sibling(s) standalone.", base.Name);
            continue;
        }

        // Contiguous levels 1..k, capped at the row's LOD budget.
        std::vector<size_t> chain;
        for (uint32 level = 1u; level <= kMaxExtraLods; ++level)
        {
            auto s = fam.Siblings.find(level);
            if (s == fam.Siblings.end())
                break;
            chain.push_back(s->second);
        }
        if (chain.empty())
            continue;

        const bool hasGapOrphan = fam.Siblings.size() > chain.size();
        if (hasGapOrphan)
            Logger::Log::Warning(
                "AuthoredLOD: '{}' has non-contiguous _LOD levels; consuming LOD1..{} "
                "and leaving the rest standalone.",
                base.Name, chain.size());
        if (fam.SawDuplicateBase)
            Logger::Log::Warning(
                "AuthoredLOD: '{}' has both a bare base and an _LOD0 submesh; using the "
                "bare base as LOD0.", base.Name);

        // Refuse the whole family if any member carries optional streams the C1
        // upload path would drop — leave every member standalone.
        bool anyOptional = HasOptionalStreams(base) || !base.HasValidLODColor0();
        for (size_t idx : chain)
            anyOptional = anyOptional || HasOptionalStreams(meshes[idx]) ||
                          !CompatibleColours(base, meshes[idx]) ||
                          meshes[idx].MaterialIndex != base.MaterialIndex;
        if (anyOptional)
        {
            Logger::Log::Warning(
                "AuthoredLOD: '{}' _LOD chain carries optional vertex streams "
                "(uv1/skin/morph/extra-uv), invalid/inconsistent RGBA, or mismatched materials; "
                "leaving levels standalone.", base.Name);
            continue;
        }

        base.ExtraLODVertices.resize(chain.size());
        base.ExtraLODColor0.clear();
        if (!base.Color0.empty()) base.ExtraLODColor0.resize(chain.size());
        base.ExtraLODs.resize(chain.size());
        for (size_t k = 0; k < chain.size(); ++k)
        {
            Mesh& sibling = meshes[chain[k]];
            base.ExtraLODVertices[k] = std::move(sibling.Vertices);
            if (!base.Color0.empty()) base.ExtraLODColor0[k] = std::move(sibling.Color0);
            base.ExtraLODs[k] = std::move(sibling.Indices);
            consumed[chain[k]] = 1;
            ++consumedCount;
        }
        SetAuthoredDefaultThresholds(base);

        // Strip a trailing "_LOD0" (or "_LOD") off the exposed base name so a
        // MeshRenderer authored to the bare base name resolves to this submesh.
        const LodSuffix baseSuffix = ParseLodSuffix(base.Name);
        if (baseSuffix.HasSuffix)
            base.Name = std::string(baseSuffix.Base);
    }

    if (consumedCount == 0)
        return 0;

    Vector<Mesh> kept;
    kept.reserve(meshes.size() - consumedCount);
    for (size_t i = 0; i < meshes.size(); ++i)
        if (!consumed[i])
            kept.push_back(std::move(meshes[i]));
    meshes = std::move(kept);
    return consumedCount;
}

std::string EncodeLodSlotValue(std::string_view path, const GUID& guid)
{
    if (guid.IsNull() && path.empty())
        return {};
    return Scene::FormatAssetRef(path, guid.IsNull() ? std::string_view{} : guid.ToString());
}

GUID DecodeLodSlotValue(std::string_view value, std::string* outPath)
{
    if (outPath) outPath->clear();
    if (value.empty())
        return GUID::Null();

    Scene::SceneValue parsed;
    if (!Scene::ParseValue(value, parsed) || parsed.Kind != Scene::SceneValueKind::AssetRef)
        return GUID::Null();

    if (outPath)
        *outPath = std::string(Scene::AssetRefPath(parsed));
    const std::string_view guidText = Scene::AssetRefGuid(parsed);
    if (guidText.empty())
        return GUID::Null();
    GUID guid(std::string{guidText});
    return guid;
}

GUID LoadLodSlotRef(const AssetRegistry& registry,
                    const std::filesystem::path& assetPath, uint32 slot)
{
    if (assetPath.empty() || slot < 1u || slot > kMaxLodSlots)
        return GUID::Null();
    std::string value;
    if (!registry.TryGetMetaValue(assetPath, kLodSlotKeys[slot - 1u], value))
        return GUID::Null();
    return DecodeLodSlotValue(value);
}

bool SaveLodSlotRef(AssetRegistry& registry, const std::filesystem::path& assetPath,
                    uint32 slot, const GUID& guid, std::string_view path)
{
    if (assetPath.empty() || slot < 1u || slot > kMaxLodSlots)
        return false;
    return registry.SetMetaValue(assetPath, kLodSlotKeys[slot - 1u],
                                 EncodeLodSlotValue(path, guid));
}

uint32 AppendSlotLevelByName(Vector<Mesh>& target, const Vector<Mesh>& slotMeshes,
                             uint32 level, const Vector<uint8>& inFileAuthored,
                             std::string_view context)
{
    if (level < 1u || level > kMaxExtraLods || slotMeshes.empty())
        return 0;
    const size_t levelIdx = static_cast<size_t>(level) - 1u;

    // Precompute the slot submesh name hashes for exact and folded matching.
    std::vector<StringId> slotExact(slotMeshes.size());
    std::vector<StringId> slotFolded(slotMeshes.size());
    for (size_t j = 0; j < slotMeshes.size(); ++j)
    {
        slotExact[j] = Components::HashMeshName(slotMeshes[j].Name);
        slotFolded[j] = Components::HashMeshName(Components::FoldMeshLodSuffix(slotMeshes[j].Name));
    }

    uint32 appended = 0;
    for (size_t mi = 0; mi < target.size(); ++mi)
    {
        Mesh& mesh = target[mi];
        // Slots layer UNDER in-file _LOD chains: a submesh authored at parse time is
        // never touched by a slot, at ANY level (extending it would splice slot
        // geometry onto an in-file chain — mixed provenance). The caller flags these
        // via `inFileAuthored` (a live HasAuthoredLODs() check cannot distinguish an
        // in-file chain from one this pass just slot-built) and issues the one warn.
        if (mi < inFileAuthored.size() && inFileAuthored[mi])
            continue;
        // Otherwise the slot chain must stay contiguous: a submesh at exactly
        // `level-1` slot-built levels is the only one eligible to receive level `k`.
        if (mesh.ExtraLODs.size() != levelIdx)
            continue;

        const StringId want = Components::HashMeshName(mesh.Name);
        int match = -1;
        for (size_t j = 0; j < slotMeshes.size(); ++j)
            if (slotExact[j] == want) { match = static_cast<int>(j); break; }
        if (match < 0)
            for (size_t j = 0; j < slotMeshes.size(); ++j)
                if (slotFolded[j] == want) { match = static_cast<int>(j); break; }
        // Single-submesh convenience: a lone slot mesh maps to a lone target mesh
        // even when names differ (the common one-mesh prop + one-mesh LOD case).
        if (match < 0 && target.size() == 1u && slotMeshes.size() == 1u)
            match = 0;
        if (match < 0)
        {
            Logger::Log::Warning(
                "AuthoredLOD[{}]: submesh '{}' has no name match in the LOD{} slot asset; "
                "skipping its level.", context, mesh.Name, level);
            continue;
        }

        const Mesh& src = slotMeshes[static_cast<size_t>(match)];
        if (HasOptionalStreams(mesh) || HasOptionalStreams(src) || !CompatibleColours(mesh, src))
        {
            Logger::Log::Warning(
                "AuthoredLOD[{}]: submesh '{}' or its LOD{} slot match carries optional "
                "vertex streams or invalid/inconsistent RGBA; skipping its level.",
                context, mesh.Name, level);
            continue;
        }

        mesh.ExtraLODVertices.push_back(src.Vertices);
        if (!mesh.Color0.empty()) mesh.ExtraLODColor0.push_back(src.Color0);
        mesh.ExtraLODs.push_back(src.Indices);
        SetAuthoredDefaultThresholds(mesh);
        ++appended;
    }
    return appended;
}

uint64 FoldLodSlotSource(uint64 running, const GUID& slotGuid, uint64 slotContentHash)
{
    constexpr uint64 kFnvPrime = 1099511628211ull;
    const auto mix = [&](uint64 v) {
        const auto* b = reinterpret_cast<const uint8*>(&v);
        for (int i = 0; i < 8; ++i) { running ^= b[i]; running *= kFnvPrime; }
    };
    const std::string guidText = slotGuid.ToString();
    for (char c : guidText) { running ^= static_cast<uint8>(c); running *= kFnvPrime; }
    mix(slotContentHash);
    return running;
}

namespace {

// Load a slot model by GUID for its LOD0 geometry. Prefers an already-loaded copy
// (AssetManager::GetAsset, non-blocking); otherwise direct-loads on the calling
// thread (no async job dispatch, mirroring the humanoid-profile precedent in
// ModelAsset::PostLoad) so a background PostLoad worker never blocks on the load
// pool. Returns nullptr (and leaves `owned` empty) when the slot cannot be loaded.
const ModelAsset* LoadSlotModel(AssetManager& am, const GUID& guid,
                                std::shared_ptr<ModelAsset>& owned)
{
    if (SharedPtr<Asset> cached = am.GetAsset(guid))
    {
        // Keep the shared owner alive via `owned`: returning a raw pointer whose
        // only other owner is m_LoadedAssets would dangle if a concurrent unload /
        // hot-reload (the inspector's own UnloadAssetAsync().wait()) evicts it
        // before the caller reads GetMeshes(). A non-ModelAsset cast yields null.
        owned = std::dynamic_pointer_cast<ModelAsset>(cached);
        return owned.get();
    }

    AssetMetadata meta;
    if (!am.GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return nullptr;
    owned = std::make_shared<ModelAsset>(guid, meta.Path);
    if (!owned->Load() || !owned->IsLoaded())
    {
        owned.reset();
        return nullptr;
    }
    return owned.get();
}

} // namespace

LodSlotResolution ResolveExplicitLodSlots(Vector<Mesh>& meshes, AssetManager* assetManager,
                                          const std::filesystem::path& assetPath,
                                          const GUID& selfGuid)
{
    LodSlotResolution result;
    if (!assetManager || assetPath.empty() || meshes.empty())
        return result;

    const std::string context = assetPath.filename().string();

    // In-file _LOD chains (assembled at parse) are authoritative; explicit slots
    // layer under them and never extend them. Snapshot the pre-slot authored state
    // so a level-k append can tell an in-file chain from a slot-built one, and warn
    // once per in-file submesh (only once we know a slot is actually configured).
    Vector<uint8> inFileAuthored(meshes.size(), 0u);
    bool anyInFile = false;
    for (size_t i = 0; i < meshes.size(); ++i)
        if (meshes[i].HasAuthoredLODs()) { inFileAuthored[i] = 1u; anyInFile = true; }
    bool warnedInFile = false;

    for (uint32 slot = 1u; slot <= kMaxLodSlots; ++slot)
    {
        const GUID slotGuid = LoadLodSlotRef(assetManager->GetRegistry(), assetPath, slot);
        if (slotGuid.IsNull())
            break; // slots are contiguous from 1; an unset slot ends the chain

        if (anyInFile && !warnedInFile)
        {
            for (size_t i = 0; i < meshes.size(); ++i)
                if (inFileAuthored[i])
                    Logger::Log::Warning(
                        "AuthoredLOD[{}]: submesh '{}' has an in-file _LOD chain; explicit "
                        "slots layer under it and are ignored for it.", context, meshes[i].Name);
            warnedInFile = true;
        }

        if (slotGuid == selfGuid)
        {
            Logger::Log::Warning(
                "AuthoredLOD[{}]: LOD{} slot references the model itself; skipping.",
                context, slot);
            break;
        }

        std::shared_ptr<ModelAsset> owned;
        const ModelAsset* slotModel = LoadSlotModel(*assetManager, slotGuid, owned);
        if (!slotModel)
        {
            Logger::Log::Warning(
                "AuthoredLOD[{}]: LOD{} slot asset {} could not be loaded; skipping.",
                context, slot, slotGuid.ToString());
            break;
        }

        result.SourceHash =
            FoldLodSlotSource(result.SourceHash, slotGuid, slotModel->GetSourceContentHash());
        result.LevelsAppended +=
            AppendSlotLevelByName(meshes, slotModel->GetMeshes(), slot, inFileAuthored, context);
    }

    return result;
}

bool ParseMsftLodIds(std::string_view extensionJson, Vector<int32>& outIds)
{
    const nlohmann::json doc =
        nlohmann::json::parse(extensionJson.begin(), extensionJson.end(), nullptr,
                              /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return false;
    const auto it = doc.find("ids");
    if (it == doc.end() || !it->is_array() || it->empty())
        return false;

    Vector<int32> ids;
    ids.reserve(it->size());
    for (const auto& element : *it)
    {
        if (!element.is_number_integer() && !element.is_number_unsigned())
            return false;
        ids.push_back(element.get<int32>());
    }
    outIds = std::move(ids);
    return true;
}

bool ParseMsftScreenCoverage(std::string_view extrasJson, Vector<float>& outCoverage)
{
    const nlohmann::json doc =
        nlohmann::json::parse(extrasJson.begin(), extrasJson.end(), nullptr,
                              /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return false;
    const auto it = doc.find("MSFT_screencoverage");
    if (it == doc.end() || !it->is_array() || it->empty())
        return false;

    Vector<float> coverage;
    coverage.reserve(it->size());
    for (const auto& element : *it)
    {
        if (!element.is_number())
            return false;
        coverage.push_back(element.get<float>());
    }
    outCoverage = std::move(coverage);
    return true;
}

namespace {

// True if `name` carries a trailing `_LOD<N>` suffix (dual-tag detection).
bool HasLodSuffix(std::string_view name)
{
    return Components::FoldMeshLodSuffix(name).size() != name.size();
}

// Collect the (not-yet-consumed) submesh indices whose SourceNodeIndex matches
// `node`, in discovery order.
std::vector<size_t> SubmeshesForNode(const Vector<Mesh>& meshes,
                                     const std::vector<char>& consumed, int32 node)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < meshes.size(); ++i)
        if (!consumed[i] && meshes[i].SourceNodeIndex == node)
            out.push_back(i);
    return out;
}

} // namespace

uint32 AssembleMsftLodChains(Vector<Mesh>& meshes, const Vector<MsftLodGroup>& groups,
                             std::string_view context)
{
    if (groups.empty() || meshes.empty())
        return 0;

    std::vector<char> consumed(meshes.size(), 0);
    uint32 consumedCount = 0;

    for (const MsftLodGroup& group : groups)
    {
        const std::vector<size_t> lod0Subs =
            SubmeshesForNode(meshes, consumed, group.Lod0Node);
        if (lod0Subs.empty())
        {
            Logger::Log::Warning(
                "MSFT_lod[{}]: LOD0 node {} owns no submeshes; skipping group.",
                context, group.Lod0Node);
            continue;
        }

        // Cap the chain at the GPU row's LOD budget; lower nodes past it are still
        // consumed (removed) below so they never render standalone.
        const uint32 levelCount =
            std::min<uint32>(static_cast<uint32>(group.LowerNodes.size()), kMaxExtraLods);

        std::vector<std::vector<size_t>> levelSubs(levelCount);
        for (uint32 k = 0; k < levelCount; ++k)
        {
            levelSubs[k] = SubmeshesForNode(meshes, consumed, group.LowerNodes[k]);
            if (levelSubs[k].empty())
                Logger::Log::Warning(
                    "MSFT_lod[{}]: LOD{} node {} owns no submeshes (out-of-range id or "
                    "empty node); levels from here are unavailable.",
                    context, k + 1u, group.LowerNodes[k]);
        }

        // Coverage is usable when its length matches the group's LOD count
        // (canonical: one per LOD incl. LOD0's cull entry) or one-per-switch.
        const size_t lowerCount = group.LowerNodes.size();
        const bool coverageUsable =
            !group.Coverage.empty() &&
            (group.Coverage.size() == lowerCount + 1u || group.Coverage.size() == lowerCount);
        if (!group.Coverage.empty() && !coverageUsable)
            Logger::Log::Warning(
                "MSFT_lod[{}]: MSFT_screencoverage length {} does not match LOD count {}; "
                "using default thresholds.",
                context, group.Coverage.size(), lowerCount + 1u);

        bool dualTag = false;
        for (size_t s0 : lod0Subs)
            dualTag = dualTag || HasLodSuffix(meshes[s0].Name);

        // Per-level "used" flags so multiple same-material LOD0 submeshes align to
        // distinct lower submeshes in discovery order.
        std::vector<std::vector<char>> used(levelCount);
        for (uint32 k = 0; k < levelCount; ++k)
            used[k].assign(levelSubs[k].size(), 0);

        for (size_t s0 : lod0Subs)
        {
            Mesh& base = meshes[s0];

            // Align a contiguous chain by MaterialIndex; the first unused lower
            // submesh at each level with the base's material wins. A missing match
            // ends the chain (levels stay contiguous from LOD1).
            std::vector<size_t> chain;
            for (uint32 k = 0; k < levelCount; ++k)
            {
                int match = -1;
                for (size_t j = 0; j < levelSubs[k].size(); ++j)
                {
                    if (used[k][j])
                        continue;
                    if (meshes[levelSubs[k][j]].MaterialIndex == base.MaterialIndex)
                    {
                        match = static_cast<int>(j);
                        break;
                    }
                }
                if (match < 0)
                    break;
                used[k][static_cast<size_t>(match)] = 1;
                chain.push_back(levelSubs[k][static_cast<size_t>(match)]);
            }

            if (chain.empty())
            {
                Logger::Log::Warning(
                    "MSFT_lod[{}]: submesh '{}' (material {}) has no material-aligned LOD1; "
                    "left LOD0-only.",
                    context, base.Name, base.MaterialIndex);
                continue;
            }
            if (chain.size() < levelCount)
                Logger::Log::Warning(
                    "MSFT_lod[{}]: submesh '{}' aligned {} of {} authored levels (a coarser "
                    "LOD dropped its material); keeping the aligned prefix.",
                    context, base.Name, chain.size(), levelCount);

            // Refuse unsupported streams or inconsistent RGBA per material chain,
            // but still remove lower submeshes because they are LOD nodes.
            bool anyOptional = HasOptionalStreams(base) || !base.HasValidLODColor0();
            for (size_t idx : chain)
                anyOptional = anyOptional || HasOptionalStreams(meshes[idx]) ||
                              !CompatibleColours(base, meshes[idx]);
            if (anyOptional)
            {
                Logger::Log::Warning(
                    "MSFT_lod[{}]: submesh '{}' chain carries optional vertex streams "
                    "(uv1/skin/morph/extra-uv) or invalid/inconsistent RGBA; "
                    "left LOD0-only.",
                    context, base.Name);
                for (size_t idx : chain)
                    if (!consumed[idx]) { consumed[idx] = 1; ++consumedCount; }
                continue;
            }

            base.ExtraLODVertices.resize(chain.size());
            base.ExtraLODColor0.clear();
            if (!base.Color0.empty()) base.ExtraLODColor0.resize(chain.size());
            base.ExtraLODs.resize(chain.size());
            for (size_t k = 0; k < chain.size(); ++k)
            {
                Mesh& src = meshes[chain[k]];
                base.ExtraLODVertices[k] = std::move(src.Vertices);
                if (!base.Color0.empty()) base.ExtraLODColor0[k] = std::move(src.Color0);
                base.ExtraLODs[k]        = std::move(src.Indices);
                consumed[chain[k]]       = 1;
                ++consumedCount;
            }
            SetAuthoredDefaultThresholds(base);
            if (coverageUsable)
                base.ExtraLODCoverage.assign(group.Coverage.begin(),
                                             group.Coverage.begin() + chain.size());

            // Strip a trailing `_LOD<N>` off the exposed name so a MeshRenderer
            // authored to the bare base binds this submesh (uniform with the _LOD
            // suffix pass). A well-formed MSFT LOD0 name has no suffix (no-op).
            const std::string_view folded = Components::FoldMeshLodSuffix(base.Name);
            if (folded.size() != base.Name.size())
                base.Name = std::string(folded);
        }

        // Remove every remaining lower-detail submesh (unmatched materials, empty
        // matches, and levels beyond the row budget): a LOD node never stands alone.
        for (size_t k = 0; k < group.LowerNodes.size(); ++k)
        {
            const std::vector<size_t> subs =
                SubmeshesForNode(meshes, consumed, group.LowerNodes[k]);
            for (size_t idx : subs)
                if (!consumed[idx]) { consumed[idx] = 1; ++consumedCount; }
        }

        if (dualTag)
            Logger::Log::Warning(
                "MSFT_lod[{}]: chain members also carry _LOD name suffixes; MSFT_lod takes "
                "priority (the _LOD suffix pass sees an already-consumed chain).",
                context);
    }

    if (consumedCount == 0)
        return 0;

    Vector<Mesh> kept;
    kept.reserve(meshes.size() - consumedCount);
    for (size_t i = 0; i < meshes.size(); ++i)
        if (!consumed[i])
            kept.push_back(std::move(meshes[i]));
    meshes = std::move(kept);
    return consumedCount;
}

float FbxLodSwitchCoverage(float switchDistance, bool relative, float worldRadius,
                           float unitScale)
{
    constexpr float kInvalid = -1.0f;
    if (relative)
    {
        // Screen-size percentage in (0,100]; used directly as a coverage fraction.
        if (switchDistance <= 0.0f || switchDistance > 100.0f)
            return kInvalid;
        return switchDistance / 100.0f;
    }
    // World distance: mirror the runtime metric coverage = worldRadius * projScaleY /
    // dist. worldRadius is already in engine units; the FBX distance is not, so scale
    // it. projScaleY = 1/tan(fovY/2) at the reference FOV.
    const float engineDistance = switchDistance * unitScale;
    if (engineDistance <= 0.0f || worldRadius <= 0.0f)
        return kInvalid;
    constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
    const float projScaleY = 1.0f / std::tan(kLodRefVerticalFovDegrees * 0.5f * kDegToRad);
    return worldRadius * projScaleY / engineDistance;
}

namespace {

// Half-diagonal of LOD0's AABB — the LOD0 reference radius the GPU row's
// thresholds are derived from (MeshGPURegistry: entry.lodReferenceRadius), so
// authored world-distance coverages land in the units ge_SelectLOD compares.
// NOT the row's boundingRadius: that is the culling envelope over every level,
// and the scatter divides it back out before comparing against a threshold.
float MeshBoundRadius(const Mesh& mesh)
{
    const float dx = mesh.MaxBounds[0] - mesh.MinBounds[0];
    const float dy = mesh.MaxBounds[1] - mesh.MinBounds[1];
    const float dz = mesh.MaxBounds[2] - mesh.MinBounds[2];
    return 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

FbxLodGroupOrder ResolveFbxLodGroupOrder(const Vector<FbxLodGroupChild>& children,
                                         bool relativeDistances)
{
    const size_t count = children.size();
    FbxLodGroupOrder result;
    Vector<uint32>& order = result.Levels;
    if (count < 2u)
        return result;

    const auto allDistinct = [](Vector<float> values) {
        std::sort(values.begin(), values.end());
        return std::adjacent_find(values.begin(), values.end()) == values.end();
    };

    Vector<float> distances;
    distances.reserve(count);
    bool finiteDistances = true;
    for (const FbxLodGroupChild& child : children)
    {
        finiteDistances = finiteDistances && std::isfinite(child.SwitchDistance);
        distances.push_back(child.SwitchDistance);
    }
    Vector<uint32> byDistance;
    if (finiteDistances && allDistinct(distances))
    {
        byDistance.resize(count);
        std::iota(byDistance.begin(), byDistance.end(), 0u);
        std::sort(byDistance.begin(), byDistance.end(), [&](uint32 a, uint32 b) {
            return relativeDistances ? distances[a] > distances[b]
                                     : distances[a] < distances[b];
        });
    }

    Vector<float> levels;
    levels.reserve(count);
    bool numberedNames = true;
    for (const FbxLodGroupChild& child : children)
    {
        const LodSuffix suffix = ParseLodSuffix(child.Name);
        numberedNames = numberedNames && suffix.HasSuffix;
        levels.push_back(static_cast<float>(suffix.Level));
    }
    Vector<uint32> byName;
    if (numberedNames && allDistinct(levels))
    {
        byName.resize(count);
        std::iota(byName.begin(), byName.end(), 0u);
        std::sort(byName.begin(), byName.end(),
                  [&](uint32 a, uint32 b) { return levels[a] < levels[b]; });
    }

    // Names win a disagreement. An exporter that writes the children out of order
    // writes its positional thresholds around that same order, so the distances
    // agree with the file's mistake; the `_LOD<N>` on the node is the one part a
    // person typed. When they agree, or only one rule is available, that rule
    // stands — and only a distance order actually chosen may seed switch coverages.
    if (!byName.empty() && !byDistance.empty() && byName != byDistance)
    {
        order = std::move(byName);
        result.OverriddenDistanceOrder = std::move(byDistance);
        return result;
    }
    if (!byDistance.empty())
    {
        order = std::move(byDistance);
        result.FromSwitchDistances = true;
        return result;
    }
    order = std::move(byName);
    return result;
}

uint32 AssembleFbxLodGroupChains(Vector<Mesh>& meshes, const Vector<FbxLodGroup>& groups,
                                 std::string_view context)
{
    if (groups.empty() || meshes.empty())
        return 0;

    std::vector<char> consumed(meshes.size(), 0);
    uint32 consumedCount = 0;

    for (const FbxLodGroup& group : groups)
    {
        const std::vector<size_t> lod0Subs =
            SubmeshesForNode(meshes, consumed, group.Lod0Node);
        if (lod0Subs.empty())
        {
            Logger::Log::Warning(
                "FBX LOD group[{}]: LOD0 node {} owns no submeshes; skipping group.",
                context, group.Lod0Node);
            continue;
        }

        // Cap the chain at the GPU row's LOD budget; lower nodes past it are still
        // consumed (removed) below so they never render standalone.
        const uint32 levelCount =
            std::min<uint32>(static_cast<uint32>(group.LowerNodes.size()), kMaxExtraLods);

        std::vector<std::vector<size_t>> levelSubs(levelCount);
        for (uint32 k = 0; k < levelCount; ++k)
        {
            levelSubs[k] = SubmeshesForNode(meshes, consumed, group.LowerNodes[k]);
            if (levelSubs[k].empty())
                Logger::Log::Warning(
                    "FBX LOD group[{}]: LOD{} node {} owns no submeshes; levels from here "
                    "are unavailable.",
                    context, k + 1u, group.LowerNodes[k]);
        }

        bool dualTag = false;
        for (size_t s0 : lod0Subs)
            dualTag = dualTag || HasLodSuffix(meshes[s0].Name);

        // Per-level "used" flags so multiple same-material LOD0 submeshes align to
        // distinct lower submeshes in discovery order (multi-material LOD models).
        std::vector<std::vector<char>> used(levelCount);
        for (uint32 k = 0; k < levelCount; ++k)
            used[k].assign(levelSubs[k].size(), 0);

        for (size_t s0 : lod0Subs)
        {
            Mesh& base = meshes[s0];

            // Align a contiguous chain by MaterialIndex; the first unused lower submesh
            // at each level with the base's material wins. Authored LODs share LOD0's
            // draw state (batch key is (material, mesh)), so a differing material is not
            // an authored LOD of this submesh — a missing match ends the chain.
            std::vector<size_t> chain;
            for (uint32 k = 0; k < levelCount; ++k)
            {
                int match = -1;
                for (size_t j = 0; j < levelSubs[k].size(); ++j)
                {
                    if (used[k][j])
                        continue;
                    if (meshes[levelSubs[k][j]].MaterialIndex == base.MaterialIndex)
                    {
                        match = static_cast<int>(j);
                        break;
                    }
                }
                if (match < 0)
                    break;
                used[k][static_cast<size_t>(match)] = 1;
                chain.push_back(levelSubs[k][static_cast<size_t>(match)]);
            }

            if (chain.empty())
            {
                Logger::Log::Warning(
                    "FBX LOD group[{}]: submesh '{}' (material {}) has no material-aligned "
                    "LOD1; left LOD0-only.",
                    context, base.Name, base.MaterialIndex);
                continue;
            }
            if (chain.size() < levelCount)
                Logger::Log::Warning(
                    "FBX LOD group[{}]: submesh '{}' aligned {} of {} authored levels (a "
                    "coarser LOD dropped its material); keeping the aligned prefix.",
                    context, base.Name, chain.size(), levelCount);

            // Refuse unsupported streams, invalid RGBA, or non-triangle topology;
            // still remove lower levels because they are LOD nodes.
            bool refused = HasOptionalStreams(base) ||
                           !base.HasValidLODColor0() ||
                           base.PrimitiveTopology != MeshPrimitiveTopology::Triangles;
            for (size_t idx : chain)
                refused = refused || HasOptionalStreams(meshes[idx]) ||
                          !CompatibleColours(base, meshes[idx]) ||
                          meshes[idx].PrimitiveTopology != MeshPrimitiveTopology::Triangles;
            if (refused)
            {
                Logger::Log::Warning(
                    "FBX LOD group[{}]: submesh '{}' chain carries unsupported vertex streams, "
                    "invalid/inconsistent RGBA, or non-triangle topology; left "
                    "LOD0-only.",
                    context, base.Name);
                for (size_t idx : chain)
                    if (!consumed[idx]) { consumed[idx] = 1; ++consumedCount; }
                continue;
            }

            base.ExtraLODVertices.resize(chain.size());
            base.ExtraLODColor0.clear();
            if (!base.Color0.empty()) base.ExtraLODColor0.resize(chain.size());
            base.ExtraLODs.resize(chain.size());
            for (size_t k = 0; k < chain.size(); ++k)
            {
                Mesh& src = meshes[chain[k]];
                base.ExtraLODVertices[k] = std::move(src.Vertices);
                if (!base.Color0.empty()) base.ExtraLODColor0[k] = std::move(src.Color0);
                base.ExtraLODs[k]        = std::move(src.Indices);
                consumed[chain[k]]       = 1;
                ++consumedCount;
            }
            SetAuthoredDefaultThresholds(base);

            // Distances -> coverage against THIS submesh's bound radius. A single
            // non-positive / out-of-range switch distance (e.g. a binary export that
            // dropped its thresholds) drops the group's coverage to the default table,
            // as does a group that carries no distance for every consumed level — the
            // loader leaves them out when the level names, not the distances, decided
            // the order.
            const float radius = MeshBoundRadius(base);
            Vector<float> coverage;
            coverage.reserve(chain.size());
            bool coverageValid = group.SwitchDistances.size() >= chain.size();
            for (size_t k = 0; coverageValid && k < chain.size(); ++k)
            {
                const float cov = FbxLodSwitchCoverage(group.SwitchDistances[k],
                                                       group.RelativeDistances, radius,
                                                       group.UnitScale);
                if (cov < 0.0f) { coverageValid = false; break; }
                coverage.push_back(cov);
            }
            if (coverageValid && !coverage.empty())
                base.ExtraLODCoverage = std::move(coverage);

            // Strip a trailing `_LOD<N>` off the exposed name so a MeshRenderer authored
            // to the bare base binds this submesh (uniform with the other sources). A
            // well-formed LOD-group child name has no suffix (no-op).
            const std::string_view folded = Components::FoldMeshLodSuffix(base.Name);
            if (folded.size() != base.Name.size())
                base.Name = std::string(folded);
        }

        // Remove every remaining lower-detail submesh (unmatched materials, empty
        // matches, and levels beyond the row budget): a LOD node never stands alone.
        for (size_t k = 0; k < group.LowerNodes.size(); ++k)
        {
            const std::vector<size_t> subs =
                SubmeshesForNode(meshes, consumed, group.LowerNodes[k]);
            for (size_t idx : subs)
                if (!consumed[idx]) { consumed[idx] = 1; ++consumedCount; }
        }

        if (dualTag)
            Logger::Log::Warning(
                "FBX LOD group[{}]: chain members also carry _LOD name suffixes; the LOD "
                "group takes priority (the _LOD suffix pass sees an already-consumed chain).",
                context);
    }

    if (consumedCount == 0)
        return 0;

    Vector<Mesh> kept;
    kept.reserve(meshes.size() - consumedCount);
    for (size_t i = 0; i < meshes.size(); ++i)
        if (!consumed[i])
            kept.push_back(std::move(meshes[i]));
    meshes = std::move(kept);
    return consumedCount;
}

} // namespace GameEngine
