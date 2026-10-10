#include "Engine/Rendering/Pipeline/PipelineEnumTables.h"

#include "Logger/Logger.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace GameEngine::Engine::Renderer::Pipeline
{
namespace R = ::GameEngine::Rendering;

namespace
{
// TextureFormat: mirrors the old MaterializeTexture ladder verbatim, including
// the reverse-Z remap of the legacy d24 name onto D32_SFLOAT_S8_UINT.
constexpr EnumTableEntry<R::TextureFormat> kTextureFormatEntries[] = {
    {"rgba8_unorm", R::TextureFormat::RGBA8_UNORM},
    {"r8g8b8a8_unorm", R::TextureFormat::RGBA8_UNORM},
    {"rgba8_srgb", R::TextureFormat::RGBA8_SRGB},
    {"r8g8b8a8_srgb", R::TextureFormat::RGBA8_SRGB},
    {"bgra8_unorm", R::TextureFormat::BGRA8_UNORM},
    {"bgra8_srgb", R::TextureFormat::BGRA8_SRGB},
    {"r16g16b16a16_float", R::TextureFormat::R16G16B16A16_FLOAT},
    {"rgba16_float", R::TextureFormat::R16G16B16A16_FLOAT},
    {"rgba16f", R::TextureFormat::R16G16B16A16_FLOAT},
    {"r16g16b16a16_unorm", R::TextureFormat::R16G16B16A16_UNORM},
    {"rgba16_unorm", R::TextureFormat::R16G16B16A16_UNORM},
    {"r32g32b32a32_float", R::TextureFormat::R32G32B32A32_FLOAT},
    {"rgba32_float", R::TextureFormat::R32G32B32A32_FLOAT},
    {"rgba32f", R::TextureFormat::R32G32B32A32_FLOAT},
    {"r32g32_float", R::TextureFormat::R32G32_FLOAT},
    {"rg32_float", R::TextureFormat::R32G32_FLOAT},
    {"rg32f", R::TextureFormat::R32G32_FLOAT},
    {"r16_float", R::TextureFormat::R16_FLOAT},
    {"r16f", R::TextureFormat::R16_FLOAT},
    {"r16g16_float", R::TextureFormat::R16G16_FLOAT},
    {"rg16_float", R::TextureFormat::R16G16_FLOAT},
    {"rg16f", R::TextureFormat::R16G16_FLOAT},
    {"r32_float", R::TextureFormat::R32_FLOAT},
    {"r32f", R::TextureFormat::R32_FLOAT},
    {"r8_unorm", R::TextureFormat::R8_UNORM},
    {"r8g8_unorm", R::TextureFormat::R8G8_UNORM},
    {"rg8_unorm", R::TextureFormat::R8G8_UNORM},
    {"r8_uint", R::TextureFormat::R8_UINT},
    {"r8_sint", R::TextureFormat::R8_SINT},
    {"r8g8_uint", R::TextureFormat::R8G8_UINT},
    {"rg8_uint", R::TextureFormat::R8G8_UINT},
    {"r8g8_sint", R::TextureFormat::R8G8_SINT},
    {"rg8_sint", R::TextureFormat::R8G8_SINT},
    {"d24_unorm_s8_uint", R::TextureFormat::D32_SFLOAT_S8_UINT},
    {"d32_float", R::TextureFormat::D32_FLOAT},
    {"d32_sfloat_s8_uint", R::TextureFormat::D32_SFLOAT_S8_UINT},
};

// TextureUsage: per-string flag; callers OR the looked-up flags together.
constexpr EnumTableEntry<R::TextureUsage> kTextureUsageEntries[] = {
    {"shadersource", R::TextureUsage::ShaderResource},
    {"shaderresource", R::TextureUsage::ShaderResource},
    {"sampled", R::TextureUsage::ShaderResource},
    {"unorderedaccess", R::TextureUsage::UnorderedAccess},
    {"uav", R::TextureUsage::UnorderedAccess},
    {"storage", R::TextureUsage::UnorderedAccess},
    {"storagewrite", R::TextureUsage::UnorderedAccess},
    {"storage_write", R::TextureUsage::UnorderedAccess},
    {"rendertarget", R::TextureUsage::RenderTarget},
    {"colorattachment", R::TextureUsage::RenderTarget},
    {"depthstencil", R::TextureUsage::DepthStencil},
    {"depth", R::TextureUsage::DepthStencil},
    {"copysource", R::TextureUsage::TransferSrc},
    {"copysrc", R::TextureUsage::TransferSrc},
    {"copydestination", R::TextureUsage::TransferDst},
    {"copydst", R::TextureUsage::TransferDst},
};

// BufferUsage: per-string flag; callers OR the looked-up flags together.
constexpr EnumTableEntry<R::BufferUsage> kBufferUsageEntries[] = {
    {"unorderedaccess", R::BufferUsage::Storage},
    {"uav", R::BufferUsage::Storage},
    {"storage", R::BufferUsage::Storage},
    {"shadersource", R::BufferUsage::Storage},
    {"shaderresource", R::BufferUsage::Storage},
    {"srv", R::BufferUsage::Storage},
    {"sampled", R::BufferUsage::Storage},
    {"constantbuffer", R::BufferUsage::Uniform},
    {"cb", R::BufferUsage::Uniform},
    {"cbv", R::BufferUsage::Uniform},
    {"indirectargs", R::BufferUsage::Indirect},
    {"indirect", R::BufferUsage::Indirect},
    {"vertexbuffer", R::BufferUsage::Vertex},
    {"vb", R::BufferUsage::Vertex},
    {"indexbuffer", R::BufferUsage::Index},
    {"ib", R::BufferUsage::Index},
    {"copysource", R::BufferUsage::TransferSrc},
    {"copysrc", R::BufferUsage::TransferSrc},
    {"copydestination", R::BufferUsage::TransferDst},
    {"copydst", R::BufferUsage::TransferDst},
};

constexpr EnumTableEntry<R::BufferMemoryUsage> kBufferMemoryUsageEntries[] = {
    {"auto", R::BufferMemoryUsage::Auto},
    {"devicelocal", R::BufferMemoryUsage::DeviceLocal},
    {"upload", R::BufferMemoryUsage::Upload},
    {"readback", R::BufferMemoryUsage::Readback},
};

constexpr EnumTableEntry<BlendMode> kBlendModeEntries[] = {
    {"disabled", BlendMode::Disabled},
    {"none", BlendMode::Disabled},
    {"off", BlendMode::Disabled},
    {"alpha", BlendMode::Alpha},
    {"additive", BlendMode::Additive},
    {"add", BlendMode::Additive},
};
} // namespace

const EnumStringTable<R::TextureFormat>& TextureFormatTable()
{
    static const EnumStringTable<R::TextureFormat> kTable{kTextureFormatEntries};
    return kTable;
}

const EnumStringTable<R::TextureUsage>& TextureUsageTable()
{
    static const EnumStringTable<R::TextureUsage> kTable{kTextureUsageEntries};
    return kTable;
}

const EnumStringTable<R::BufferUsage>& BufferUsageTable()
{
    static const EnumStringTable<R::BufferUsage> kTable{kBufferUsageEntries};
    return kTable;
}

const EnumStringTable<R::BufferMemoryUsage>& BufferMemoryUsageTable()
{
    static const EnumStringTable<R::BufferMemoryUsage> kTable{kBufferMemoryUsageEntries};
    return kTable;
}

const EnumStringTable<BlendMode>& BlendModeTable()
{
    static const EnumStringTable<BlendMode> kTable{kBlendModeEntries};
    return kTable;
}

void WarnUnknownEnumOnce(std::string_view category, std::string_view value,
                         std::span<const std::string_view> validNames)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;

    std::string key(category);
    key += '|';
    key.append(value);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(key).second)
            return;
    }

    std::string names;
    for (size_t i = 0; i < validNames.size(); ++i)
    {
        if (i != 0)
            names += ", ";
        names.append(validNames[i]);
    }
    Logger::Log::Warning("[PipelineEnumTables] unknown {} '{}' — using lenient default. Valid: {}",
                         std::string(category), std::string(value), names);
}

} // namespace GameEngine::Engine::Renderer::Pipeline
