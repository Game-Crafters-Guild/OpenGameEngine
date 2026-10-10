#pragma once

// Answers one question and nothing else: given THIS CSS, THIS element tree and
// THIS content scale, what does the engine resolve, lay out and emit?
//
// Everything an editor screenshot drags in is absent by construction. The
// manager is built over a headless device with exactly one stylesheet — the
// string the test passed — no AssetManager, no project mount, no runtime style
// injection, and no input delivered, so no element is hovered, focused or
// active unless the test puts it there itself. A number read back here is
// attributable to the CSS the test wrote and to nothing else.
//
// Two spaces are in play and mixing them is the defect class this fixture
// exists to expose. CSS and Yoga work in LOGICAL px; every primitive is emitted
// in PHYSICAL px (logical * contentScale, UIManager_PrimitiveGen.cpp). Every
// accessor below states which space it returns.

#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"

#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::UITesting
{

// A rect in physical px — the space primitives are emitted in.
struct PhysicalRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

class IsolatedUIFixture
{
  public:
    IsolatedUIFixture() = default;
    IsolatedUIFixture(const IsolatedUIFixture&) = delete;
    IsolatedUIFixture& operator=(const IsolatedUIFixture&) = delete;

    ~IsolatedUIFixture()
    {
        m_Rg.reset();
        m_Ui.reset();
    }

    // Builds the manager, parses `xml` into the root tree and `css` into the one
    // and only stylesheet, then settles style, layout and primitive generation
    // at `contentScale`.
    //
    // Returns false both when the host has no Vulkan device and when the test's
    // own XML or CSS failed to parse. DeviceAvailable() separates the skip case
    // from the authoring error; Diagnostic() names whichever happened.
    bool Build(float contentScale, const std::string& xml, const std::string& css)
    {
        m_ContentScale = contentScale;

        m_Device = SharedHeadlessDevice();
        if (!m_Device)
        {
            m_Diagnostic = "no Vulkan device";
            return false;
        }

        // Constructing the manager registers the built-in control factories,
        // which XMLParser needs before it can turn <label>/<textarea> into
        // controls rather than plain UIElements.
        m_Platform = std::make_unique<FixedScalePlatform>(contentScale);
        m_Ui = std::make_unique<UIManager>(m_Device);
        m_Ui->SetPlatform(m_Platform.get());

        // The override is PHYSICAL: UIManager hands Yoga viewport/contentScale.
        // Scaling it here keeps the LOGICAL layout space identical at every
        // content scale, so a geometry difference between two scales is the
        // scale mapping and never a differently-sized viewport.
        m_Ui->SetLayoutSizeOverride(
            static_cast<uint32_t>(std::lround(kLogicalViewportW * contentScale)),
            static_cast<uint32_t>(std::lround(kLogicalViewportH * contentScale)));

        std::unique_ptr<UIElement> root;
        if (!UIParsing::XMLParser::ParseLayoutFromString(xml, root, kXmlSourceName) || !root)
        {
            m_Diagnostic = "XML parse failed";
            return false;
        }
        m_Ui->SetRoot(std::move(root));

        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(css, sheet))
        {
            m_Diagnostic = "CSS parse failed";
            return false;
        }
        sheet.SourceName = kCssSourceName;
        m_Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));

        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        for (int i = 0; i < kSettleFrames; ++i)
        {
            m_Ui->Update(kFrameSeconds, /*interactive=*/true);
            DriveUiRender(*m_Ui, *m_Rg);
        }
        return true;
    }

    // Re-settles style, layout and primitive generation after the test changed
    // something Build could not express in CSS or XML — focus, a selection
    // range, a programmatic style override. Without it the primitives read back
    // are still the ones Build produced.
    void Settle()
    {
        if (!m_Ui || !m_Rg)
            return;
        for (int i = 0; i < kSettleFrames; ++i)
        {
            m_Ui->Update(kFrameSeconds, /*interactive=*/true);
            DriveUiRender(*m_Ui, *m_Rg);
        }
    }

    // One update/render pair at an explicit delta. Settle() also advances the
    // UI clock, but only in fixed 16 ms steps; a test about anything the clock
    // drives needs to choose the step itself.
    void PumpSeconds(float deltaSeconds)
    {
        if (!m_Ui || !m_Rg)
            return;
        m_Ui->Update(deltaSeconds, /*interactive=*/true);
        DriveUiRender(*m_Ui, *m_Rg);
    }

    // One update/render pair at the same fixed step Settle() advances by. A
    // per-frame measurement wants the standard frame rather than a chosen delta,
    // and it has to be Settle()'s step or the warmup frames and the measured
    // frames are not the same workload.
    void StepFrame() { PumpSeconds(kFrameSeconds); }

    // A character delivered through the manager, which is the path a keystroke
    // takes to the focused control.
    void TypeChar(unsigned int codepoint)
    {
        if (!m_Ui)
            return;
        m_Ui->OnChar(codepoint);
        Settle();
    }

    bool DeviceAvailable() const { return m_Device != nullptr; }
    const std::string& Diagnostic() const { return m_Diagnostic; }

    UIManager& Manager() const { return *m_Ui; }

    // The element with this id, or null when absent.
    UIElement* Element(const std::string& id) const { return Find(id); }

    // Tab to the next focusable. Tab, and a focus move a key handler makes,
    // raise the keyboard-focus flag that makes :focus-visible match;
    // SetFocusById outside a key's dispatch does not, so going through
    // UIManager::OnKey rather than setting focus directly is the point here.
    void FocusViaTab()
    {
        m_Ui->OnKey(kKeyCodeTab, kKeyActionPress, 0);
        Settle();
    }

    // A press/release over the element's centre, so focus is assigned by the
    // real pointer path: :focus matches afterwards and :focus-visible does not.
    // Returns false when the id is absent.
    bool FocusViaClick(const std::string& id)
    {
        const UIElement* el = Find(id);
        if (!el)
            return false;
        // OnMouseMove takes LOGICAL px, the space Yoga lays out in.
        const float cx = el->GetLayoutX() + el->GetLayoutWidth() * 0.5f;
        const float cy = el->GetLayoutY() + el->GetLayoutHeight() * 0.5f;
        m_Ui->OnMouseMove(cx, cy);
        m_Ui->Update(kFrameSeconds, /*interactive=*/true);
        m_Ui->OnMouseButton(0, true);
        m_Ui->Update(kFrameSeconds, /*interactive=*/true);
        m_Ui->OnMouseButton(0, false);
        Settle();
        return true;
    }

    // The cascade's answer for this element, or null when the id is absent.
    const ResolvedStyle* Style(const std::string& id) const
    {
        const UIElement* el = Find(id);
        return el ? &el->GetResolvedStyle() : nullptr;
    }

    // Border box in PHYSICAL px — same space, same origin as the primitives.
    PhysicalRect BorderBox(const std::string& id) const
    {
        const UIElement* el = Find(id);
        if (!el)
            return {};
        const float cs = m_ContentScale;
        return {el->GetLayoutX() * cs, el->GetLayoutY() * cs, el->GetLayoutWidth() * cs,
                el->GetLayoutHeight() * cs};
    }

    // Everything the element emitted this frame, in emission order.
    std::vector<UI::UIPrimitive> Primitives(const std::string& id) const
    {
        std::vector<UI::UIPrimitive> out;
        const UIElement* el = Find(id);
        if (!el || !m_Ui)
            return out;
        for (uint16_t i = 0;; ++i)
        {
            const UI::UIPrimitive* p = m_Ui->PeekPrimitiveForTesting(*el, i);
            if (!p)
                break;
            out.push_back(*p);
        }
        return out;
    }

    std::vector<UI::UIPrimitive> Primitives(const std::string& id, UI::PrimitiveMode mode) const
    {
        std::vector<UI::UIPrimitive> out;
        for (const UI::UIPrimitive& p : Primitives(id))
        {
            if (UI::GetMode(p.ModeAndFlags) == mode)
                out.push_back(p);
        }
        return out;
    }

    // Family name the element's `font-family` actually resolved to. A test
    // whose expected numbers came from one face must check this: with the
    // staged font missing, the resolver silently falls through to a system face
    // and every measurement is taken against the wrong outlines.
    std::string ResolvedFontFamily(const std::string& id) const
    {
        const ResolvedStyle* style = Style(id);
        if (!style || !m_Ui)
            return {};
        Rendering::Text::FontAtlas* font = m_Ui->ResolveFontForStyle(*style);
        if (!font)
            return {};
        const auto info = font->GetFaceDebugInfo();
        return info ? info->family : std::string{};
    }

  private:
    UIElement* Find(const std::string& id) const
    {
        UIElement* root = m_Ui ? m_Ui->GetRootElement() : nullptr;
        return root ? root->FindById(id) : nullptr;
    }

    // Logical viewport, held constant across content scales (see Build).
    static constexpr uint32_t kLogicalViewportW = 800u;
    static constexpr uint32_t kLogicalViewportH = 600u;

    // Style resolution, Yoga solve and primitive generation each settle over
    // successive frames — font resolution and deferred relayout land a frame
    // after the change that caused them. Four frames is what the sibling
    // text-geometry fixtures use to reach a fixed point.
    static constexpr int kSettleFrames = 4;
    static constexpr float kFrameSeconds = 0.016f;

    static constexpr const char* kXmlSourceName = "IsolatedUIFixture.xml";
    static constexpr const char* kCssSourceName = "IsolatedUIFixture.css";

    static constexpr int kKeyCodeTab = Input::kKeyCode_Tab;
    static constexpr int kKeyActionPress = 1; // GLFW_PRESS

    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    std::unique_ptr<FixedScalePlatform> m_Platform;
    Rendering::IDevice* m_Device = nullptr;
    std::unique_ptr<UIManager> m_Ui;
    std::unique_ptr<UiRgHarness> m_Rg;
    float m_ContentScale = 1.0f;
    std::string m_Diagnostic;
};

} // namespace GameEngine::UITesting
