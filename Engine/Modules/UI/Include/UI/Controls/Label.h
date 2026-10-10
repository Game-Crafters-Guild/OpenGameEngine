#pragma once

#include <string>
#include "UI/ITextMeasurable.h"
#include "UI/UIElement.h"
namespace GameEngine {

class Label : public UIElement, public ITextMeasurable {
public:
    Label() { m_TextMeasurable = this; }

    // Text API specific to Label
    const std::string& GetText() const { return m_Text; }
    void SetText(const std::string& t);
    // For frequently-updated debug/perf overlays where the label's Yoga layout box
    // is known to be stable (e.g. fixed width / absolute positioned), allow a
    // paint-only update that avoids triggering a Yoga relayout.
	
		    // Provide intrinsic text measurement so UIManager/Yoga can size labels
		    // without knowing about Label specifically.
		    void GetTextMeasureInfo(ITextMeasurable::TextMeasureInfo& info,
		                           const ResolvedStyle& style) const override
	    {
	        (void)style;
	        info.HasText = !m_Text.empty();
	        info.Text = m_Text;
	        info.ExplicitFontSize = 0.0f; // use style.fontSize
	    }

	    bool AllowWrapForMeasure() const override { return true; }

	    // Expose label text to generic UIManager helpers (geometry overrides,
	    // debug overlays) without requiring it to know about Label.
	    const std::string& GetTextContent() const override { return m_Text; }

private:
    std::string m_Text;
};

} // namespace GameEngine

