#pragma once

// ShaderPropertyTableCache: the one place a program's declared-property table is
// built from disk. Registration (MaterialSystem) and composition (ShaderComposer,
// on worker threads) both resolve through it, so the lanes the CPU cache writes
// and the lanes the composed GLSL reads come from the same parse. Entries are
// keyed by the three source files and drop when any of them changes on disk.

#include "Rendering/Materials/ShaderPropertyTable.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace GameEngine::Rendering
{

struct ShaderPropertyTableInputs
{
    std::filesystem::path AdapterPath;        // adapter_forward.glsl (declares the adapter's own reads)
    std::filesystem::path SurfacePath;        // resolved surface file
    std::filesystem::path VertexModifierPath; // resolved vertex modifier, or empty
};

class ShaderPropertyTableCache
{
  public:
    static ShaderPropertyTableCache& Instance();

    // The table for these files: parsed once per (path, mtime, size) triple and
    // shared until one of the files changes. Thread-safe. An unreadable surface
    // yields a table whose Errors name the file.
    std::shared_ptr<const ShaderPropertyTable> Resolve(const ShaderPropertyTableInputs& inputs);

  private:
    struct FileStamp
    {
        int64_t WriteTime = 0;
        uint64_t Size = 0;
        bool Exists = false;
        bool operator==(const FileStamp&) const = default;
    };
    struct Entry
    {
        FileStamp Adapter;
        FileStamp Surface;
        FileStamp VertexModifier;
        std::shared_ptr<const ShaderPropertyTable> Table;
    };

    static FileStamp Stamp(const std::filesystem::path& path);
    static std::shared_ptr<const ShaderPropertyTable> Build(const ShaderPropertyTableInputs& inputs);

    std::mutex m_Mutex;
    std::unordered_map<std::string, Entry> m_Entries;
};

} // namespace GameEngine::Rendering
