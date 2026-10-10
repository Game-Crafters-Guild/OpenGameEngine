#include "AgentCallMedia.h"

#include "AssistantActionLedger.h"

#include "AssetCore/AssetTypes.h"
#include "Editor/CaptureOutputDirectory.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace GameEngine
{
namespace
{
// The tools whose result is a capture the editor wrote.
constexpr std::array<std::string_view, 2> kCaptureTools = {"take_screenshot", "capture_resource"};

// A path longer than this is not a path an asset reference uses.
constexpr size_t kMaxPathLength = 1024;

// A path whose extension is a model's, a material's or a texture's.
bool IsAssetPathText(const std::string& text)
{
    if (text.empty() || text.size() > kMaxPathLength || text.find('\n') != std::string::npos)
        return false;
    const AssetType type = GetAssetTypeFromExtension(GetCompoundExtensionFromPath(text));
    return type == AssetType::Model || type == AssetType::Material || type == AssetType::Texture;
}

void CollectReferences(const nlohmann::json& value, const std::string& exceptPath, std::vector<std::string>& out)
{
    if (value.is_string())
    {
        const std::string& text = value.get_ref<const std::string&>();
        if (text != exceptPath && (ParserExtraction::LooksLikeGuid(text) || IsAssetPathText(text)) &&
            std::find(out.begin(), out.end(), text) == out.end())
            out.push_back(text);
        return;
    }
    if (value.is_structured())
        for (const nlohmann::json& item : value)
            CollectReferences(item, exceptPath, out);
}

// `path` names a file inside `directory`.
bool IsInside(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    const std::filesystem::path file = path.lexically_normal();
    const std::filesystem::path root = directory.lexically_normal();
    const auto [rootEnd, fileAt] = std::mismatch(root.begin(), root.end(), file.begin(), file.end());
    return rootEnd == root.end() && fileAt != file.end();
}

// The handler's own result inside the editor's response ({"id", "ok", "result"}); null when
// the text is not a response that succeeded (a refusal, a response cut at the ledger's limit).
nlohmann::json ResultOf(const AssistantAction& action)
{
    const nlohmann::json response = nlohmann::json::parse(action.Result, nullptr, false);
    if (!response.is_object() || !response.value("ok", false))
        return nullptr;
    const auto result = response.find("result");
    return result == response.end() ? nlohmann::json(nullptr) : *result;
}
// The editor's capture directory, read once: Editor::CaptureOutputDirectory creates the
// directory on every call, and the rows ask on every refresh.
const std::filesystem::path& CaptureDirectory()
{
    static const std::filesystem::path kDirectory = Editor::CaptureOutputDirectory();
    return kDirectory;
}

// The capture `action`'s `result` names (AgentCallMedia::Image).
std::optional<AgentCallImage> CaptureImage(const AssistantAction& action, const nlohmann::json& result)
{
    if (action.State != AssistantActionState::Done ||
        std::find(kCaptureTools.begin(), kCaptureTools.end(), action.Tool) == kCaptureTools.end() ||
        !result.is_object())
        return std::nullopt;
    const auto path = result.find("filePath");
    const auto width = result.find("width");
    const auto height = result.find("height");
    if (path == result.end() || !path->is_string() || width == result.end() || !width->is_number_unsigned() ||
        height == result.end() || !height->is_number_unsigned())
        return std::nullopt;
    AgentCallImage image{path->get<std::string>(), width->get<uint32_t>(), height->get<uint32_t>()};
    if (GetAssetTypeFromExtension(GetCompoundExtensionFromPath(image.Path)) != AssetType::Texture || image.Width == 0 ||
        image.Height == 0 ||
        !IsInside(std::filesystem::path(std::u8string(image.Path.begin(), image.Path.end())), CaptureDirectory()))
        return std::nullopt;
    return image;
}
} // namespace

AgentCallMedia AgentCallMediaOf(const AssistantAction& action)
{
    AgentCallMedia media;
    const nlohmann::json result = ResultOf(action);
    media.Image = CaptureImage(action, result);
    const std::string exceptPath = media.Image ? media.Image->Path : std::string();
    CollectReferences(nlohmann::json::parse(action.Arguments, nullptr, false), exceptPath, media.Resources);
    CollectReferences(result, exceptPath, media.Resources);
    return media;
}

AgentCallImageSize FitAgentCallImage(uint32_t width, uint32_t height, float maxWidth, float maxHeight)
{
    if (width == 0 || height == 0 || maxWidth <= 0.0f || maxHeight <= 0.0f)
        return {};
    const float scale = std::min({1.0f, maxWidth / static_cast<float>(width), maxHeight / static_cast<float>(height)});
    return {static_cast<float>(width) * scale, static_cast<float>(height) * scale};
}
} // namespace GameEngine
