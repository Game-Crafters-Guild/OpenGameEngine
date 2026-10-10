#include "Particles/Assets/ParticleStackAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Particles/ParticleStackAuthoring.h"
#include "Particles/ParticleStackBinary.h"

#include <fstream>
#include <utility>

namespace GameEngine::Particles
{
namespace
{
constexpr int kAuthoringIndent = 2;

bool WriteBytes(const std::filesystem::path& file, std::span<const char> bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}
} // namespace

ParticleStackAsset::ParticleStackAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::ParticleStack, path)
{
}

bool ParticleStackAsset::Load()
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(GetPath(), bytes))
    {
        m_Diagnostics = {{"stack", "Cannot read " + GetPath().string()}};
        SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(bytes);
}

bool ParticleStackAsset::LoadFromData(const Vector<uint8>& data)
{
    const bool loaded = Adopt(data);
    if (!loaded && m_Compiled)
    {
        // A first load that fails holds no stack; only a reload keeps the last valid one.
        m_Compiled.reset();
    }
    SetState(loaded ? AssetState::Loaded : AssetState::Failed);
    return loaded;
}

void ParticleStackAsset::Unload()
{
    m_Compiled.reset();
    m_Diagnostics.clear();
    SetState(AssetState::Unloaded);
}

bool ParticleStackAsset::ReloadFromData(const Vector<uint8>& data)
{
    // A refused file keeps the last valid stack running; the asset stays loaded.
    return Adopt(data);
}

bool ParticleStackAsset::SetDocument(StackDocument document)
{
    return Adopt(std::make_shared<const StackDocument>(std::move(document)));
}

bool ParticleStackAsset::Save() const
{
    if (!Document())
        return false;
    const std::string text = SerializeParticleStack(*Document(), kAuthoringIndent);
    return WriteBytes(GetPath(), text);
}

bool ParticleStackAsset::CookFile(const std::filesystem::path& file, std::string& error)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(file, bytes))
    {
        error = "Cannot read the particle stack";
        return false;
    }
    StackDocument document;
    std::vector<StackDiagnostic> diagnostics;
    if (!LoadParticleStack(bytes, document, diagnostics))
    {
        error = diagnostics.empty() ? "Invalid particle stack" : diagnostics.front().Path + ": " + diagnostics.front().Message;
        return false;
    }
    std::vector<uint8> cooked;
    if (!WriteParticleStackBinary(document, cooked, error))
        return false;
    if (!WriteBytes(file, {reinterpret_cast<const char*>(cooked.data()), cooked.size()}))
    {
        error = "Cannot write the cooked particle stack";
        return false;
    }
    return true;
}

bool ParticleStackAsset::Adopt(std::span<const uint8> bytes)
{
    StackDocument document;
    std::vector<StackDiagnostic> diagnostics;
    if (!LoadParticleStack(bytes, document, diagnostics))
    {
        m_Diagnostics = std::move(diagnostics);
        return false;
    }
    return Adopt(std::make_shared<const StackDocument>(std::move(document)));
}

bool ParticleStackAsset::Adopt(std::shared_ptr<const StackDocument> document)
{
    std::vector<StackDiagnostic> diagnostics;
    auto compiled = CompileParticleStack(std::move(document), diagnostics);
    m_Diagnostics = std::move(diagnostics);
    if (!compiled)
        return false;
    m_Compiled = std::move(compiled);
    return true;
}

} // namespace GameEngine::Particles
