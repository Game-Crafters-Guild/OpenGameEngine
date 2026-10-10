#pragma once

#include "AssetCore/Asset.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {

namespace Rendering { struct ShaderPackage; }

// A reusable shader \"program\" description (.shader).
// This is optional; materials can bind stages directly without a program asset.
struct ShaderStageProgramEntry
{
    std::string source;              // source path (.hlsl/.glsl/.vert/.frag/.comp/...)
    std::string entryPoint = "main"; // meaningful for HLSL; GLSL usually stays \"main\"
    std::vector<std::string> defines;
};

struct ShaderProgramDocument
{
    std::string name;
    // stage key: \"vs\",\"fs\",\"cs\",\"ms\",\"gs\" -> entry
    std::unordered_map<std::string, ShaderStageProgramEntry> stages;
};

class ShaderProgramAsset : public Asset
{
public:
    ShaderProgramAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Shader, path)
    {
    }

    static void SetShaderCacheRoot(std::filesystem::path root) noexcept;
    // Empty until the engine resolves the workspace (<workspace>/.Cache/Shaders).
    static const std::filesystem::path& GetShaderCacheRoot();

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const ShaderProgramDocument& GetDocument() const { return m_Doc; }
    const std::vector<std::string>& GetErrors() const { return m_Errors; }

    // Returns nullptr if compilation/reflection failed or hasn't run.
    const Rendering::ShaderPackage* GetPackage() const { return m_Package.get(); }
    // Absolute path to the generated .shaderpkg in <workspace>/.Cache/Shaders/..., or empty if not generated.
    const std::string& GetGeneratedShaderPkgPath() const { return m_GeneratedShaderPkgPath; }

private:
    struct ShaderPackageDeleter
    {
        void operator()(Rendering::ShaderPackage* p) const noexcept;
    };

    ShaderProgramDocument m_Doc{};
    std::vector<std::string> m_Errors;
    std::unique_ptr<Rendering::ShaderPackage, ShaderPackageDeleter> m_Package;
    std::string m_GeneratedShaderPkgPath;
};

} // namespace GameEngine

