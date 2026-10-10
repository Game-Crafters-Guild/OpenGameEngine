#include "Panels/SettingsPanel.h"

#include "Editor/EditorTreeTitleIconVars.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/StyleProperties.h"

#include <cmath>
#include <memory>
#include <string>

namespace GameEngine
{
void SettingsPanel::ApplyHierarchyTreeIconSize(float px)
{
    const float clamped = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
    m_HierarchyTreeIconSizeValue = clamped;
    if (m_OnHierarchyTreeIconSizeChanged)
        m_OnHierarchyTreeIconSizeChanged(clamped);

    const float desiredRow = DeriveEditorTreeRowHeightFromIconSize(clamped);
    if (m_HierarchyTreeRowHeightSlider)
        m_HierarchyTreeRowHeightSlider->SetValue(desiredRow);
    if (m_OnHierarchyTreeRowHeightChanged)
        m_OnHierarchyTreeRowHeightChanged(desiredRow);
    UpdateTreePaneHeight();
}

void SettingsPanel::ApplyAssetsTreeIconSize(float px)
{
    const float clamped = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorAssetsTreeIconSizePx);
    m_AssetsTreeIconSizeValue = clamped;
    if (m_OnAssetsTreeIconSizeChanged)
        m_OnAssetsTreeIconSizeChanged(clamped);

    const float desiredRow = DeriveEditorTreeRowHeightFromIconSize(clamped);
    if (m_AssetsTreeRowHeightSlider)
        m_AssetsTreeRowHeightSlider->SetValue(desiredRow);
    if (m_OnAssetsTreeRowHeightChanged)
        m_OnAssetsTreeRowHeightChanged(desiredRow);
    UpdateTreePaneHeight();
}

void SettingsPanel::SetHierarchyTreeIconSizeValue(float px)
{
    const float clamped = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
    m_HierarchyTreeIconSizeUpdating = true;
    if (m_HierarchyTreeIconSizeSlider)
        m_HierarchyTreeIconSizeSlider->SetValue(clamped);
    m_HierarchyTreeIconSizeUpdating = false;
    if (m_HierarchyTreeIconSizeField)
        m_HierarchyTreeIconSizeField->SetValue(clamped);
    ApplyHierarchyTreeIconSize(clamped);
}

void SettingsPanel::SetAssetsTreeIconSizeValue(float px)
{
    const float clamped = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorAssetsTreeIconSizePx);
    m_AssetsTreeIconSizeUpdating = true;
    if (m_AssetsTreeIconSizeSlider)
        m_AssetsTreeIconSizeSlider->SetValue(clamped);
    m_AssetsTreeIconSizeUpdating = false;
    if (m_AssetsTreeIconSizeField)
        m_AssetsTreeIconSizeField->SetValue(clamped);
    ApplyAssetsTreeIconSize(clamped);
}

void SettingsPanel::SetHierarchyTreeIconSizeValueAndPersist(float px)
{
    SetHierarchyTreeIconSizeValue(px);
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.SetDouble("ui.hierarchyTreeIconSize", static_cast<double>(m_HierarchyTreeIconSizeValue));
    prefs.SetDouble("ui.hierarchyTreeRowHeight",
                    static_cast<double>(DeriveEditorTreeRowHeightFromIconSize(m_HierarchyTreeIconSizeValue)));
    prefs.Save(&err);
}

void SettingsPanel::SetAssetsTreeIconSizeValueAndPersist(float px)
{
    SetAssetsTreeIconSizeValue(px);
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.SetDouble("ui.assetsTreeIconSize", static_cast<double>(m_AssetsTreeIconSizeValue));
    prefs.SetDouble("ui.assetsTreeRowHeight",
                    static_cast<double>(DeriveEditorTreeRowHeightFromIconSize(m_AssetsTreeIconSizeValue)));
    prefs.Save(&err);
}

void SettingsPanel::CreateTreeScalingSettingsForTarget(bool forHierarchy)
{
    if (!m_ContentBody)
        return;

    const char* rowPref = forHierarchy ? "ui.hierarchyTreeRowHeight" : "ui.assetsTreeRowHeight";
    const char* indentPref = forHierarchy ? "ui.hierarchyTreeChildIndent" : "ui.assetsTreeChildIndent";
    const char* iconPref = forHierarchy ? "ui.hierarchyTreeIconSize" : "ui.assetsTreeIconSize";

    const std::string rowPrefStr(rowPref);
    const std::string indentPrefStr(indentPref);
    const std::string iconPrefStr(iconPref);

    auto subHeader = std::make_unique<Label>();
    subHeader->SetText(forHierarchy ? "Hierarchy tree" : "Assets browser tree");
    subHeader->AddClass("settings-section-header");
    m_ContentBody->AddContent(std::move(subHeader));

    Slider** rowSliderSlot = forHierarchy ? &m_HierarchyTreeRowHeightSlider : &m_AssetsTreeRowHeightSlider;
    Label** rowLabelSlot = forHierarchy ? &m_HierarchyTreeRowHeightLabel : &m_AssetsTreeRowHeightLabel;
    Slider** indentSliderSlot =
        forHierarchy ? &m_HierarchyTreeChildIndentSlider : &m_AssetsTreeChildIndentSlider;
    Label** indentLabelSlot =
        forHierarchy ? &m_HierarchyTreeChildIndentLabel : &m_AssetsTreeChildIndentLabel;
    Slider** iconSliderSlot = forHierarchy ? &m_HierarchyTreeIconSizeSlider : &m_AssetsTreeIconSizeSlider;
    Label** iconLabelSlot = forHierarchy ? &m_HierarchyTreeIconSizeLabel : &m_AssetsTreeIconSizeLabel;
    FloatField** iconFieldSlot =
        forHierarchy ? &m_HierarchyTreeIconSizeField : &m_AssetsTreeIconSizeField;
    float* iconValueSlot = forHierarchy ? &m_HierarchyTreeIconSizeValue : &m_AssetsTreeIconSizeValue;
    bool* iconUpdatingSlot = forHierarchy ? &m_HierarchyTreeIconSizeUpdating : &m_AssetsTreeIconSizeUpdating;

    // Row height
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Row height");
        label->AddClass("settings-row-label");
        *rowLabelSlot = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        *rowSliderSlot = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(kMinEditorTreeRowHeightPx);
        slider->SetMax(kMaxEditorTreeRowHeightPx);
        slider->SetStep(1.0f);

        float rowHeight = 20.0f;
        if (forHierarchy)
        {
            if (m_OnGetHierarchyTreeRowHeight)
            {
                rowHeight = std::clamp(m_OnGetHierarchyTreeRowHeight(), kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                double stored = rowHeight;
                if (!prefs.TryGetDouble(rowPref, stored) || std::fabs(stored - rowHeight) > 0.01)
                {
                    prefs.SetDouble(rowPref, rowHeight);
                    if (!prefs.Save(&err) && !err.empty())
                        Logger::Log::Error("Failed to sync hierarchy tree row height to preferences: {}", err);
                }
            }
            else
            {
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                double stored = rowHeight;
                if (prefs.TryGetDouble(rowPref, stored))
                    rowHeight = static_cast<float>(stored);
            }
        }
        else
        {
            if (m_OnGetAssetsTreeRowHeight)
            {
                rowHeight = std::clamp(m_OnGetAssetsTreeRowHeight(), kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                double stored = rowHeight;
                if (!prefs.TryGetDouble(rowPref, stored) || std::fabs(stored - rowHeight) > 0.01)
                {
                    prefs.SetDouble(rowPref, rowHeight);
                    if (!prefs.Save(&err) && !err.empty())
                        Logger::Log::Error("Failed to sync assets tree row height to preferences: {}", err);
                }
            }
            else
            {
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                double stored = rowHeight;
                if (prefs.TryGetDouble(rowPref, stored))
                    rowHeight = static_cast<float>(stored);
            }
        }

        rowHeight = std::clamp(rowHeight, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
        slider->SetValue(rowHeight);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(rowHeight);
        FloatField* valueFieldPtr = valueField.get();
        Slider* rowSliderPtr = *rowSliderSlot;
        slider->SetOnValueChanging([this, forHierarchy, valueFieldPtr](const float& v)
                                   {
                                       const float clamped =
                                           std::clamp(v, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
                                       valueFieldPtr->SetValue(clamped);
                                       if (forHierarchy)
                                       {
                                           if (m_OnHierarchyTreeRowHeightChanged)
                                               m_OnHierarchyTreeRowHeightChanged(clamped);
                                       }
                                       else
                                       {
                                           if (m_OnAssetsTreeRowHeightChanged)
                                               m_OnAssetsTreeRowHeightChanged(clamped);
                                       }
                                       UpdateTreePaneHeight();
                                   });

        slider->SetOnValueChanged([this, forHierarchy, valueFieldPtr, rowPrefStr](const float& value)
                                  {
                                      const float clamped =
                                          std::clamp(value, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
                                      valueFieldPtr->SetValue(clamped);
                                      if (forHierarchy)
                                      {
                                          if (m_OnHierarchyTreeRowHeightChanged)
                                              m_OnHierarchyTreeRowHeightChanged(clamped);
                                      }
                                      else
                                      {
                                          if (m_OnAssetsTreeRowHeightChanged)
                                              m_OnAssetsTreeRowHeightChanged(clamped);
                                      }
                                      UpdateTreePaneHeight();
                                      auto prefs = Editor::OpenEditorPreferences();
                                      std::string err;
                                      prefs.Load(&err);
                                      prefs.SetDouble(rowPrefStr, clamped);
                                      if (!prefs.Save(&err) && !err.empty())
                                          Logger::Log::Error("Failed to save tree row height: {}", err);
                                  });

        valueField->SetOnValueChanged(
            [this, forHierarchy, sliderPtr = rowSliderPtr, valueFieldPtr, rowPrefStr](const float& value)
            {
                float clamped = std::clamp(value, kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
                sliderPtr->SetValue(clamped);
                valueFieldPtr->SetValue(clamped);
                if (forHierarchy)
                {
                    if (m_OnHierarchyTreeRowHeightChanged)
                        m_OnHierarchyTreeRowHeightChanged(clamped);
                }
                else
                {
                    if (m_OnAssetsTreeRowHeightChanged)
                        m_OnAssetsTreeRowHeightChanged(clamped);
                }
                UpdateTreePaneHeight();
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.SetDouble(rowPrefStr, clamped);
                prefs.Save(&err);
            });

        constexpr float kDefaultTreeRowHeight = 20.0f;
        InspectorDrag::SetupLabelDragSlider(
            *rowLabelSlot, rowSliderPtr, nullptr, nullptr, kDefaultTreeRowHeight);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Child indent
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Child indent");
        label->AddClass("settings-row-label");
        *indentLabelSlot = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        *indentSliderSlot = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.0f);
        slider->SetMax(32.0f);
        slider->SetStep(1.0f);

        float childIndent = 16.0f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = childIndent;
            if (prefs.TryGetDouble(indentPref, stored))
                childIndent = static_cast<float>(stored);
        }
        slider->SetValue(childIndent);

        auto applyIndent = [this, forHierarchy](float value)
        {
            if (forHierarchy)
            {
                if (m_OnHierarchyTreeChildIndentChanged)
                    m_OnHierarchyTreeChildIndentChanged(value);
            }
            else
            {
                if (m_OnAssetsTreeChildIndentChanged)
                    m_OnAssetsTreeChildIndentChanged(value);
            }
            UpdateTreePaneHeight();
        };

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(childIndent);
        FloatField* valueFieldPtr = valueField.get();
        Slider* indentSliderPtr = *indentSliderSlot;

        slider->SetOnValueChanging([valueFieldPtr, applyIndent](const float& v)
                                   {
                                       valueFieldPtr->SetValue(v);
                                       applyIndent(v);
                                   });
        slider->SetOnValueChanged([applyIndent, valueFieldPtr, indentPrefStr](const float& value)
                                  {
                                      valueFieldPtr->SetValue(value);
                                      applyIndent(value);
                                      auto prefs = Editor::OpenEditorPreferences();
                                      std::string err;
                                      prefs.Load(&err);
                                      prefs.SetDouble(indentPrefStr, value);
                                      prefs.Save(&err);
                                  });
        valueField->SetOnValueChanged(
            [sliderPtr = indentSliderPtr, valueFieldPtr, applyIndent, indentPrefStr](const float& value)
            {
                float clamped = std::max(0.0f, std::min(32.0f, value));
                sliderPtr->SetValue(clamped);
                valueFieldPtr->SetValue(clamped);
                applyIndent(clamped);
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.SetDouble(indentPrefStr, clamped);
                prefs.Save(&err);
            });

        constexpr float kDefaultTreeChildIndent = 16.0f;
        InspectorDrag::SetupLabelDragSlider(
            *indentLabelSlot, indentSliderPtr, nullptr, nullptr, kDefaultTreeChildIndent);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Icon size
    {
        const float iconMax =
            forHierarchy ? kMaxEditorHierarchyTreeIconSizePx : kMaxEditorAssetsTreeIconSizePx;

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->AddClass("settings-row-label");
        *iconLabelSlot = label.get();
        label->SetText("Icon size");
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        *iconSliderSlot = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(kMinEditorTreeIconSizePx);
        slider->SetMax(iconMax);
        slider->SetStep(1.0f);

        float iconSize = *iconValueSlot;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = iconSize;
            if (prefs.TryGetDouble(iconPref, stored))
                iconSize = std::clamp(static_cast<float>(stored), kMinEditorTreeIconSizePx, iconMax);
        }
        *iconValueSlot = iconSize;
        slider->SetValue(iconSize);
        if (forHierarchy)
            ApplyHierarchyTreeIconSize(iconSize);
        else
            ApplyAssetsTreeIconSize(iconSize);

        auto valueField = std::make_unique<FloatField>();
        *iconFieldSlot = valueField.get();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(iconSize);
        FloatField* valueFieldPtr = valueField.get();
        Slider* iconSliderPtr = *iconSliderSlot;

        auto applySize = [this, forHierarchy, valueFieldPtr, iconUpdatingSlot, iconValueSlot, iconMax](float value)
        {
            if (*iconUpdatingSlot)
                return;
            const float clamped = std::clamp(value, kMinEditorTreeIconSizePx, iconMax);
            *iconValueSlot = clamped;
            if (forHierarchy)
                ApplyHierarchyTreeIconSize(clamped);
            else
                ApplyAssetsTreeIconSize(clamped);
            valueFieldPtr->SetValue(clamped);
        };

        slider->SetOnValueChanging([applySize](const float& value) { applySize(value); });
        slider->SetOnValueChanged([applySize, iconPrefStr, rowPrefStr, iconMax](const float& value)
                                  {
                                      const float clamped = std::clamp(value, kMinEditorTreeIconSizePx, iconMax);
                                      applySize(clamped);
                                      auto prefs = Editor::OpenEditorPreferences();
                                      std::string err;
                                      prefs.Load(&err);
                                      prefs.SetDouble(iconPrefStr, clamped);
                                      prefs.SetDouble(rowPrefStr,
                                                      static_cast<double>(
                                                          DeriveEditorTreeRowHeightFromIconSize(clamped)));
                                      prefs.Save(&err);
                                  });

        valueField->SetOnValueChanged(
            [this, forHierarchy, sliderPtr = iconSliderPtr, valueFieldPtr, iconUpdatingSlot, iconValueSlot,
             iconPrefStr, rowPrefStr, iconMax](const float& value)
            {
                const float clamped = std::clamp(value, kMinEditorTreeIconSizePx, iconMax);
                *iconUpdatingSlot = true;
                sliderPtr->SetValue(clamped);
                *iconUpdatingSlot = false;
                valueFieldPtr->SetValue(clamped);
                *iconValueSlot = clamped;
                if (forHierarchy)
                    ApplyHierarchyTreeIconSize(clamped);
                else
                    ApplyAssetsTreeIconSize(clamped);
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.SetDouble(iconPrefStr, clamped);
                prefs.SetDouble(rowPrefStr,
                                static_cast<double>(DeriveEditorTreeRowHeightFromIconSize(clamped)));
                prefs.Save(&err);
            });

        constexpr float kDefaultTreeIconSize = 20.0f;
        InspectorDrag::SetupLabelDragSlider(
            *iconLabelSlot, iconSliderPtr, nullptr, nullptr, kDefaultTreeIconSize);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }
}

} // namespace GameEngine
