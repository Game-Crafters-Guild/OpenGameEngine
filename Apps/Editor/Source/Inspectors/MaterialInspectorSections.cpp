#include "Inspectors/MaterialInspectorSections.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"

namespace GameEngine::Editor
{

std::unique_ptr<Foldout> MakeMaterialPropertiesFoldout()
{
    auto foldout = std::make_unique<Foldout>();
    foldout->SetTitle("Material Properties");
    foldout->AddClass("material-properties-foldout");
    foldout->AddClass("rp-foldout");
    foldout->GetHeader()->AddClass("material-properties-header");
    foldout->GetContentContainer()->AddClass("material-properties-content");
    foldout->SetExpanded(true);
    return foldout;
}

std::unique_ptr<Foldout> MakeMaterialSection(const std::string& title, bool expanded)
{
    auto section = std::make_unique<Foldout>();
    section->SetTitle(title);
    section->AddClass("rp-foldout");
    section->GetHeader()->AddClass("material-section-header");
    section->GetContentContainer()->AddClass("material-section-content");
    section->SetExpanded(expanded);
    return section;
}

Label* AddMaterialLine(UIElement* parent, const std::string& text)
{
    auto line = std::make_unique<Label>();
    line->AddClass("inspector-text");
    line->AddClass("material-line");
    line->SetText(text);
    Label* raw = line.get();
    parent->AddChild(std::move(line));
    return raw;
}

Label* AddMaterialWarning(UIElement* parent, const std::string& text)
{
    // Not an inspector-text line: the asset view softens that class's colour with a rule more
    // specific than the warning's own, which would turn the warning grey.
    auto line = std::make_unique<Label>();
    line->AddClass("inspector-warning");
    line->AddClass("material-warning");
    line->SetText(text);
    Label* raw = line.get();
    parent->AddChild(std::move(line));
    return raw;
}

void MakeMaterialRowInactive(const InspectorDrag::SliderWithFloatValueRow& row)
{
    if (row.Row)
        row.Row->SetEnabled(false);
    if (row.Label)
        row.Label->SetEnabled(false);
    if (row.Slider)
        row.Slider->SetEnabled(false);
    if (row.ValueField)
        row.ValueField->SetEnabled(false);
}

} // namespace GameEngine::Editor
