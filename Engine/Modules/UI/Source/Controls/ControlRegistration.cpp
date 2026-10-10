#include "UI/Controls/BaseField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/WeightedPane.h"

#include "UI/Controls/Accordion.h"
#include "UI/Controls/AccordionItem.h"
#include "UI/Controls/DockLeaf.h"
#include "UI/Controls/DockOverlay.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockTab.h"
#include "UI/Controls/DockTabBar.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/SplitView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/TreeView.h"

namespace GameEngine::UIRegistration
{
template<>
struct Parser<Dropdown::Mode>
{
    static Dropdown::Mode Parse(std::string_view s)
    {
        const std::string lower = ToLowerAscii(s);
        if (lower == "native" || lower == "os")
            return Dropdown::Mode::Native;
        return Dropdown::Mode::Ui;
    }
};

template<>
struct Parser<Accordion::Mode>
{
    static Accordion::Mode Parse(std::string_view s)
    {
        const std::string lower = ToLowerAscii(s);
        if (lower == "exclusive" || lower == "single")
            return Accordion::Mode::Exclusive;
        return Accordion::Mode::Multiple;
    }
};

namespace
{
// How many times the chains below have run. Nothing in the registry can answer that:
// RegisterFactory assigns by key, so a second definition overwrites every entry with an equal
// one and leaves all counts identical. See BuiltInControlDefinitionCount.
std::size_t s_definitionCount = 0;

// The one definition of the built-in tag set: every tag, alias and attribute binding the
// engine ships. Reached only through RegisterBuiltInControls(), which runs it once.
//
// Registration is a side effect of constructing a Registrar, which has no destructor, so the
// chains below are deliberately temporaries — nothing needs to outlive this call.
void DefineBuiltInControls()
{
    ++s_definitionCount;

    RegisterWithFactory<UIElement>("uielement", []()
                                   { return std::make_unique<UIElement>(); })
        .Attr("tabindex", &UIElement::SetTabIndex)
        .Attr("focusable", &UIElement::SetFocusable)
        .Attr("enabled", &UIElement::SetEnabled)
        .Attr("disabled", &UIElement::SetDisabled);

    Register<Label>("label")
        .Text(&Label::SetText)
        .Attr("text", &Label::SetText);

    // Public string field element. Uses TextField (string-based typed field)
    // and keeps legacy aliases for backwards compatibility.
    {
        auto reg = Register<TextField>("TextField");
        reg.Attr("value", &BaseField::SetValue);
        reg.Attr("selectallonmousefocus", &TextFieldBase<std::string>::SetSelectsAllOnMouseFocus);
        reg.TagAlias("textfield"); // legacy lowercase tag
        reg.TagAlias("input");     // legacy <input> alias
    }

    // Single-line editor embedded in TextField / numeric fields. Registration is required so
    // CSS type selectors (e.g. `.inspector-field TextInput`) resolve and match the inner node.
    Register<TextInput>("textinput");

    // Field controls: canonical PascalCase tags with lowercase/legacy aliases.
    // Use TagAlias for tag names; Attr::Alias remains for attribute name aliases.
    RegisterWithFactory<FloatField>("FloatField", []()
                                    { return std::make_unique<FloatField>(); })
        .TagAlias("floatfield");
    RegisterWithFactory<IntField>("IntField", []()
                                  { return std::make_unique<IntField>(); })
        .TagAlias("intfield");
    RegisterWithFactory<Vector3Field>("Vector3Field", []()
                                      { return std::make_unique<Vector3Field>(); })
        .TagAlias("vector3field");

    Register<TextArea>("textarea")
        .Attr("value", &BaseField::SetValue)
        .Attr("readonly", &TextArea::SetReadOnly);

    RegisterWithFactory<Scrollbar>("scrollbar", []()
                                   { return std::make_unique<Scrollbar>(Scrollbar::Orientation::Vertical); });

    RegisterWithFactory<TreeView>("treeview", []()
                                  { return std::make_unique<TreeView>(); });
    RegisterWithFactory<GridView>("gridview", []()
                                  { return std::make_unique<GridView>(); });

    Register<ScrollView>("scrollview");
    RegisterWithFactory<ListView>("listview", []()
                                  { return std::make_unique<ListView>(); });

    Register<DockspaceElement>("dockspace");

    Register<WeightedPane>("pane")
        .Attr("weight", &WeightedPane::SetFlexWeight, 1.0f);

    RegisterWithFactory<SplitView>("splitview", []()
                                   { return std::make_unique<SplitView>(); });
    RegisterWithFactory<Splitter>("splitter", []()
                                  { return std::make_unique<Splitter>(); });
    RegisterWithFactory<DockLeaf>("dockleaf", []()
                                  { return std::make_unique<DockLeaf>(); });
    RegisterWithFactory<DockTabBar>("docktabbar", []()
                                    { return std::make_unique<DockTabBar>(); });
    Register<DockTab>("docktab").Text(&DockTab::SetText).Attr("text", &DockTab::SetText);
    RegisterWithFactory<DockOverlay>("dockoverlay", []()
                                     { return std::make_unique<DockOverlay>(); });

    RegisterWithFactory<DockPanel>("dockpanel", []()
                                   { return std::make_unique<DockPanel>(""); })
        .Attr("title", &DockPanel::SetTitle);

    Register<Button>("button")
        .Text(&Button::SetText)
        .Attr("text", &Button::SetText)
        .Alias("value");

    RegisterWithFactory<Checkbox>("Checkbox", []()
                                  { return std::make_unique<Checkbox>(); })
        .Text(&Checkbox::SetText)
        .Attr("checked", &Checkbox::SetChecked)
        .Attr("text", &Checkbox::SetText)
        .TagAlias("checkbox");

    RegisterWithFactory<Toggle>("Toggle", []()
                                { return std::make_unique<Toggle>(); })
        .Attr("checked", &Toggle::SetChecked)
        .TagAlias("toggle");

    RegisterWithFactory<Slider>("Slider", []()
                                { return std::make_unique<Slider>(); })
        .Attr("min", &Slider::SetMin)
        .Attr("max", &Slider::SetMax)
        .Attr("value", &Slider::SetValue)
        .Attr("step", &Slider::SetStep)
        .Attr("ticks", &Slider::SetShowTicks)
        .Attr("valuebubble", &Slider::SetShowValueBubble)
        .Attr("centered", &Slider::SetCentered)
        .Attr("range", &Slider::SetRangeMode)
        .Attr("rangestart", &Slider::SetRangeStart)
        .TagAlias("slider");

    RegisterWithFactory<Dropdown>("Dropdown", []()
                                  { return std::make_unique<Dropdown>(); })
        .Attr("mode", &Dropdown::SetMode)
        .Attr("options", &Dropdown::SetOptionsFromString)
        .TagAlias("dropdown");

    RegisterWithFactory<Foldout>("Foldout", []()
                                 { return std::make_unique<Foldout>(); })
        .Attr("title", &Foldout::SetTitle)
        .Attr("icon", &Foldout::SetIconClass)
        .Attr("expanded", &Foldout::SetExpanded)
        .TagAlias("foldout");

    RegisterWithFactory<AccordionItem>("AccordionItem", []()
                                       { return std::make_unique<AccordionItem>(); })
        .Attr("title", &AccordionItem::SetTitle)
        .Attr("icon", &AccordionItem::SetIconClass)
        .Attr("expanded", &AccordionItem::SetExpanded)
        .TagAlias("accordionitem");

    RegisterWithFactory<Accordion>("Accordion", []()
                                   { return std::make_unique<Accordion>(); })
        .Attr("mode", &Accordion::SetMode)
        .Attr("autoexpandfirst", &Accordion::SetAutoExpandFirst)
        .TagAlias("accordion");

    RegisterWithFactory<SearchDialog>("SearchDialog", []()
                                      { return std::make_unique<SearchDialog>(); })
        .TagAlias("searchdialog");
}
} // namespace

void RegisterBuiltInControls()
{
    // Magic static: the tag set is defined exactly once per process, whichever caller gets
    // here first — this module's dynamic initialisation below, a UIManager constructor, or a
    // test — and concurrent first calls serialise on it.
    [[maybe_unused]] static const bool s_registered = (DefineBuiltInControls(), true);
}

std::size_t BuiltInControlDefinitionCount()
{
    return s_definitionCount;
}

namespace
{
// Parsing a document does not require a UIManager, so the built-ins have to be resolvable as
// soon as this module is loaded rather than when the first explicit caller runs.
[[maybe_unused]] const bool s_builtInControlsRegistered = (RegisterBuiltInControls(), true);
} // namespace

} // namespace GameEngine::UIRegistration
