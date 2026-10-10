#include "UI/AssetField.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSearchProvider.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Editor/DragDropPayloads.h"
#include "Types/StringUtils.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Interaction/Payload.h"

#include <any>
#include <filesystem>
#include <string_view>

namespace GameEngine
{
namespace
{
// Display name for a GUID that names an engine built-in rather than a registry
// asset. Meshes first, then materials — the two families are separate lookups
// because IsPrimitive() means "primitive mesh".
std::string_view BuiltInAssetName(const GUID& guid)
{
    using Engine::Renderer::PrimitiveGenerator;
    if (const std::string_view mesh = PrimitiveGenerator::NameFromGuid(guid); !mesh.empty())
        return mesh;
    return PrimitiveGenerator::MaterialNameFromGuid(guid);
}
} // namespace

AssetField::AssetField()
{
    // Base adds the "asset-field" class + stylesheet and builds the name + clear
    // button. Add the asset-specific chrome (icon, preview) BEFORE the name so the
    // child/paint order is icon, preview, name, clear.
    auto icon = std::make_unique<UIElement>();
    icon->AddClass("asset-field-icon");
    m_Icon = icon.get();
    AddChild(std::move(icon));

    // Preview strip: model thumbnail (via IThumbnailProvider) or the texture
    // file itself; hidden for every other asset type.
    auto preview = std::make_unique<UIElement>();
    preview->AddClass("asset-field-preview");
    preview->AddClass("hidden");
    m_Preview = preview.get();
    AddChild(std::move(preview));

    BuildNameAndClearButton();
}

void AssetField::SetAcceptedTypes(std::vector<AssetType> types)
{
    m_AcceptedTypes = std::move(types);
}

void AssetField::SetMetadataFilter(std::function<bool(const AssetIndexRecord&)> filter)
{
    m_MetadataFilter = std::move(filter);
}

void AssetField::SetAssetRegistry(AssetRegistry* registry)
{
    m_Registry = registry;
}

void AssetField::SetValue(const GUID& guid)
{
    if (m_Value == guid)
        return;
    m_Value = guid;
    UpdateDisplay();
}

void AssetField::Clear()
{
    ApplyValue(GUID::Null());
}

void AssetField::ApplyValue(const GUID& guid)
{
    if (m_Value == guid)
        return;
    m_Value = guid;
    UpdateDisplay();
    if (m_OnValueChanged)
        m_OnValueChanged(m_Value);
}

void AssetField::ClearModelPreviewStrip()
{
    if (!m_Preview)
        return;
    m_Preview->AddClass("hidden");
    UI::Layout::ClearBackgroundOverride(*m_Preview);
}

void AssetField::UpdateDisplay()
{
    if (m_Value.IsNull())
    {
        ShowEmpty();
        ClearModelPreviewStrip();
        if (!m_CurrentIconClass.empty())
        {
            m_Icon->RemoveClass(m_CurrentIconClass);
            m_CurrentIconClass.clear();
        }
        m_Icon->AddClass("hidden");
        return;
    }

    // Try to resolve asset metadata
    if (m_Registry)
    {
        AssetMetadata meta;
        if (m_Registry->TryGetAssetMetadata(m_Value, meta))
        {
            ShowNamed(meta.Name);

            // Type icon (hidden when a model preview image is shown).
            m_Icon->RemoveClass("hidden");
            if (!m_CurrentIconClass.empty())
                m_Icon->RemoveClass(m_CurrentIconClass);
            m_CurrentIconClass = AssetSearchProvider::GetIconClassForType(meta.Type);
            m_Icon->AddClass(m_CurrentIconClass);

            if (meta.Type == AssetType::Texture && m_Preview && !meta.Path.empty())
            {
                // A texture is its own thumbnail: paint the file into the
                // preview strip directly, the way MaterialTexturePreviewHost
                // does — no provider round-trip, and the UI texture cache
                // dedups with any larger preview of the same file.
                ++m_ModelPreviewRequestGen; // invalidate any in-flight model thumb
                UI::Layout::SetBackgroundPath(*m_Preview, meta.Path.string());
                m_Preview->RemoveClass("hidden");
                m_Icon->AddClass("hidden");
                if (!m_CurrentIconClass.empty())
                {
                    m_Icon->RemoveClass(m_CurrentIconClass);
                    m_CurrentIconClass.clear();
                }
            }
            else if (meta.Type == AssetType::Model && m_Thumbnails && m_Preview)
            {
                ++m_ModelPreviewRequestGen;
                const uint32_t gen = m_ModelPreviewRequestGen;
                ClearModelPreviewStrip();

                auto applyThumb = [this, gen](const std::string& rel) {
                    if (gen != m_ModelPreviewRequestGen || !m_Preview)
                        return;
                    if (rel.empty())
                    {
                        ClearModelPreviewStrip();
                        m_Icon->RemoveClass("hidden");
                        if (m_CurrentIconClass.empty())
                        {
                            m_CurrentIconClass = AssetSearchProvider::GetIconClassForType(AssetType::Model);
                            m_Icon->AddClass(m_CurrentIconClass);
                        }
                        return;
                    }
                    constexpr const char* kEnginePrefix = "engine:";
                    constexpr size_t kEnginePrefixLen = 7;
                    if (rel.rfind(kEnginePrefix, 0) == 0)
                        UI::Layout::SetBackgroundResourceName(*m_Preview, rel.substr(kEnginePrefixLen));
                    else
                        UI::Layout::SetBackgroundPath(*m_Preview, rel);
                    m_Preview->RemoveClass("hidden");
                    m_Icon->AddClass("hidden");
                    if (!m_CurrentIconClass.empty())
                    {
                        m_Icon->RemoveClass(m_CurrentIconClass);
                        m_CurrentIconClass.clear();
                    }
                };

                // GetOrRequest always fires the callback synchronously (via the
                // model-handler or fallback path), so we rely solely on applyThumb
                // rather than also acting on the return value – that would call
                // applyThumb twice, leaving m_CurrentIconClass in a cleared state
                // on the second call and causing a stale-texture crash in the renderer.
                m_Thumbnails->GetOrRequest(meta.Path, 64, applyThumb, true);
            }
            else
            {
                ClearModelPreviewStrip();
            }

            return;
        }
    }

    ClearModelPreviewStrip();
    m_Icon->AddClass("hidden");

    // Engine built-ins — the default scene's primitive meshes and materials — are
    // deliberately not registry assets, so the metadata lookup above misses them
    // by design. Name them anyway: a truncated GUID tells the author nothing
    // about what is assigned, and built-ins are what a fresh scene is made of.
    if (const std::string_view builtIn = BuiltInAssetName(m_Value); !builtIn.empty())
    {
        ShowNamed(std::string(builtIn));
        return;
    }

    ShowNamed(m_Value.ToString().substr(0, 13) + "...");
}

std::unique_ptr<ISearchProvider> AssetField::CreateProvider()
{
    if (!m_Registry)
        return nullptr;

    auto provider = std::make_unique<AssetSearchProvider>(m_Registry);
    if (!m_AcceptedTypes.empty())
        provider->SetTypeFilter(m_AcceptedTypes);
    if (m_MetadataFilter)
        provider->SetMetadataFilter(m_MetadataFilter);
    provider->SetShowFullPath(true);
    if (m_Thumbnails)
        provider->SetThumbnailProvider(m_Thumbnails);
    return provider;
}

void AssetField::OnResultSelected(const SearchResultItem& item)
{
    if (const auto* guid = std::any_cast<GUID>(&item.UserData))
        ApplyValue(*guid);
}

SearchItemId AssetField::CurrentResultId() const
{
    return AssetSearchProvider::ResultIdFor(m_Value);
}

void AssetField::PositionDialog(SearchDialog& dialog)
{
    // Anchor the panel's right edge to the field's right edge (Inspector fields sit
    // on the window edge, so the panel extends left).
    dialog.SetAnchorPosition(GetLayoutX() + GetLayoutWidth(), GetLayoutY() + GetLayoutHeight(),
                             SearchDialogHorizontalAnchor::TrailingRight);
}

bool AssetField::IsAcceptedExtension(const std::string& ext) const
{
    if (m_AcceptedTypes.empty())
        return true; // Accept any asset type

    for (auto type : m_AcceptedTypes)
    {
        if (IsValidExtensionForAssetType(type, ext))
            return true;
    }
    return false;
}

// -- IDropTarget implementation ------------------------------------------------

bool AssetField::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
}

UI::Interaction::DropFeedback AssetField::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty())
        return {false, "No payload"};

    const std::filesystem::path p = payload->paths[0];
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec))
        return {false, "Missing file"};
    if (std::filesystem::is_directory(p, ec))
        return {false, "Drop a file, not a folder"};

    const std::string ext = p.extension().string();
    if (!IsAcceptedExtension(ext))
    {
        std::string typeNames;
        for (auto type : m_AcceptedTypes)
        {
            if (!typeNames.empty())
                typeNames += "/";
            typeNames += AssetTypeToString(type);
        }
        return {false, "Expected " + typeNames + " asset"};
    }
    if (m_MetadataFilter && m_Registry)
    {
        AssetMetadata meta{};
        if (!m_Registry->TryGetAssetMetadata(p, meta) ||
            !m_MetadataFilter(ToAssetIndexRecord(meta)))
            return {false, "Asset is not valid for this slot"};
    }

    return {true, {}};
}

void AssetField::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty() || !m_Registry)
        return;

    const std::filesystem::path p = payload->paths[0];
    const auto result = m_Registry->RegisterAssetByPath(p);
    if (!result.IsOk())
        return;

    if (m_MetadataFilter)
    {
        AssetMetadata meta{};
        if (!m_Registry->TryGetAssetMetadata(result.Value(), meta) ||
            !m_MetadataFilter(ToAssetIndexRecord(meta)))
            return;
    }

    ApplyValue(result.Value());
}

} // namespace GameEngine
