#pragma once

// ShaderCompileErrorLog: the editor-facing record of material shader compile
// failures. ShaderCompilationCache reports every runtime compile outcome here
// (success clears, failure upserts), so the editor can surface broken shaders
// the moment they break — without requiring the material to be selected in an
// inspector. Thread-safe: compiles run on workers; the editor polls Version()
// from the UI thread and snapshots on change.
//
// Header-only by design (precedent: AssetReloadInvalidator) — a small
// mutex-guarded vector; its only dependency is AssetCore's path
// normalization, so purge-by-path matches the registry's canonical form.

#include "AssetCore/PathNormalization.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{

class ShaderCompileErrorLog
{
  public:
    struct Entry
    {
        // Registry material name (GetOrCompile's debugName) — the display key.
        std::string MaterialName;
        // Authored surface reference ("my_surface.glsl") — resolves rows to files.
        std::string SurfaceShaderPath;
        std::filesystem::path MaterialAssetPath; // empty for synthetic/headless builds
        std::vector<std::string> Errors;         // raw diagnostic lines (file:line attributed)
        std::chrono::system_clock::time_point When;
    };

    // Upsert keyed by (MaterialName, SurfaceShaderPath): pass variants of one
    // material collapse into a single entry; the latest failure wins.
    void ReportFailure(Entry entry)
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        entry.When = std::chrono::system_clock::now();
        auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                               [&](const Entry& e)
                               {
                                   return e.MaterialName == entry.MaterialName
                                       && e.SurfaceShaderPath == entry.SurfaceShaderPath;
                               });
        if (it != m_Entries.end())
            *it = std::move(entry);
        else
            m_Entries.push_back(std::move(entry));
        ++m_Version;
    }

    void ReportSuccess(const std::string& materialName, const std::string& surfaceShaderPath)
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        const size_t before = m_Entries.size();
        std::erase_if(m_Entries,
                      [&](const Entry& e)
                      {
                          return e.MaterialName == materialName
                              && e.SurfaceShaderPath == surfaceShaderPath;
                      });
        if (m_Entries.size() != before)
            ++m_Version;
    }

    // The material file at `materialPath` no longer exists under that name
    // (renamed or deleted): its entries describe an identity nothing can
    // recompile, so they would otherwise outlive the file forever.
    //
    // Entries carry whatever casing their producer held (the registry's
    // case-folded metadata path on the compile path; on-disk casing from
    // synthetic builds) while purge callers pass the on-disk path, so both
    // sides are reduced to the registry's canonical key at compare time —
    // no producer or caller can regress the match by picking the other form.
    void RemoveMaterialAsset(const std::filesystem::path& materialPath)
    {
        const std::string key = AssetPaths::NormalizeForRegistryKey(materialPath);
        std::lock_guard<std::mutex> lk(m_Mutex);
        const size_t before = m_Entries.size();
        std::erase_if(m_Entries,
                      [&](const Entry& e)
                      { return AssetPaths::NormalizeForRegistryKey(e.MaterialAssetPath) == key; });
        if (m_Entries.size() != before)
            ++m_Version;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Entries.empty())
            return;
        m_Entries.clear();
        ++m_Version;
    }

    uint64_t Version() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_Version;
    }

    size_t Count() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_Entries.size();
    }

    std::vector<Entry> Snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_Entries;
    }

  private:
    mutable std::mutex m_Mutex;
    std::vector<Entry> m_Entries;
    uint64_t m_Version = 0;
};

} // namespace Engine::Renderer
} // namespace GameEngine
