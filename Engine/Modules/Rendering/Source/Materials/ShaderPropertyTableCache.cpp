#include "Rendering/Materials/ShaderPropertyTableCache.h"

#include <chrono>
#include <fstream>
#include <iterator>

namespace GameEngine::Rendering
{

namespace
{

bool ReadFileText(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

std::string CacheKey(const ShaderPropertyTableInputs& inputs)
{
    return inputs.AdapterPath.generic_string() + "|" + inputs.SurfacePath.generic_string() + "|" +
           inputs.VertexModifierPath.generic_string();
}

} // namespace

ShaderPropertyTableCache& ShaderPropertyTableCache::Instance()
{
    static ShaderPropertyTableCache cache;
    return cache;
}

ShaderPropertyTableCache::FileStamp ShaderPropertyTableCache::Stamp(const std::filesystem::path& path)
{
    FileStamp stamp{};
    if (path.empty())
        return stamp;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
        return stamp;
    stamp.Exists = true;
    stamp.WriteTime = static_cast<int64_t>(time.time_since_epoch().count());
    stamp.Size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    return stamp;
}

std::shared_ptr<const ShaderPropertyTable> ShaderPropertyTableCache::Build(
    const ShaderPropertyTableInputs& inputs)
{
    std::vector<ShaderPropertySource> sources;
    auto add = [&](const std::filesystem::path& path, ShaderPropertyOrigin origin, bool required,
                   ShaderPropertyTable& errors)
    {
        if (path.empty())
            return;
        ShaderPropertySource source{};
        source.File = path.generic_string();
        source.Origin = origin;
        if (!ReadFileText(path, source.Text))
        {
            if (required)
                errors.Errors.push_back({source.File, 0, "could not read the shader to collect its @property declarations"});
            return;
        }
        sources.push_back(std::move(source));
    };

    ShaderPropertyTable readErrors{};
    add(inputs.AdapterPath, ShaderPropertyOrigin::Adapter, true, readErrors);
    add(inputs.SurfacePath, ShaderPropertyOrigin::Surface, true, readErrors);
    add(inputs.VertexModifierPath, ShaderPropertyOrigin::VertexModifier, false, readErrors);
    if (readErrors.Rejected())
        return std::make_shared<const ShaderPropertyTable>(std::move(readErrors));
    return std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable(sources));
}

std::shared_ptr<const ShaderPropertyTable> ShaderPropertyTableCache::Resolve(
    const ShaderPropertyTableInputs& inputs)
{
    const std::string key = CacheKey(inputs);
    const FileStamp adapter = Stamp(inputs.AdapterPath);
    const FileStamp surface = Stamp(inputs.SurfacePath);
    const FileStamp modifier = Stamp(inputs.VertexModifierPath);

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Entries.find(key);
    if (it != m_Entries.end() && it->second.Adapter == adapter && it->second.Surface == surface &&
        it->second.VertexModifier == modifier)
        return it->second.Table;

    Entry entry{adapter, surface, modifier, Build(inputs)};
    auto table = entry.Table;
    m_Entries[key] = std::move(entry);
    return table;
}

} // namespace GameEngine::Rendering
