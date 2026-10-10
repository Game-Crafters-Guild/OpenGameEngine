#include "EditorApplication.h"

#include "Panels/SettingsPanel.h"
#include "Panels/InspectorPanel.h"
#include "Editor/Settings/ExperimentalRenderingSettingsPage.h"
#include "Editor/Entities/DDGIVolumeComponentTraits.h"
#include "Editor/Settings/InterfaceSettingsPage.h"
#include "Editor/Settings/AssetPreviewSettings.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "Editor/Settings/GameUIScaleSettingsPage.h"
#include "Editor/Settings/GraphSettingsPage.h"
#include "Editor/Settings/LodSettingsPage.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/TooltipSettings.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Panels/AssetsPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

namespace GameEngine
{
static void MigrateLegacySharedTreePrefs(Editor::SettingsStore& prefs, std::string* errOut)
{
    auto migrate = [&](const char* legacyKey, const char* hKey, const char* aKey, double defaultVal)
    {
        double legacyVal = defaultVal;
        const bool hasLegacy = prefs.TryGetDouble(legacyKey, legacyVal);
        double hProbe = defaultVal;
        double aProbe = defaultVal;
        const bool hasH = prefs.TryGetDouble(hKey, hProbe);
        const bool hasA = prefs.TryGetDouble(aKey, aProbe);
        if (!hasH)
            prefs.SetDouble(hKey, hasLegacy ? legacyVal : defaultVal);
        if (!hasA)
            prefs.SetDouble(aKey, hasLegacy ? legacyVal : defaultVal);
    };
    migrate("ui.treeIconSize", "ui.hierarchyTreeIconSize", "ui.assetsTreeIconSize", 20.0);
    migrate("ui.treeRowHeight", "ui.hierarchyTreeRowHeight", "ui.assetsTreeRowHeight", 20.0);
    migrate("ui.treeChildIndent", "ui.hierarchyTreeChildIndent", "ui.assetsTreeChildIndent", 16.0);
    if (errOut)
        prefs.Save(errOut);
}

void EditorApplication::MigrateLegacySharedTreePreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    MigrateLegacySharedTreePrefs(prefs, &err);
}

void EditorApplication::InitializeTreeAndListSettings(SettingsPanel* settingsPanel,
                                                      AssetsPanel* assetsPanel,
                                                      HierarchyPanel* hierarchyPanel)
{
    if (!settingsPanel)
        return;

    // UI text scale / tree row height across panels.
    settingsPanel->SetOnBaseFontSizeChanged(
        [assetsPanel](float /*px*/)
        {
            if (assetsPanel)
                assetsPanel->RefreshViews();
        });

    settingsPanel->SetOnHierarchyTreeRowHeightChanged(
        [hierarchyPanel](float px)
        {
            if (hierarchyPanel)
                hierarchyPanel->SetTreeRowHeight(px);
        });
    settingsPanel->SetOnAssetsTreeRowHeightChanged(
        [assetsPanel](float px)
        {
            if (assetsPanel)
                assetsPanel->SetTreeRowHeight(px);
        });

    settingsPanel->SetOnGetHierarchyTreeRowHeight(
        [hierarchyPanel]() -> float
        {
            if (hierarchyPanel)
                return hierarchyPanel->GetTreeRowHeight();
            return 20.0f;
        });
    settingsPanel->SetOnGetAssetsTreeRowHeight(
        [assetsPanel]() -> float
        {
            if (assetsPanel)
                return assetsPanel->GetTreeRowHeight();
            return 20.0f;
        });

    settingsPanel->SetOnHierarchyTreeChildIndentChanged(
        [hierarchyPanel](float px)
        {
            if (hierarchyPanel)
                hierarchyPanel->SetTreeChildIndent(px);
        });
    settingsPanel->SetOnAssetsTreeChildIndentChanged(
        [assetsPanel](float px)
        {
            if (assetsPanel)
                assetsPanel->SetTreeChildIndent(px);
        });

    settingsPanel->SetOnHierarchyTreeIconSizeChanged(
        [hierarchyPanel](float px)
        {
            if (hierarchyPanel)
                hierarchyPanel->SetTreeIconSize(px);
        });
    settingsPanel->SetOnPreviewThumbnailsChanged(
        [hierarchyPanel]()
        {
            // Preview thumbnails (IBL on/off) were invalidated; rebuild the hierarchy tree
            // so its model-thumbnail icons re-fetch the re-rendered images.
            if (hierarchyPanel)
                hierarchyPanel->Refresh();
        });
    settingsPanel->SetOnAssetsTreeIconSizeChanged(
        [assetsPanel](float px)
        {
            if (assetsPanel)
                assetsPanel->SetTreeIconSize(px);
        });

    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        MigrateLegacySharedTreePrefs(prefs, &err);

        double hIndent = 16.0;
        prefs.TryGetDouble("ui.hierarchyTreeChildIndent", hIndent);
        const float hierarchyIndent = static_cast<float>(std::clamp(hIndent, 0.0, 32.0));
        if (hierarchyPanel)
            hierarchyPanel->SetTreeChildIndent(hierarchyIndent);

        double aIndent = 16.0;
        prefs.TryGetDouble("ui.assetsTreeChildIndent", aIndent);
        const float assetsIndent = static_cast<float>(std::clamp(aIndent, 0.0, 32.0));
        if (assetsPanel)
            assetsPanel->SetTreeChildIndent(assetsIndent);
    }

    if (hierarchyPanel)
    {
        hierarchyPanel->SetOnTreeIconSizeWheelCommit(
            [settingsPanel](float px)
            {
                if (settingsPanel)
                    settingsPanel->SetHierarchyTreeIconSizeValueAndPersist(px);
            });
    }
    if (assetsPanel)
    {
        assetsPanel->SetOnTreeIconSizeWheelCommit(
            [settingsPanel](float px)
            {
                if (settingsPanel)
                    settingsPanel->SetAssetsTreeIconSizeValueAndPersist(px);
            });
    }

    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double hIcon = 20.0;
        prefs.TryGetDouble("ui.hierarchyTreeIconSize", hIcon);
        const float hierarchyIconSize = static_cast<float>(
            std::clamp(hIcon, static_cast<double>(kMinEditorTreeIconSizePx),
                       static_cast<double>(kMaxEditorHierarchyTreeIconSizePx)));
        settingsPanel->SetHierarchyTreeIconSizeValue(hierarchyIconSize);

        double aIcon = 20.0;
        prefs.TryGetDouble("ui.assetsTreeIconSize", aIcon);
        const float assetsIconSize = static_cast<float>(
            std::clamp(aIcon, static_cast<double>(kMinEditorTreeIconSizePx),
                       static_cast<double>(kMaxEditorAssetsTreeIconSizePx)));
        settingsPanel->SetAssetsTreeIconSizeValue(assetsIconSize);
    }
}

void EditorApplication::InitializeAssetsGridAndSmartFolderSettings(SettingsPanel* settingsPanel,
                                                                    AssetsPanel* assetsPanel)
{
    if (!settingsPanel || !assetsPanel)
        return;

    const auto& workspaceRoot = GameEngine::EngineCore::GetInstance().GetWorkspaceRoot();
    auto loadGridIconSize = [workspaceRoot]() -> std::optional<float>
    {
        if (workspaceRoot.empty())
            return std::nullopt;
        GameEngine::Editor::SettingsStore store =
            GameEngine::Editor::OpenUserProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);
        const auto& root = store.Json();
        const auto it = root.find("ui.assets.grid.iconSize");
        if (it == root.end() || !it->is_number())
            return std::nullopt;
        return static_cast<float>(it->get<double>());
    };

    auto saveGridIconSize = [workspaceRoot](float px)
    {
        if (workspaceRoot.empty())
            return;
        GameEngine::Editor::SettingsStore store =
            GameEngine::Editor::OpenUserProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);
        store.SetDouble("ui.assets.grid.iconSize", px);
        (void)store.Save(&err);
    };

    // Initialize grid icon size from project settings and wire callbacks.
    if (auto saved = loadGridIconSize())
    {
        assetsPanel->SetGridIconSize(*saved);
        settingsPanel->SetGridIconSizeValue(*saved);
    }
    else
    {
        settingsPanel->SetGridIconSizeValue(assetsPanel->GetGridIconSize());
    }

    settingsPanel->SetOnGridIconSizeChanged(
        [assetsPanel](float px)
        {
            if (assetsPanel)
                assetsPanel->SetGridIconSize(px);
        });
    settingsPanel->SetOnGridIconSizeFinalized(
        [saveGridIconSize](float px)
        {
            saveGridIconSize(px);
        });
    // VCS badge display settings now change through provider settings tabs
    // (registry-driven); the refresh wire lives on the provider registry.
    Editor::EditorVcsProviderRegistry::Get().SetBadgeSettingsChangedHandler(
        [assetsPanel]()
        {
            if (assetsPanel)
                assetsPanel->RefreshViews();
        });
    settingsPanel->SetOnTruncationThresholdChanged(
        [assetsPanel](float threshold)
        {
            if (assetsPanel)
                assetsPanel->SetTruncationThreshold(threshold);
        });
    settingsPanel->SetOnTruncationEnabledChanged(
        [assetsPanel](bool enabled)
        {
            if (assetsPanel)
                assetsPanel->SetTruncationEnabled(enabled);
        });

    settingsPanel->SetOnAssetsListColumnsChanged(
        [assetsPanel]()
        {
            if (assetsPanel)
                assetsPanel->RefreshViews();
        });

    assetsPanel->SetOnGridIconSizeChanged(
        [settingsPanel, saveGridIconSize](float px)
        {
            if (settingsPanel)
                settingsPanel->SetGridIconSizeValue(px);
            saveGridIconSize(px);
        });

    // Smart folders position
    settingsPanel->SetOnSmartFoldersAtTopChanged(
        [assetsPanel](bool atTop)
        {
            if (assetsPanel)
                assetsPanel->SetSmartFoldersAtTop(atTop);
        });
    settingsPanel->SetOnSmartFoldersExpandedOnStartupChanged(
        [assetsPanel](bool expanded)
        {
            if (assetsPanel)
                assetsPanel->SetSmartFoldersExpandedOnStartup(expanded);
        });

    settingsPanel->SetOnAssetsFoldersFirstChanged(
        [assetsPanel](bool foldersFirst)
        {
            if (assetsPanel)
                assetsPanel->SetFoldersFirst(foldersFirst);
        });
    settingsPanel->SetOnAssetsExpandFoldersOnLoadChanged(
        [assetsPanel](bool expand)
        {
            if (assetsPanel)
                assetsPanel->SetExpandFoldersOnLoad(expand);
        });
    settingsPanel->SetOnAssetsExtraBottomViewToolbarChanged(
        [assetsPanel](bool enabled)
        {
            if (assetsPanel)
                assetsPanel->SetExtraBottomViewToolbarEnabled(enabled);
        });
    settingsPanel->SetOnAssetsSingleViewToggleIconChanged(
        [assetsPanel](bool enabled)
        {
            if (assetsPanel)
                assetsPanel->SetSingleViewToggleIconEnabled(enabled);
        });
    settingsPanel->SetOnAssetsBottomToolbarZoomSliderVisibleChanged(
        [assetsPanel](bool visible)
        {
            if (assetsPanel)
                assetsPanel->SetBottomToolbarZoomSliderVisible(visible);
        });

    // Tooltips and Online Assets pages register through the settings registry;
    // the SettingsPanel renders them from the descriptors.
    {
        using Editor::SettingsCategoryDescriptor;
        using Editor::SettingsFieldDescriptor;
        using Editor::TooltipSettings;

        SettingsCategoryDescriptor tooltips;
        tooltips.CategoryId = "tooltip";
        tooltips.Title = "Tooltips";
        tooltips.Group = Editor::SettingsCategoryGroup::UserSettings;
        tooltips.TreeRowClass = "tooltips-row";
        tooltips.Description =
            "Tooltips appear when hovering over icons and editor labels. Middle-mouse show lets "
            "you trigger a tooltip instantly by pressing the middle mouse button.";

        SettingsFieldDescriptor enabled;
        enabled.Label = "Enable Tooltips";
        enabled.SearchKeywords = "tooltips hover help hints enable disable";
        SettingsFieldDescriptor::ToggleField enabledToggle;
        enabledToggle.DefaultValue = true;
        enabledToggle.Get = []() { return TooltipSettings::Get().GetEnabled(); };
        enabledToggle.Set = [](bool value) { TooltipSettings::Get().SetEnabled(value); };
        enabled.Control = enabledToggle;
        tooltips.Fields.push_back(std::move(enabled));

        SettingsFieldDescriptor hoverDelay;
        hoverDelay.Label = "Hover Delay (seconds)";
        hoverDelay.SearchKeywords = "tooltips hover delay seconds wait";
        SettingsFieldDescriptor::SliderField delaySlider;
        delaySlider.DefaultValue = 0.5f;
        delaySlider.MinValue = 0.0f;
        delaySlider.MaxValue = 3.0f;
        delaySlider.Step = 0.05f;
        delaySlider.Get = []() { return TooltipSettings::Get().GetHoverDelaySeconds(); };
        delaySlider.Set = [](float value) { TooltipSettings::Get().SetHoverDelaySeconds(value); };
        hoverDelay.Control = delaySlider;
        tooltips.Fields.push_back(std::move(hoverDelay));

        SettingsFieldDescriptor hoverResetDelay;
        hoverResetDelay.Label = "Hover Reset Delay (seconds)";
        hoverResetDelay.Tooltip =
            "Time after leaving a shown tooltip during which hovering another tooltip shows it "
            "immediately. After this interval, the normal hover delay applies again.";
        hoverResetDelay.SearchKeywords = "tooltips hover reset delay grace interval normal again";
        SettingsFieldDescriptor::SliderField resetDelaySlider;
        resetDelaySlider.DefaultValue = 0.5f;
        resetDelaySlider.MinValue = 0.0f;
        resetDelaySlider.MaxValue = 3.0f;
        resetDelaySlider.Step = 0.05f;
        resetDelaySlider.Get = []() { return TooltipSettings::Get().GetHoverResetDelaySeconds(); };
        resetDelaySlider.Set = [](float value) { TooltipSettings::Get().SetHoverResetDelaySeconds(value); };
        hoverResetDelay.Control = resetDelaySlider;
        tooltips.Fields.push_back(std::move(hoverResetDelay));

        SettingsFieldDescriptor middleMouse;
        middleMouse.Label = "Show on Middle Mouse";
        middleMouse.SearchKeywords = "tooltips middle mouse button instant show";
        SettingsFieldDescriptor::ToggleField middleToggle;
        middleToggle.DefaultValue = false;
        middleToggle.Get = []() { return TooltipSettings::Get().GetMiddleMouseShow(); };
        middleToggle.Set = [](bool value) { TooltipSettings::Get().SetMiddleMouseShow(value); };
        middleMouse.Control = middleToggle;
        tooltips.Fields.push_back(std::move(middleMouse));

        SettingsFieldDescriptor arrowColor;
        arrowColor.Label = "Arrow Color";
        arrowColor.SearchKeywords = "tooltips arrow color pointer";
        SettingsFieldDescriptor::ColorField colorField;
        colorField.DefaultArgb = 0xFF000000u;
        colorField.Get = []() { return TooltipSettings::Get().GetArrowColor(); };
        colorField.Set = [](uint32_t argb) { TooltipSettings::Get().SetArrowColor(argb); };
        arrowColor.Control = colorField;
        tooltips.Fields.push_back(std::move(arrowColor));

        Editor::EditorSettingsRegistry::Get().RegisterCategory(std::move(tooltips));

        SettingsCategoryDescriptor onlineAssets;
        onlineAssets.CategoryId = "onlineAssets";
        onlineAssets.Title = "Online Assets";
        onlineAssets.Group = Editor::SettingsCategoryGroup::UserSettings;
        onlineAssets.TreeRowClass = "online-assets-row";
        onlineAssets.Description =
            "Online Assets enables browsing and downloading assets from online sources. Poly "
            "Haven provides free CC0 textures, models, and HDRIs.";

        SettingsFieldDescriptor onlineEnabled;
        onlineEnabled.Label = "Online Assets";
        onlineEnabled.SearchKeywords = "online assets browsing download";
        onlineEnabled.PrefKey = "onlineAssets.enabled";
        SettingsFieldDescriptor::ToggleField onlineToggle;
        onlineToggle.DefaultValue = true;
        onlineToggle.Set = [assetsPanel](bool value)
        {
            if (assetsPanel)
                assetsPanel->SetOnlineAssetsEnabled(value);
        };
        onlineEnabled.Control = onlineToggle;
        onlineAssets.Fields.push_back(std::move(onlineEnabled));

        SettingsFieldDescriptor polyHaven;
        polyHaven.Label = "Poly Haven";
        polyHaven.SearchKeywords = "poly haven textures models hdri cc0";
        polyHaven.PrefKey = "onlineAssets.polyHaven";
        SettingsFieldDescriptor::ToggleField polyToggle;
        polyToggle.DefaultValue = true;
        polyToggle.Set = [assetsPanel](bool value)
        {
            if (assetsPanel)
                assetsPanel->SetPolyHavenEnabled(value);
        };
        polyHaven.Control = polyToggle;
        onlineAssets.Fields.push_back(std::move(polyHaven));

        SettingsFieldDescriptor textureResolution;
        textureResolution.Label = "Texture Resolution";
        textureResolution.SearchKeywords = "poly haven texture resolution download size";
        textureResolution.PrefKey = "onlineAssets.polyHavenTextureResolution";
        SettingsFieldDescriptor::DropdownField textureDropdown;
        textureDropdown.Options = {"1k", "2k", "4k", "8k"};
        textureDropdown.DefaultValue = "1k";
        textureResolution.Control = textureDropdown;
        onlineAssets.Fields.push_back(std::move(textureResolution));

        SettingsFieldDescriptor hdriResolution;
        hdriResolution.Label = "HDRI Resolution";
        hdriResolution.SearchKeywords = "poly haven hdri skybox resolution download size";
        hdriResolution.PrefKey = "onlineAssets.polyHavenHDRIResolution";
        SettingsFieldDescriptor::DropdownField hdriDropdown;
        hdriDropdown.Options = {"1k", "2k", "4k", "8k", "16k"};
        hdriDropdown.DefaultValue = "4k";
        hdriResolution.Control = hdriDropdown;
        onlineAssets.Fields.push_back(std::move(hdriResolution));

        Editor::EditorSettingsRegistry::Get().RegisterCategory(std::move(onlineAssets));
    }

    // Level of Detail owns its descriptor next to the settings it reads.
    Editor::RegisterLodSettingsCategory();
    // Game UI Scaling likewise; the game UI hosts read it back from the project.
    Editor::RegisterGameUIScaleSettingsCategory();
    // Experimental renderer switches own theirs, and apply them at startup.
    Editor::RegisterExperimentalRenderingSettingsCategory();
    // Interface (context-menu backend) likewise.
    Editor::RegisterInterfaceSettingsCategory();
    // DDGIVolume owns its inspector/hierarchy icon through the component-traits
    // registry rather than another branch in the built-in icon chains.
    Editor::RegisterDDGIVolumeComponentTraits();
    // Shared info-card background / outline / shadow on UI > Appearance.
    Editor::RegisterInfoCardAppearanceSettingsCategory();
    // Asset Preview (scroll-wheel dolly) registers declaratively and seeds
    // the thumbnail handler from preferences.
    Editor::RegisterAssetPreviewSettingsCategory();
    Editor::ApplyAssetPreviewSettingsFromPreferences();
    Editor::RegisterGraphSettingsCategory();

    // Apply initial value from preferences
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        bool atTop = true;
        if (prefs.TryGetBool("ui.smartFoldersAtTop", atTop))
        {
            assetsPanel->SetSmartFoldersAtTop(atTop);
            settingsPanel->SetSmartFoldersAtTopValue(atTop);
        }

        bool smartFoldersExpandedOnStartup = false;
        prefs.TryGetBool("ui.assets.smartFoldersExpandedOnStartup", smartFoldersExpandedOnStartup);
        assetsPanel->SetSmartFoldersExpandedOnStartup(smartFoldersExpandedOnStartup);
        settingsPanel->SetSmartFoldersExpandedOnStartupValue(smartFoldersExpandedOnStartup);

        bool foldersFirst = true;
        prefs.TryGetBool("ui.assets.foldersFirst", foldersFirst);
        assetsPanel->SetFoldersFirst(foldersFirst);
        settingsPanel->SetAssetsFoldersFirstValue(foldersFirst);

        bool expandFoldersOnLoad = false;
        prefs.TryGetBool("ui.assets.expandFoldersOnLoad", expandFoldersOnLoad);
        assetsPanel->SetExpandFoldersOnLoad(expandFoldersOnLoad);
        settingsPanel->SetAssetsExpandFoldersOnLoadValue(expandFoldersOnLoad);

        bool extraBottomToolbar = true;
        prefs.TryGetBool("ui.assetsExtraBottomViewToolbar", extraBottomToolbar);
        assetsPanel->SetExtraBottomViewToolbarEnabled(extraBottomToolbar);
        settingsPanel->SetAssetsExtraBottomViewToolbarValue(extraBottomToolbar);

        bool singleToggleIcon = true;
        prefs.TryGetBool("ui.assetsSingleViewToggleIcon", singleToggleIcon);
        assetsPanel->SetSingleViewToggleIconEnabled(singleToggleIcon);
        settingsPanel->SetAssetsSingleViewToggleIconValue(singleToggleIcon);

        bool showAssetsZoomSlider = true;
        prefs.TryGetBool("ui.assets.bottomToolbarShowZoomSlider", showAssetsZoomSlider);
        assetsPanel->SetBottomToolbarZoomSliderVisible(showAssetsZoomSlider);
        settingsPanel->SetAssetsBottomToolbarZoomSliderVisibleValue(showAssetsZoomSlider);
    }
}

void EditorApplication::InitializeInspectorSettings(SettingsPanel* settingsPanel)
{
    if (!settingsPanel)
        return;

    settingsPanel->SetOnHiDpiPlatformSettingsChanged([this]() { SyncHiDpiPlatformSettingsFromPreferences(); });

    // Toggle all inspector/settings disclosure chevrons, including nested foldouts.
    // Mounted panels are not DOM children of root/dockspace, so we apply the class
    // to each panel that contains inspector sections.
    auto applyCollapseArrowVisibility = [this](bool visible)
    {
        for (auto& p : m_PanelStorage)
        {
            UIElement* el = p.get();
            if (dynamic_cast<InspectorPanel*>(el) || dynamic_cast<SettingsPanel*>(el))
            {
                if (visible)
                    el->RemoveClass("inspector-hide-collapse-arrow");
                else
                    el->AddClass("inspector-hide-collapse-arrow");
            }
        }
    };
    settingsPanel->SetOnInspectorCollapseArrowVisibilityChanged(applyCollapseArrowVisibility);

    bool showArrow = false; // default: arrows hidden
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorShowCollapseArrow", showArrow);
        applyCollapseArrowVisibility(showArrow);
        settingsPanel->SetInspectorCollapseArrowVisibleValue(showArrow);
    }

    // Toggle component icon visibility in inspector section headers.
    auto applyComponentIconsVisibility = [this](bool visible)
    {
        for (auto& p : m_PanelStorage)
        {
            UIElement* el = p.get();
            if (dynamic_cast<InspectorPanel*>(el) || dynamic_cast<SettingsPanel*>(el))
            {
                if (visible)
                    el->RemoveClass("inspector-hide-component-icons");
                else
                    el->AddClass("inspector-hide-component-icons");
            }
        }
    };
    settingsPanel->SetOnInspectorComponentIconsVisibilityChanged(applyComponentIconsVisibility);

    bool showComponentIcons = true; // default: icons shown
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorShowComponentIcons", showComponentIcons);
        applyComponentIconsVisibility(showComponentIcons);
        settingsPanel->SetInspectorComponentIconsVisibleValue(showComponentIcons);
    }

    auto applyFilledSections = [this](bool enabled)
    {
        for (auto& p : m_PanelStorage)
        {
            UIElement* el = p.get();
            if (dynamic_cast<InspectorPanel*>(el) || dynamic_cast<SettingsPanel*>(el))
            {
                if (enabled)
                    el->AddClass("inspector-filled-sections");
                else
                    el->RemoveClass("inspector-filled-sections");
            }
        }
    };
    settingsPanel->SetOnInspectorFilledSectionsChanged(applyFilledSections);

    bool filledSections = false;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorFilledSections", filledSections);
        applyFilledSections(filledSections);
        settingsPanel->SetInspectorFilledSectionsValue(filledSections);
    }

    auto refreshInspectorInfoCards = [this](bool)
    {
        for (auto& p : m_PanelStorage)
        {
            if (auto* ip = dynamic_cast<InspectorPanel*>(p.get()))
                ip->RefreshCurrentTarget();
        }
    };
    settingsPanel->SetOnInspectorInfoCardsChanged(refreshInspectorInfoCards);

    bool showInfoCards = true;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorShowInfoCards", showInfoCards);
        settingsPanel->SetInspectorInfoCardsVisibleValue(showInfoCards);
    }

    // Wire SettingsPanel to control inspector "solo section" behaviour (only one section expanded).
    auto applyInspectorSoloSections = [this](bool enabled)
    {
        for (auto& p : m_PanelStorage)
        {
            if (auto* ip = dynamic_cast<InspectorPanel*>(p.get()))
            {
                InspectorPanel::ApplySoloSections(ip, enabled);
            }
        }
    };
    settingsPanel->SetOnInspectorSoloSectionsChanged(applyInspectorSoloSections);

    bool soloEnabled = false; // default: solo mode off
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorSoloSections", soloEnabled);
        applyInspectorSoloSections(soloEnabled);
        settingsPanel->SetInspectorSoloSectionsValue(soloEnabled);
    }

    // Wire SettingsPanel option to keep Transform section always expanded in solo mode.
    auto applyInspectorSoloKeepTransform = [this](bool enabled)
    {
        for (auto& p : m_PanelStorage)
        {
            if (auto* ip = dynamic_cast<InspectorPanel*>(p.get()))
            {
                ip->SetSoloKeepTransform(enabled);
            }
        }
    };
    settingsPanel->SetOnInspectorSoloKeepTransformChanged(applyInspectorSoloKeepTransform);

    bool soloKeepTransform = false; // default: off
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.inspectorSoloKeepTransform", soloKeepTransform);
        applyInspectorSoloKeepTransform(soloKeepTransform);
        settingsPanel->SetInspectorSoloKeepTransformValue(soloKeepTransform);
    }

    // Inspector toggle alignment (left/right/hidden)
    settingsPanel->SetOnInspectorToggleAlignChanged(
        [this](const std::string& v)
        {
            ApplyInspectorToggleAlign(v);
        });

    // VSync toggle: applies to the main window device only (editor preference, not per-project)
    settingsPanel->SetOnVsyncChanged([this](bool enabled)
    {
        if (!m_Windows.empty() && m_Windows[0]->renderCtx)
        {
            if (auto* dev = m_Windows[0]->renderCtx->GetDevice())
                dev->SetVsync(enabled);
        }
    });

    // The panel persists any project renderer edit before firing this shared
    // notification. One SettingsStore snapshot is then fanned out to every
    // window, matching startup and project-switch application.
    settingsPanel->SetOnProjectRenderSettingsChanged([this]()
    {
        ApplyProjectRenderSettingsToAllWindows();
    });
}

} // namespace GameEngine
