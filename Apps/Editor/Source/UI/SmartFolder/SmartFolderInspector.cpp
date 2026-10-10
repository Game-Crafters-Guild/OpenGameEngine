#include "UI/SmartFolder/SmartFolderInspector.h"
#include "UI/SmartFolder/SmartFolderManager.h"
#include "UI/SmartFolder/SmartFolderCommands.h"
#include "UndoRedo/UndoRedoService.h"
#include "Core/Engine.h"
#include "UI/EditorTags.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/Dropdown.h"

#include <filesystem>
#include <functional>

namespace GameEngine {

SmartFolderInspector::SmartFolderInspector()
{
    AddClass("smart-folder-inspector");
}

void SmartFolderInspector::SetSmartFolder(const std::string& smartFolderId, SmartFolderManager* manager)
{
    m_SmartFolderId = smartFolderId;
    m_Manager = manager;
    // New selection/context: rebuild cache and UI.
    m_CachedLocationOptionsValid = false;
    RequestRebuildUI();
}

void SmartFolderInspector::RequestRebuildUI()
{
    if (m_RebuildPosted)
        return;

    // Avoid rebuilding the UI tree during event dispatch (can cause reentrancy and stalls).
    if (UIElement::IsInEventDispatch())
    {
        m_RebuildPosted = true;
        PostAction([this]()
                   {
                       m_RebuildPosted = false;
                       RebuildUI();
                   });
        return;
    }

    RebuildUI();
}

void SmartFolderInspector::SetNameValueFromExternal(const std::string& name)
{
    if (m_NameField && m_NameField->GetValue() != name)
        m_NameField->SetValueWithoutNotify(name);
}

void SmartFolderInspector::EnsureLocationOptionsCached()
{
    const std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (m_CachedLocationOptionsValid && m_CachedWorkspaceRoot == workspaceRoot)
        return;

    m_CachedWorkspaceRoot = workspaceRoot;
    m_CachedLocationOptions.clear();
    m_CachedLocationOptions.reserve(64);

    m_CachedLocationOptions.push_back("(All Assets)");

    const std::filesystem::path assetsRoot = workspaceRoot / "Assets";
    if (!assetsRoot.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(assetsRoot, ec) && std::filesystem::is_directory(assetsRoot, ec))
        {
            m_CachedLocationOptions.push_back("Assets");

            // Collect directories recursively (up to 3 levels deep within Assets).
            std::function<void(const std::filesystem::path&, int)> collectDirs;
            collectDirs = [this, &collectDirs, &workspaceRoot](const std::filesystem::path& dir, int depth)
            {
                if (depth > 3)
                    return;
                std::error_code ecIt;
                for (auto it = std::filesystem::directory_iterator(dir, ecIt); !ecIt && it != std::filesystem::end(it); ++it)
                {
                    std::error_code ecDir;
                    if (!it->is_directory(ecDir) || ecDir)
                        continue;
                    const std::string name = it->path().filename().string();
                    if (!name.empty() && name[0] == '.')
                        continue;

                    std::error_code ecRel;
                    std::string relativePath = std::filesystem::relative(it->path(), workspaceRoot, ecRel).string();
                    if (ecRel || relativePath.empty())
                        continue;

                    m_CachedLocationOptions.push_back(relativePath);
                    collectDirs(it->path(), depth + 1);
                }
            };
            collectDirs(assetsRoot, 1);
        }
    }

    m_CachedLocationOptionsValid = true;
}

void SmartFolderInspector::RebuildUI()
{
    m_RebuildPosted = false;
    m_NameField = nullptr;

    // Clear existing children
    RemoveAllChildren();
    
    if (!m_Manager || m_SmartFolderId.empty()) {
        return;
    }

    SmartFolder* folder = m_Manager->GetById(m_SmartFolderId);
    if (!folder) {
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Smart folder not found");
        AddChild(std::move(label));
        return;
    }

    // Header
    {
        auto header = std::make_unique<Label>();
        header->AddClass("inspector-header");
        header->SetText("Smart Folder");
        AddChild(std::move(header));
    }

    BuildNameField(folder);
    BuildLocationDropdown(folder);
    BuildMatchDropdown(folder);
    BuildGlobalToggle(folder);
    BuildFiltersSection(folder);
    BuildDeleteButton(folder);
}

void SmartFolderInspector::BuildNameField(SmartFolder* folder)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");
    row->AddClass("smart-folder-name-row");

    auto nameLabel = std::make_unique<Label>();
    nameLabel->AddClass("inspector-label");
    nameLabel->SetText("Name");
    row->AddChild(std::move(nameLabel));

    auto nameField = std::make_unique<TextField>();
    nameField->AddClass("inspector-field");
    nameField->SetValue(folder->Name);
    m_NameField = nameField.get();
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    SmartFolderInspector* self = this;
    nameField->SetOnValueChanging([self](const std::string& newValue) {
        if (self->m_OnNameChanging)
            self->m_OnNameChanging(newValue);
    });
    nameField->SetOnValueChanged([self, manager, folderId](const std::string& newValue) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            f->Name = newValue;
            manager->Update(*f);
        }
        if (self->m_OnNameChanged)
            self->m_OnNameChanged(newValue);
    });
    row->AddChild(std::move(nameField));

    AddChild(std::move(row));
}

void SmartFolderInspector::BuildLocationDropdown(SmartFolder* folder)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");
    row->AddClass("smart-folder-row");

    auto scopeLabel = std::make_unique<Label>();
    scopeLabel->AddClass("inspector-label");
    scopeLabel->SetText("Location");
    row->AddChild(std::move(scopeLabel));

    EnsureLocationOptionsCached();
    const auto& dirOptions = m_CachedLocationOptions;
    
    // Find current selection index
    int selectedIndex = 0;
    if (!folder->DirectoryScope.empty()) {
        std::string currentScope = folder->DirectoryScope.string();
        for (size_t i = 0; i < dirOptions.size(); ++i) {
            if (dirOptions[i] == currentScope) {
                selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    
    auto scopeDropdown = std::make_unique<Dropdown>();
    scopeDropdown->AddClass("inspector-dropdown");
    scopeDropdown->SetOptionsFromLabels(dirOptions, selectedIndex);
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    Dropdown* dropdownPtr = scopeDropdown.get();
    dropdownPtr->SetOnValueChanged([this, manager, folderId, dropdownPtr](const std::string&) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            int idx = dropdownPtr->GetSelectedIndex();
            if (idx == 0) {
                f->DirectoryScope = std::filesystem::path{};
            } else if (idx > 0 && idx < static_cast<int>(m_CachedLocationOptions.size())) {
                f->DirectoryScope = std::filesystem::path{m_CachedLocationOptions[(size_t)idx]};
            }
            manager->Update(*f);
        }
    });
    row->AddChild(std::move(scopeDropdown));

    AddChild(std::move(row));
}

void SmartFolderInspector::BuildMatchDropdown(SmartFolder* folder)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");
    row->AddClass("smart-folder-row");

    auto modeLabel = std::make_unique<Label>();
    modeLabel->AddClass("inspector-label");
    modeLabel->SetText("Match");
    row->AddChild(std::move(modeLabel));

    auto modeDropdown = std::make_unique<Dropdown>();
    modeDropdown->AddClass("inspector-dropdown");
    modeDropdown->SetOptionsFromLabels({"All filters (AND)", "Any filter (OR)"}, 
        folder->CombineMode == FilterCombineMode::And ? 0 : 1);
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    Dropdown* dropdownPtr = modeDropdown.get();
    dropdownPtr->SetOnValueChanged([manager, folderId, dropdownPtr](const std::string&) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            f->CombineMode = (dropdownPtr->GetSelectedIndex() == 0) 
                ? FilterCombineMode::And 
                : FilterCombineMode::Or;
            manager->Update(*f);
        }
    });
    row->AddChild(std::move(modeDropdown));

    AddChild(std::move(row));
}

void SmartFolderInspector::BuildGlobalToggle(SmartFolder* folder)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");
    row->AddClass("smart-folder-row");

    auto storageLabel = std::make_unique<Label>();
    storageLabel->AddClass("inspector-label");
    storageLabel->SetText("Global");
    row->AddChild(std::move(storageLabel));

    auto storageToggle = std::make_unique<Toggle>();
    storageToggle->AddClass("inspector-toggle");
    storageToggle->SetChecked(folder->IsGlobal);
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    storageToggle->SetOnValueChanged([manager, folderId](bool isGlobal) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            f->IsGlobal = isGlobal;
            manager->Update(*f);
        }
    });
    row->AddChild(std::move(storageToggle));

    AddChild(std::move(row));
}

void SmartFolderInspector::BuildFiltersSection(SmartFolder* folder)
{
    // Filters section header
    {
        auto filtersHeader = std::make_unique<Label>();
        filtersHeader->AddClass("inspector-section-subheader");
        filtersHeader->SetText("Filters");
        AddChild(std::move(filtersHeader));
    }

    // Add filter button
    {
        auto addButton = std::make_unique<Button>();
        addButton->AddClass("inspector-button");
        addButton->AddClass("add-filter-button");
        addButton->SetText("Add Filter");
        
        std::string folderId = m_SmartFolderId;
        SmartFolderManager* manager = m_Manager;
        SmartFolderInspector* self = this;
        addButton->RegisterEventHandler(kEventButtonClick, [self, manager, folderId](UIEvent&) {
            if (SmartFolder* f = manager->GetById(folderId)) {
                SmartFolderFilter newFilter;
                newFilter.FilterType = SmartFolderFilter::Type::FileType;
                newFilter.Value = ".png";
                f->Filters.push_back(newFilter);
                manager->Update(*f);
                // Refresh the inspector
                self->RequestRebuildUI();
            }
        });
        AddChild(std::move(addButton));
    }

    // Existing filters
    for (size_t i = 0; i < folder->Filters.size(); ++i) {
        BuildFilterRow(folder, i);
    }
}

void SmartFolderInspector::BuildFilterRow(SmartFolder* folder, size_t filterIndex)
{
    const SmartFolderFilter& filter = folder->Filters[filterIndex];
    
    auto filterRow = std::make_unique<UIElement>();
    filterRow->AddClass("inspector-row");
    filterRow->AddClass("smart-folder-filter-row");

    // Filter type dropdown
    auto typeDropdown = std::make_unique<Dropdown>();
    typeDropdown->AddClass("inspector-dropdown");
    typeDropdown->AddClass("filter-type-dropdown");
    
    int selectedTypeIndex = 0;
    switch (filter.FilterType) {
        case SmartFolderFilter::Type::FileType: selectedTypeIndex = 0; break;
        case SmartFolderFilter::Type::NameContains: selectedTypeIndex = 1; break;
        case SmartFolderFilter::Type::Regex: selectedTypeIndex = 2; break;
        case SmartFolderFilter::Type::Tag: selectedTypeIndex = 3; break;
        case SmartFolderFilter::Type::VCSStatus: selectedTypeIndex = 4; break;
    }
    typeDropdown->SetOptionsFromLabels({"File Type", "Name Contains", "Regex", "Tag", "VCS Status"}, selectedTypeIndex);
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    SmartFolderInspector* self = this;
    Dropdown* typeDropdownPtr = typeDropdown.get();
    typeDropdownPtr->SetOnValueChanged([manager, folderId, filterIndex, typeDropdownPtr, self](const std::string&) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            if (filterIndex < f->Filters.size()) {
                switch (typeDropdownPtr->GetSelectedIndex()) {
                    case 0: f->Filters[filterIndex].FilterType = SmartFolderFilter::Type::FileType; break;
                    case 1: f->Filters[filterIndex].FilterType = SmartFolderFilter::Type::NameContains; break;
                    case 2: f->Filters[filterIndex].FilterType = SmartFolderFilter::Type::Regex; break;
                    case 3: f->Filters[filterIndex].FilterType = SmartFolderFilter::Type::Tag; break;
                    case 4: f->Filters[filterIndex].FilterType = SmartFolderFilter::Type::VCSStatus; break;
                }
                manager->Update(*f);
                self->RequestRebuildUI();
            }
        }
    });
    filterRow->AddChild(std::move(typeDropdown));

    // Value field - dropdown for file type or tag, text field for name/regex
    if (filter.FilterType == SmartFolderFilter::Type::FileType) {
        auto valueDropdown = std::make_unique<Dropdown>();
        valueDropdown->AddClass("inspector-dropdown");
        valueDropdown->AddClass("filter-value-dropdown");
        
        std::vector<std::string> fileTypes = {
            ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".hdr",
            ".fbx", ".obj", ".gltf", ".glb",
            ".material", ".mat",
            ".shader", ".glsl", ".hlsl", ".vert", ".frag", ".comp",
            ".renderpipeline", ".rendergraph",
            ".cs", ".cpp", ".h", ".hpp",
            ".txt", ".json", ".xml", ".yaml",
            ".uxml", ".css",
            ".wav", ".mp3", ".ogg"
        };
        
        int selectedIndex = 0;
        for (size_t j = 0; j < fileTypes.size(); ++j) {
            if (fileTypes[j] == filter.Value) {
                selectedIndex = static_cast<int>(j);
                break;
            }
        }
        valueDropdown->SetOptionsFromLabels(fileTypes, selectedIndex);
        
        Dropdown* valueDropdownPtr = valueDropdown.get();
        valueDropdownPtr->SetOnValueChanged([manager, folderId, filterIndex](const std::string& newValue) {
            if (SmartFolder* f = manager->GetById(folderId)) {
                if (filterIndex < f->Filters.size()) {
                    f->Filters[filterIndex].Value = newValue;
                    manager->Update(*f);
                }
            }
        });
        filterRow->AddChild(std::move(valueDropdown));
    } else if (filter.FilterType == SmartFolderFilter::Type::Tag) {
        auto valueDropdown = std::make_unique<Dropdown>();
        valueDropdown->AddClass("inspector-dropdown");
        valueDropdown->AddClass("filter-value-dropdown");
        
        std::vector<Dropdown::Option> tagOptions;
        const std::vector<EditorTagDefinition> tagDefs = EditorTags::Load();
        for (const EditorTagDefinition& def : tagDefs) {
            Dropdown::Option opt;
            opt.value = def.Name;
            opt.label = def.Name;
            opt.color = def.Color;
            tagOptions.push_back(std::move(opt));
        }
        if (tagOptions.empty()) {
            tagOptions.push_back({ "(No tags defined)", "(No tags defined)", "" });
        }
        int selectedIndex = 0;
        for (size_t j = 0; j < tagOptions.size(); ++j) {
            if (tagOptions[j].value == filter.Value) {
                selectedIndex = static_cast<int>(j);
                break;
            }
        }
        valueDropdown->SetOptions(tagOptions, selectedIndex);
        
        Dropdown* valueDropdownPtr = valueDropdown.get();
        valueDropdownPtr->SetOnValueChanged([manager, folderId, filterIndex](const std::string& newValue) {
            if (SmartFolder* f = manager->GetById(folderId)) {
                if (filterIndex < f->Filters.size()) {
                    f->Filters[filterIndex].Value = newValue;
                    manager->Update(*f);
                }
            }
        });
        filterRow->AddChild(std::move(valueDropdown));
    } else if (filter.FilterType == SmartFolderFilter::Type::VCSStatus) {
        auto valueDropdown = std::make_unique<Dropdown>();
        valueDropdown->AddClass("inspector-dropdown");
        valueDropdown->AddClass("filter-value-dropdown");

        std::vector<std::string> statusOptions = {
            "Modified", "Added", "Deleted", "Unversioned",
            "Conflict", "LockedByMe", "LockedByOthers",
            "ServerHasChanges", "Ignored", "Clean"
        };

        int selectedIndex = 0;
        for (size_t j = 0; j < statusOptions.size(); ++j) {
            if (statusOptions[j] == filter.Value) {
                selectedIndex = static_cast<int>(j);
                break;
            }
        }
        valueDropdown->SetOptionsFromLabels(statusOptions, selectedIndex);

        Dropdown* valueDropdownPtr = valueDropdown.get();
        valueDropdownPtr->SetOnValueChanged([manager, folderId, filterIndex](const std::string& newValue) {
            if (SmartFolder* f = manager->GetById(folderId)) {
                if (filterIndex < f->Filters.size()) {
                    f->Filters[filterIndex].Value = newValue;
                    manager->Update(*f);
                }
            }
        });
        filterRow->AddChild(std::move(valueDropdown));
    } else {
        auto valueField = std::make_unique<TextField>();
        valueField->AddClass("inspector-field");
        valueField->AddClass("filter-value-field");
        valueField->SetValue(filter.Value);
        
        valueField->SetOnValueChanged([manager, folderId, filterIndex](const std::string& newValue) {
            if (SmartFolder* f = manager->GetById(folderId)) {
                if (filterIndex < f->Filters.size()) {
                    f->Filters[filterIndex].Value = newValue;
                    manager->Update(*f);
                }
            }
        });
        filterRow->AddChild(std::move(valueField));
    }

    // Remove filter button
    auto removeButton = std::make_unique<Button>();
    removeButton->AddClass("small");
    removeButton->AddClass("secondary");
    removeButton->AddClass("icon-button");
    removeButton->AddClass("xclose-icon");
    
    removeButton->RegisterEventHandler(kEventButtonClick, [self, manager, folderId, filterIndex](UIEvent&) {
        if (SmartFolder* f = manager->GetById(folderId)) {
            if (filterIndex < f->Filters.size()) {
                f->Filters.erase(f->Filters.begin() + static_cast<ptrdiff_t>(filterIndex));
                manager->Update(*f);
                self->RequestRebuildUI();
            }
        }
    });
    filterRow->AddChild(std::move(removeButton));

    AddChild(std::move(filterRow));
}

void SmartFolderInspector::BuildDeleteButton(SmartFolder* /*folder*/)
{
    auto deleteButton = std::make_unique<Button>();
    deleteButton->AddClass("inspector-button");
    deleteButton->AddClass("danger");
    deleteButton->SetText("Delete Smart Folder");
    
    std::string folderId = m_SmartFolderId;
    SmartFolderManager* manager = m_Manager;
    Editor::UndoRedoService* undo = m_Undo;
    std::function<void()> onDeleted = m_OnDeleted;
    
    deleteButton->RegisterEventHandler(kEventButtonClick, [manager, folderId, undo, onDeleted](UIEvent&) {
        if (undo) {
            // Use undo command for delete
            auto cmd = std::make_unique<Editor::DeleteSmartFolderCommand>(
                manager, folderId,
                [manager]() { manager->NotifyChanged(); });
            undo->Execute(std::move(cmd));
        } else {
            // Fallback if no undo service
            manager->Delete(folderId);
        }
        // Notify that the folder was deleted
        if (onDeleted) {
            onDeleted();
        }
    });
    AddChild(std::move(deleteButton));
}

} // namespace GameEngine
