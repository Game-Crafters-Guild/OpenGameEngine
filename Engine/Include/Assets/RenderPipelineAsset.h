#pragma once

#include "AssetCore/Asset.h"

#include <string>
#include <vector>

namespace GameEngine {

// Text-based render pipeline asset that describes a customizable render graph/pipeline.
// The schema is JSON and versioned via `schemaVersion` in the document.
struct RenderPipelinePassSummary
{
    std::string id;
    std::string type;
    bool enabled = true;
};

struct RenderPipelineResourceSummary
{
    std::string name;
    std::string kind;  // "Texture" | "Buffer" (stringly-typed at asset layer)
    std::string scope; // "Frame" | "PerView"
};

struct RenderPipelineDocument
{
    uint32_t schemaVersion = 0;
    std::string pipelineName;
    // Raw JSON text (preserved as loaded). The pipeline compiler consumes this.
    std::string jsonText;
    // Lightweight summaries for Editor UI and quick diagnostics.
    std::vector<RenderPipelinePassSummary> passes;
    std::vector<RenderPipelineResourceSummary> resources;
};

class RenderPipelineAsset final : public Asset
{
public:
    RenderPipelineAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::RenderPipeline, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const RenderPipelineDocument& GetDocument() const { return m_Doc; }
    const std::vector<std::string>& GetErrors() const { return m_Errors; }

    // FNV-1a64 of the JSON text, computed once per Load() / Reload().
    // Consumers (RenderServices) compare this across frames to detect content
    // changes without rehashing the JSON every frame. Zero means "no content".
    uint64_t GetSourceHash() const { return m_SourceHash; }

private:
    bool ParseFromText(const std::string& text, const std::filesystem::path& sourcePathForRelativeErrors);

    RenderPipelineDocument m_Doc{};
    std::vector<std::string> m_Errors;
    uint64_t m_SourceHash = 0;
};

} // namespace GameEngine


