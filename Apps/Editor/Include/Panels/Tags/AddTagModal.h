#pragma once

#include "UI/UIElement.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

// Modal to pick a tag to assign to selected asset(s), or add a new tag.
// Tag definitions loaded/saved via EditorTags (editor preferences).
// Assignments are applied via SetOnAssignTag (caller uses AssetRegistry SetMetaValue).
class AddTagModal final : public UIElement
{
public:
    AddTagModal();

    void Show(const std::vector<std::filesystem::path>& paths);
    void Hide();

    // Called when user picks a tag: (paths, tagName). Caller appends tag to assets' "tags" meta.
    void SetOnAssignTag(std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> cb)
    {
        m_OnAssignTag = std::move(cb);
    }

private:
    void BuildTagList();
    void OnTagRowClicked(const std::string& tagName);
    void OnAllTagsClicked();
    void OnAddNewTagClicked();
    void OnBackdropClicked();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_Window = nullptr;
    UIElement* m_TagListContainer = nullptr;  // scroll content: tag rows
    UIElement* m_AllTagsRow = nullptr;
    UIElement* m_AddNewSection = nullptr;   // inline form (name field + Add button), hidden by default
    class TextField* m_NewTagNameField = nullptr;
    class Button* m_AddTagButton = nullptr;

    std::vector<std::filesystem::path> m_Paths;
    std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> m_OnAssignTag;
};

} // namespace GameEngine
