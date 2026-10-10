// Text positioning, caret geometry and hit-testing correctness for
// TextInput-backed fields, exercised through the real UIManager pipeline
// (style resolution -> Yoga layout -> primitive generation) at several
// content scales.
//
// Three independent code paths compute where text sits, and a defect in any
// one of them shows up as "the caret is not where the glyphs are" or "clicking
// lands on the wrong character":
//
//   1. EmitTextPrimitives      places the glyph quads
//   2. BuildTextInputOverlays  places the caret bar and selection highlight
//   3. TextInput::BuildPointerGeometry maps a mouse X back to a caret index
//
// The tests below pin the agreement between them. The caret primitive is the
// probe: it is the one thing the renderer emits whose X is exactly a caret
// position, so feeding its X back through hit-testing closes the round trip
// end to end, including the logical-px -> physical-px conversion that a DPI
// bug would corrupt.

#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/TextField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"
#include "UI/UIPrimitive.h"

#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::UITesting::FixedScalePlatform;

namespace
{

// Content scales worth covering: 1.0 (no scaling), the two fractional Windows
// steps where a scale applied twice or truncated inconsistently shows up, and
// 2.0 (Retina).
constexpr float kContentScales[] = {1.0f, 1.25f, 1.5f, 2.0f};

// Key actions as UIManager::OnKey receives them from the platform layer.
constexpr int kKeyActionRelease = 0; // GLFW_RELEASE
constexpr int kKeyActionPress = 1;   // GLFW_PRESS

struct FieldGeometry
{
    // All values in physical px, matching the primitive stream.
    float ContentLeft = 0.0f;
    float ContentTop = 0.0f;
    float ContentWidth = 0.0f;
    float ContentHeight = 0.0f;
};

// A single field in a fixed-size root, styled from CSS so the whole
// resolve/layout path runs exactly as it does in the editor. Init builds the
// free-text TextField the geometry tests use; InitValueField builds a numeric
// field instead, for the focus-policy tests that compare the two.
struct TextFieldFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    std::unique_ptr<FixedScalePlatform> Platform;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    TextField* Field = nullptr;      // set by Init; null for a value field
    IntField* ValueField = nullptr;  // set by InitValueField; null otherwise
    UIElement* FocusTarget = nullptr;
    TextInput* Input = nullptr;
    float ContentScale = 1.0f;

    // fieldWidth/fieldHeight/fontSize/padding are CSS-logical px.
    // `lineHeight` is a CSS line-height declaration for the editor (e.g. "30px").
    // Null leaves it unset, i.e. `normal`: the line box IS the font's own height,
    // so the half-leading is zero and anything that seats the font box inside a
    // wider line box goes unexercised.
    bool Init(float contentScale, float fontSize, float fieldWidth, float fieldHeight,
              const std::string& text, const char* textAlign = "left",
              float padding = 4.0f, const char* lineHeight = nullptr)
    {
        auto field = std::make_unique<TextField>();
        Field = field.get();
        if (!Build(std::move(field), contentScale, fontSize, fieldWidth, fieldHeight,
                   textAlign, padding, lineHeight))
            return false;
        Field->SetValue(text);
        return Settle();
    }

    // Numeric value field in the same root and CSS. Value fields keep
    // select-all on mouse focus, so the policy tests need one to hold the
    // free-text behaviour against.
    bool InitValueField(float contentScale, float fontSize, float fieldWidth,
                        float fieldHeight, int value)
    {
        auto field = std::make_unique<IntField>();
        ValueField = field.get();
        if (!Build(std::move(field), contentScale, fontSize, fieldWidth, fieldHeight,
                   "left", 4.0f))
            return false;
        ValueField->SetValue(value);
        return Settle();
    }

    bool Build(std::unique_ptr<UIElement> field, float contentScale, float fontSize,
               float fieldWidth, float fieldHeight, const char* textAlign, float padding,
               const char* lineHeight = nullptr)
    {
        ContentScale = contentScale;
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        FocusTarget = field.get();
        field->SetId("field");
        root->AddChild(std::move(field));

        Platform = std::make_unique<FixedScalePlatform>(contentScale);
        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetPlatform(Platform.get());
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() /
                         ("ui_text_geometry_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 600px; height: 200px; }\n"
              << "#field { width: " << fieldWidth << "px; height: " << fieldHeight << "px; "
              << "font-size: " << fontSize << "px; }\n"
              << ".field-editor { width: 100%; height: 100%; padding: 0 " << padding << "px; "
              << "border-width: 0px; text-align: " << textAlign << "; "
              << (lineHeight ? ("line-height: " + std::string(lineHeight) + "; ") : std::string())
              << "}\n";
        }
        return Ui->AttachStyleFromFile(css.string());
    }

    bool Settle()
    {
        Rg = std::make_unique<UiRgHarness>(Dev);

        for (int i = 0; i < 4; ++i)
            Pump();

        Input = FindFirstTextInput(FocusTarget);
        return Input != nullptr;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    // Programmatic focus. UIManager reports this as not-from-keyboard, so it
    // follows the same select-all policy a mouse press does.
    void Focus()
    {
        Ui->FocusElement(FocusTarget);
        Pump();
        Pump();
    }

    // Tab focus, the one path that selects the whole value on every field.
    void FocusViaTab()
    {
        Ui->OnKey(Input::kKeyCode_Tab, kKeyActionPress, 0);
        Pump();
        Pump();
    }

    void Blur()
    {
        Ui->SetFocusById("");
        Pump();
        Pump();
    }

    // A press/release cycle through UIManager, so focus is assigned by the real
    // mouse-down path rather than by a direct call.
    void ClickThroughManagerAtLogicalX(float logicalX)
    {
        const float y = Input->GetLayoutY() + Input->GetLayoutHeight() * 0.5f;
        Ui->OnMouseMove(logicalX, y);
        Ui->OnMouseButton(0, true);
        Pump();
        Ui->OnMouseButton(0, false);
        Pump();
        Pump();
    }

    // UIManager derives its held-modifier state from key events, and TextInput's
    // pointer path reads it back through GetModifierKeys().
    void HoldModifier(int modifierKey, int mods)
    {
        Ui->OnKey(modifierKey, kKeyActionPress, mods);
    }

    void ReleaseModifiers()
    {
        Ui->OnKey(Input::kKeyCode_LeftControl, kKeyActionRelease, 0);
    }

    Rendering::Text::FontAtlas* Font() const { return Ui->GetDefaultFontAtlas(); }

    // Physical-pixel font size the render path uses. Mirrors the single
    // definition in EmitTextPrimitives/BuildTextInputOverlays.
    float PhysicalPixelSize() const
    {
        const float fontSize = Input->GetResolvedStyle().Visual.FontSize;
        return std::max(1.0f, fontSize * ContentScale);
    }

    // The editor's content box in physical px, derived from layout (logical)
    // and the resolved padding/border, exactly as ComputeContentBox does.
    FieldGeometry ContentBox() const
    {
        const ResolvedStyle& rs = Input->GetResolvedStyle();
        const float cs = ContentScale;
        const float padL = rs.Layout.Padding.Left * cs;
        const float padR = rs.Layout.Padding.Right * cs;
        const float padT = rs.Layout.Padding.Top * cs;
        const float padB = rs.Layout.Padding.Bottom * cs;
        const float blL = rs.Layout.BorderWidth.Left * cs;
        const float blR = rs.Layout.BorderWidth.Right * cs;
        const float blT = rs.Layout.BorderWidth.Top * cs;
        const float blB = rs.Layout.BorderWidth.Bottom * cs;

        FieldGeometry g;
        g.ContentLeft = Input->GetLayoutX() * cs + padL + blL;
        g.ContentTop = Input->GetLayoutY() * cs + padT + blT;
        g.ContentWidth = std::max(0.0f, Input->GetLayoutWidth() * cs - padL - padR - blL - blR);
        g.ContentHeight = std::max(0.0f, Input->GetLayoutHeight() * cs - padT - padB - blT - blB);
        return g;
    }

    std::vector<UI::UIPrimitive> InputPrims() const
    {
        std::vector<UI::UIPrimitive> prims;
        for (uint16_t i = 0;; ++i)
        {
            const UI::UIPrimitive* p = Ui->PeekPrimitiveForTesting(*Input, i);
            if (!p)
                break;
            prims.push_back(*p);
        }
        return prims;
    }

    // The caret bar, identified by the dedicated flag bit the shader uses to
    // drive blinking. Only emitted for a focused editor.
    std::optional<UI::UIPrimitive> CaretPrim() const
    {
        for (const auto& p : InputPrims())
        {
            if ((p.ModeAndFlags & UI::kPrimCaretBit) != 0)
                return p;
        }
        return std::nullopt;
    }

    // Slug curve glyphs — the mode every non-emoji glyph uses.
    std::vector<UI::UIPrimitive> GlyphPrims() const
    {
        std::vector<UI::UIPrimitive> out;
        for (const auto& p : InputPrims())
        {
            if (UI::GetMode(p.ModeAndFlags) == UI::PrimitiveMode::Slug)
                out.push_back(p);
        }
        return out;
    }

    // The selection highlight: a plain rect that is neither the caret nor the
    // editor's own background. Present only while a range is selected.
    std::optional<UI::UIPrimitive> SelectionPrim() const
    {
        const auto prims = InputPrims();
        std::optional<UI::UIPrimitive> found;
        for (const auto& p : prims)
        {
            if (UI::GetMode(p.ModeAndFlags) != UI::PrimitiveMode::Rect)
                continue;
            if ((p.ModeAndFlags & UI::kPrimCaretBit) != 0)
                continue;
            found = p; // the selection is emitted after any background
        }
        return found;
    }

    // Drive the caret to a byte index with keyboard movement, which never
    // touches the pointer path being measured. Right moves by character, so
    // step until the target byte is reached rather than pressing it that many
    // times — the two differ as soon as the text is not all ASCII.
    void PlaceCaretAt(int byteIndex)
    {
        Input->OnKey(Input::kKeyCode_Home, 0, nullptr);
        while (Input->GetCaretIndex() < byteIndex)
        {
            const int before = Input->GetCaretIndex();
            Input->OnKey(Input::kKeyCode_Right, 0, nullptr);
            if (Input->GetCaretIndex() == before)
                break;
        }
        Pump();
    }

    // Feed a logical-space mouse X straight into the editor the way
    // TextFieldBase::OnPointerDown does. The reset click keeps consecutive
    // calls from tripping TextInput's double-click (select-all) detector,
    // which fires for clicks within 400ms and 20px of each other.
    void ClickAtLogicalX(float logicalX)
    {
        const float resetX = Input->GetLayoutX() + Input->GetLayoutWidth() + 500.0f;
        DispatchPointerDown(resetX);
        DispatchPointerDown(logicalX);
    }

    void DispatchPointerDown(float logicalX)
    {
        Input->OnPointerDown(logicalX, Input->GetLayoutY() + Input->GetLayoutHeight() * 0.5f,
                             Input->GetLayoutX(), Input->GetLayoutY(),
                             Input->GetLayoutWidth(), Input->GetLayoutHeight(),
                             Input->GetResolvedStyle(), Font());
    }

    // The logical-space X of a byte index, read back from the rendered caret so
    // clicks are aimed at what the user actually sees. Only meaningful while
    // focused — the caret primitive is the probe and it is focus-gated.
    std::optional<float> LogicalXForCaretIndex(int byteIndex)
    {
        PlaceCaretAt(byteIndex);
        const std::optional<UI::UIPrimitive> caret = CaretPrim();
        if (!caret.has_value())
            return std::nullopt;
        return caret->X / ContentScale;
    }

    // Logical-space X at `fraction` across the character cell spanning the two
    // given byte offsets, with the cell measured independently of the map the
    // thing under test consults.
    //
    // FontAtlas::MeasureText fills its two outputs from two computations:
    // metrics from MeasureUtf8, and the caret map from BuildCaretMapUtf8. The
    // caret bar and hit-testing both read the caret map, so a sample X
    // interpolated between two of its entries is a sample the scan cannot
    // disagree with — it would land on the expected boundary for any monotone
    // map, including one that put the caret nowhere near the glyphs. Taking the
    // cell edges from prefix *metrics* instead is the move the browser oracle
    // made with its mirror span, and it makes that failure visible.
    //
    // The only engine-derived term left is the text origin, which is boundary 0
    // and never one of the boundaries under test.
    //
    // Never sample at 0.5: hit-testing resolves a click to the nearest caret
    // boundary, so the midpoint is that rule's tipping point and a test aimed
    // there cannot tell it apart from "the boundary at or before the pointer".
    // kLeftHalf/kRightHalf are the two sides the oracle measured.
    std::optional<float> LogicalXAcrossCharacter(int byteIndex, int nextByteIndex, float fraction)
    {
        const std::optional<float> origin = LogicalXForCaretIndex(0);
        if (!origin.has_value() || !Font())
            return std::nullopt;

        const std::string& value = Input->GetValue();
        if (byteIndex < 0 || byteIndex >= nextByteIndex || nextByteIndex > static_cast<int>(value.size()))
            return std::nullopt;

        const float px = PhysicalPixelSize();
        const float left = Font()->MeasureText(value.substr(0, static_cast<size_t>(byteIndex)), px).metrics.width;
        const float right = Font()->MeasureText(value.substr(0, static_cast<size_t>(nextByteIndex)), px).metrics.width;
        return *origin + (left + (right - left) * fraction) / ContentScale;
    }

    // Two pointer-downs at the same X inside TextInput's double-click window.
    void DoubleClickAtLogicalX(float logicalX)
    {
        DispatchPointerDown(logicalX);
        DispatchPointerDown(logicalX);
    }

    void DragToLogicalX(float logicalX)
    {
        Input->OnPointerDrag(logicalX, Input->GetLayoutY() + Input->GetLayoutHeight() * 0.5f,
                             Input->GetLayoutX(), Input->GetLayoutY(),
                             Input->GetLayoutWidth(), Input->GetLayoutHeight(),
                             Input->GetResolvedStyle(), Font());
    }

    ~TextFieldFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

// Every geometry test needs a device and a real font; skip cleanly without.
#define REQUIRE_FIXTURE(fx, inited)                                                   \
    do                                                                                \
    {                                                                                 \
        if (!(fx).Dev)                                                                \
            GTEST_SKIP() << "Device init failed";                                     \
        ASSERT_TRUE(inited);                                                          \
        if (!(fx).Font())                                                             \
            GTEST_SKIP() << "Font atlas not available";                               \
    } while (false)

const std::string kSampleText = "Hello World";

// The two sampling positions inside a glyph that tell "nearest caret boundary"
// apart from "the boundary at or before the pointer". Chrome was measured at
// exactly these fractions of every glyph's advance.
constexpr float kLeftHalf = 0.25f;
constexpr float kRightHalf = 0.75f;

} // namespace

// ---------------------------------------------------------------------------
// Caret index <-> pixel position round trip
// ---------------------------------------------------------------------------

// For every caret index: render the caret, take the X the renderer chose, feed
// it back through hit-testing, and require the same index. This is the exact
// loop a user closes when they look at the caret and click on it.
TEST(TextInputGeometryTests, CaretPixelRoundTripsAtEveryIndexAndScale)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 220.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        const int len = static_cast<int>(kSampleText.size());
        for (int index = 0; index <= len; ++index)
        {
            SCOPED_TRACE(::testing::Message() << "index=" << index);

            fx.PlaceCaretAt(index);
            ASSERT_EQ(fx.Input->GetCaretIndex(), index);

            const auto caret = fx.CaretPrim();
            ASSERT_TRUE(caret.has_value()) << "focused editor must emit a caret";

            // The caret is drawn in physical px; pointer events arrive logical.
            const float caretLogicalX = caret->X / scale;
            fx.ClickAtLogicalX(caretLogicalX);

            EXPECT_EQ(fx.Input->GetCaretIndex(), index)
                << "clicking the rendered caret (physical X=" << caret->X
                << ", logical X=" << caretLogicalX << ") must return the same index";
        }
    }
}

// Clicking past either end of the text clamps to the ends of the string
// rather than wrapping or sticking at an interior index.
TEST(TextInputGeometryTests, ClicksOutsideTheTextClampToTheEnds)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 220.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        fx.DispatchPointerDown(fx.Input->GetLayoutX() - 200.0f);
        EXPECT_EQ(fx.Input->GetCaretIndex(), 0);

        fx.ClickAtLogicalX(fx.Input->GetLayoutX() + fx.Input->GetLayoutWidth() + 200.0f);
        EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(kSampleText.size()));
    }
}

// Hit-testing must be monotonic in X and must be able to produce every index:
// a gap would make some character unreachable by clicking, an inversion would
// make the caret jump backwards as the mouse moves right.
TEST(TextInputGeometryTests, HitTestingIsMonotonicAndReachesEveryIndex)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 260.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const float left = fx.Input->GetLayoutX() - 4.0f;
    const float right = fx.Input->GetLayoutX() + fx.Input->GetLayoutWidth() + 4.0f;

    int previous = -1;
    std::vector<bool> reached(kSampleText.size() + 1, false);
    for (float x = left; x <= right; x += 0.25f)
    {
        fx.DispatchPointerDown(right + 1000.0f); // defeat double-click detection
        fx.DispatchPointerDown(x);
        const int index = fx.Input->GetCaretIndex();

        ASSERT_GE(index, 0);
        ASSERT_LE(index, static_cast<int>(kSampleText.size()));
        EXPECT_GE(index, previous) << "caret index must not go backwards as X increases (x=" << x << ")";
        previous = index;
        reached[static_cast<size_t>(index)] = true;
    }

    for (size_t i = 0; i < reached.size(); ++i)
        EXPECT_TRUE(reached[i]) << "no mouse X maps to caret index " << i;
}

// ---------------------------------------------------------------------------
// Alignment
// ---------------------------------------------------------------------------

// Left/center/right must put the run of text at the exact x the alignment
// rule prescribes, and the caret at index 0 marks that x.
TEST(TextInputGeometryTests, AlignmentPlacesTextAtTheExpectedX)
{
    struct Case
    {
        const char* Align;
        const char* Name;
    };
    const Case kCases[] = {{"left", "left"}, {"center", "center"}, {"right", "right"}};

    for (float scale : kContentScales)
    {
        for (const Case& c : kCases)
        {
            SCOPED_TRACE(::testing::Message() << "align=" << c.Name << " contentScale=" << scale);

            TextFieldFixture fx;
            const bool inited = fx.Init(scale, 14.0f, 220.0f, 26.0f, kSampleText, c.Align);
            REQUIRE_FIXTURE(fx, inited);
            fx.Focus();
            fx.PlaceCaretAt(0);

            const auto caret = fx.CaretPrim();
            ASSERT_TRUE(caret.has_value());

            const FieldGeometry g = fx.ContentBox();
            const auto measure = fx.Font()->MeasureText(kSampleText, fx.PhysicalPixelSize());
            const float textWidth = measure.metrics.width;
            ASSERT_LT(textWidth, g.ContentWidth) << "this case must not overflow";

            float expectedX = g.ContentLeft;
            if (std::string(c.Align) == "center")
                expectedX = g.ContentLeft + (g.ContentWidth - textWidth) * 0.5f;
            else if (std::string(c.Align) == "right")
                expectedX = g.ContentLeft + g.ContentWidth - textWidth;

            EXPECT_NEAR(caret->X, expectedX, 1.0f)
                << "caret at index 0 marks where the text starts";
        }
    }
}

// The distance the caret travels from the first index to the last must equal
// the measured width of the string. If it does not, the caret map and the
// width used for alignment disagree, which is what makes a right-aligned
// selection miss the final glyph.
TEST(TextInputGeometryTests, CaretTravelFromStartToEndEqualsTextWidth)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 260.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        fx.PlaceCaretAt(0);
        const auto caretStart = fx.CaretPrim();
        ASSERT_TRUE(caretStart.has_value());

        fx.PlaceCaretAt(static_cast<int>(kSampleText.size()));
        const auto caretEnd = fx.CaretPrim();
        ASSERT_TRUE(caretEnd.has_value());

        const auto measure = fx.Font()->MeasureText(kSampleText, fx.PhysicalPixelSize());
        EXPECT_NEAR(caretEnd->X - caretStart->X, measure.metrics.width, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Vertical metrics
// ---------------------------------------------------------------------------

// A single line in a fixed-height field is centered: the space above the line
// box equals the space below it. The caret spans exactly the line box, so it
// is the probe for where that box landed.
TEST(TextInputGeometryTests, TextIsVerticallyCenteredAcrossFontSizesAndScales)
{
    const float kFontSizes[] = {10.0f, 12.0f, 14.0f, 18.0f, 24.0f};
    // One physical pixel of asymmetry is the most a correct implementation can
    // produce (the line box height need not have the same parity as the box).
    constexpr float kToleranceP = 1.0f;

    for (float scale : kContentScales)
    {
        for (float fontSize : kFontSizes)
        {
            SCOPED_TRACE(::testing::Message() << "fontSize=" << fontSize << " contentScale=" << scale);

            TextFieldFixture fx;
            const bool inited = fx.Init(scale, fontSize, 260.0f, 40.0f, kSampleText);
            REQUIRE_FIXTURE(fx, inited);
            fx.Focus();
            fx.PlaceCaretAt(0);

            const auto caret = fx.CaretPrim();
            ASSERT_TRUE(caret.has_value());

            const FieldGeometry g = fx.ContentBox();
            const float spaceAbove = caret->Y - g.ContentTop;
            const float spaceBelow = (g.ContentTop + g.ContentHeight) - (caret->Y + caret->H);

            EXPECT_NEAR(spaceAbove, spaceBelow, kToleranceP)
                << "line box is not centered: above=" << spaceAbove << " below=" << spaceBelow;
            EXPECT_GE(spaceAbove, -kToleranceP) << "line box overflows the top of the field";
            EXPECT_GE(spaceBelow, -kToleranceP) << "line box overflows the bottom of the field";
        }
    }
}

// The caret band must coincide with the font's line box at the physical pixel
// size actually rendered. A scale applied twice would inflate it.
TEST(TextInputGeometryTests, CaretHeightMatchesTheRenderedLineHeight)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 260.0f, 40.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();
        fx.PlaceCaretAt(0);

        const auto caret = fx.CaretPrim();
        ASSERT_TRUE(caret.has_value());

        const auto lineMetrics = fx.Font()->GetFontLineMetrics(fx.PhysicalPixelSize());
        EXPECT_NEAR(caret->H, std::max(1.0f, lineMetrics.height), 0.75f);
    }
}

// Glyphs and the caret are placed by different functions; their vertical
// extents must still describe the same line.
TEST(TextInputGeometryTests, GlyphsShareTheCaretsVerticalBand)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        // Lower case only, so no glyph reaches above the ascender or below
        // the descender of the line box.
        const std::string text = "someone";
        const bool inited = fx.Init(scale, 14.0f, 260.0f, 40.0f, text);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();
        fx.PlaceCaretAt(0);

        const auto caret = fx.CaretPrim();
        ASSERT_TRUE(caret.has_value());
        const auto glyphs = fx.GlyphPrims();
        ASSERT_FALSE(glyphs.empty());

        const float bandTop = caret->Y;
        const float bandBottom = caret->Y + caret->H;
        // Slug dilates each quad by half a pixel per side for anti-aliasing.
        constexpr float kDilationP = 1.5f;

        for (const auto& gp : glyphs)
        {
            EXPECT_GE(gp.Y, bandTop - kDilationP) << "glyph sits above the caret band";
            EXPECT_LE(gp.Y + gp.H, bandBottom + kDilationP) << "glyph sits below the caret band";
        }
    }
}

// The same agreement, with the line box WIDER than the font. This is the case
// the test above cannot see: with `line-height` unset the box IS the font's own
// height, the half-leading is zero, and the term that seats the font box inside
// the line box contributes nothing whatever it computes. Declaring a line-height
// is what gives it a value to get wrong.
//
// The two placements are computed by different functions from the same inputs --
// BuildTextInputOverlays for the band, EmitTextPrimitives for the run -- so they
// agree only if both seat the font box the same way.
//
// They are compared on the device pixel ROW their baselines land on, not on the
// raw floats. The run's origin is snapped to a whole device pixel
// (SnapRunOriginY takes std::round of the baseline) and the band is not, so the
// two are expected to differ by up to the snap; what may NOT differ is which row
// they round to. A band computed by re-centring on the font box lands half a
// pixel out and rounds to the row below.
TEST(TextInputGeometryTests, GlyphsShareTheCaretsVerticalBandWithDeclaredLineHeight)
{
    // Wider than Roboto's own box at 14px (19 physical px at scale 1), so the
    // half-leading is a real, non-zero number at every scale below.
    constexpr const char* kLineHeight = "30px";

    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const std::string text = "someone";
        const bool inited = fx.Init(scale, 14.0f, 260.0f, 40.0f, text, "left", 4.0f, kLineHeight);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();
        fx.PlaceCaretAt(0);

        const auto caret = fx.CaretPrim();
        ASSERT_TRUE(caret.has_value());
        const auto glyphs = fx.GlyphPrims();
        ASSERT_FALSE(glyphs.empty());

        const auto lm = fx.Font()->GetFontLineMetrics(fx.PhysicalPixelSize());

        // Where the GLYPHS say the font box starts: their drawn baseline, minus
        // the ascent that separates it from the box top. MakeSlugGlyph stores
        // the undilated em-space bounds in UvRect and H spans them, so inverting
        // the quad-top formula recovers the baseline (see BaselineSnapTests).
        float runBaseline = 0.0f;
        bool haveBaseline = false;
        for (const auto& gp : glyphs)
        {
            const float emSpan = gp.UvRect[1] - gp.UvRect[3];
            if (emSpan <= 1e-4f || gp.H <= 1e-4f)
                continue;
            const float b = gp.Y + gp.UvRect[1] * (gp.H / emSpan);
            if (!haveBaseline || b < runBaseline)
            {
                runBaseline = b;
                haveBaseline = true;
            }
        }
        ASSERT_TRUE(haveBaseline) << "no glyph carried a usable em span";

        EXPECT_FLOAT_EQ(std::round(caret->Y + lm.ascender), runBaseline)
            << "the caret band and the glyph run put the baseline on different device pixel "
               "rows: band implies " << (caret->Y + lm.ascender) << ", run drew at " << runBaseline;

        // The containment the unstyled case checks, held with a line box the
        // glyphs do not fill.
        constexpr float kDilationP = 1.5f;
        for (const auto& gp : glyphs)
        {
            EXPECT_GE(gp.Y, caret->Y - kDilationP) << "glyph sits above the caret band";
            EXPECT_LE(gp.Y + gp.H, caret->Y + caret->H + kDilationP)
                << "glyph sits below the caret band";
        }
    }
}

// ---------------------------------------------------------------------------
// Selection geometry
// ---------------------------------------------------------------------------

// Select-all must highlight the whole run: from where the text starts to
// where it ends, with no glyph left uncovered at either edge.
TEST(TextInputGeometryTests, SelectAllHighlightsTheEntireRun)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 260.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        fx.PlaceCaretAt(0);
        const auto caretAtStart = fx.CaretPrim();
        ASSERT_TRUE(caretAtStart.has_value());
        const float textStartX = caretAtStart->X;

        fx.Input->SelectAll();
        fx.Pump();

        const auto selection = fx.SelectionPrim();
        ASSERT_TRUE(selection.has_value()) << "select-all must emit a highlight";

        const auto measure = fx.Font()->MeasureText(kSampleText, fx.PhysicalPixelSize());
        EXPECT_NEAR(selection->X, textStartX, 1.0f) << "highlight must start where the text starts";
        EXPECT_NEAR(selection->W, measure.metrics.width, 1.0f)
            << "highlight must span the full measured width — a short highlight is the "
               "'last character not selected' symptom";
    }
}

// A drag that runs off both ends selects everything in between, including the
// first and last byte.
TEST(TextInputGeometryTests, DragPastBothEndsSelectsTheWholeValue)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 260.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const float farLeft = fx.Input->GetLayoutX() - 300.0f;
    const float farRight = fx.Input->GetLayoutX() + fx.Input->GetLayoutWidth() + 300.0f;

    fx.DispatchPointerDown(farLeft);
    EXPECT_EQ(fx.Input->GetCaretIndex(), 0);
    fx.DragToLogicalX(farRight);

    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 0);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
              static_cast<int>(kSampleText.size()));
    EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(kSampleText.size()));

    // ...and the same in reverse.
    fx.DispatchPointerDown(farRight);
    EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(kSampleText.size()));
    fx.DragToLogicalX(farLeft);

    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 0);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
              static_cast<int>(kSampleText.size()));
    EXPECT_EQ(fx.Input->GetCaretIndex(), 0);
}

// ---------------------------------------------------------------------------
// Horizontal scroll (value wider than the field)
// ---------------------------------------------------------------------------

// A field narrower than its text scrolls internally. Wherever the caret goes,
// it must stay inside the visible content box — otherwise the user is typing
// somewhere they cannot see.
TEST(TextInputGeometryTests, CaretStaysVisibleWhileMovingThroughOverflowingText)
{
    const std::string longText = "The quick brown fox jumps over the lazy dog";

    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 90.0f, 26.0f, longText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        const auto measure = fx.Font()->MeasureText(longText, fx.PhysicalPixelSize());
        const FieldGeometry g = fx.ContentBox();
        ASSERT_GT(measure.metrics.width, g.ContentWidth) << "this case must overflow";

        const int len = static_cast<int>(longText.size());
        for (int index = 0; index <= len; ++index)
        {
            SCOPED_TRACE(::testing::Message() << "index=" << index);
            fx.PlaceCaretAt(index);

            const auto caret = fx.CaretPrim();
            ASSERT_TRUE(caret.has_value());

            EXPECT_GE(caret->X, g.ContentLeft - 1.0f) << "caret scrolled off the left edge";
            EXPECT_LE(caret->X + caret->W, g.ContentLeft + g.ContentWidth + 1.0f)
                << "caret scrolled off the right edge";
        }
    }
}

// With the view scrolled, hit-testing must still land on the character under
// the mouse: the scroll offset has to be applied to the pointer path with the
// same sign and units as the render path.
TEST(TextInputGeometryTests, HitTestingHonoursTheScrollOffset)
{
    const std::string longText = "The quick brown fox jumps over the lazy dog";

    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 90.0f, 26.0f, longText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();

        // Scroll to the far end, then probe indices that are inside the
        // visible window at that scroll position.
        const int len = static_cast<int>(longText.size());
        fx.PlaceCaretAt(len);
        ASSERT_GT(fx.Input->GetTextScrollX(), 0.0f) << "field must have scrolled";

        for (int index = len; index >= len - 6; --index)
        {
            SCOPED_TRACE(::testing::Message() << "index=" << index);
            fx.PlaceCaretAt(index);
            const auto caret = fx.CaretPrim();
            ASSERT_TRUE(caret.has_value());

            const float scrollBefore = fx.Input->GetTextScrollX();
            fx.ClickAtLogicalX(caret->X / scale);
            EXPECT_EQ(fx.Input->GetCaretIndex(), index)
                << "click at the rendered caret must return the same index "
                   "(scrollX=" << scrollBefore << ")";
        }
    }
}

// Text that fits must never be left scrolled — a stale scroll offset shifts
// the whole run sideways.
TEST(TextInputGeometryTests, ScrollResetsWhenTheValueShrinksToFit)
{
    TextFieldFixture fx;
    const std::string longText = "The quick brown fox jumps over the lazy dog";
    const bool inited = fx.Init(1.0f, 14.0f, 90.0f, 26.0f, longText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    fx.PlaceCaretAt(static_cast<int>(longText.size()));
    ASSERT_GT(fx.Input->GetTextScrollX(), 0.0f);

    fx.Field->SetValue("ok");
    fx.Pump();
    fx.Pump();

    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f)
        << "a fitting value must render from the start of the text";

    fx.PlaceCaretAt(0);
    const auto caret = fx.CaretPrim();
    ASSERT_TRUE(caret.has_value());
    EXPECT_NEAR(caret->X, fx.ContentBox().ContentLeft, 1.0f);
}

// The internal scroll has exactly one writer, and it runs before ANY of the
// element's primitives are emitted (RefreshTextInputScroll) so the glyph run,
// the selection highlight and the caret all place against the same offset.
// The two tests below drive the offset in both directions and check what a
// settle that ran late — after the glyphs, or a frame behind — would break:
// the run and the caret would be drawn against different offsets, and the
// pointer map would agree with neither.

TEST(TextInputGeometryTests, ScrollFollowsTheCaretWhileTyping)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 90.0f, 26.0f, "");
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    // Instrument check: an empty field has nothing to scroll, so a non-zero
    // offset below is attributable to the typing and to nothing else.
    ASSERT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f);

    const std::string typed = "The quick brown fox";
    for (char c : typed)
    {
        fx.Input->OnChar(static_cast<unsigned int>(c));
        fx.Pump();
    }
    ASSERT_EQ(fx.Input->GetValue(), typed) << "every keystroke must have landed";
    ASSERT_GT(fx.Input->GetTextScrollX(), 0.0f)
        << "text typed past the right edge must bring the caret back into view";

    const auto caret = fx.CaretPrim();
    ASSERT_TRUE(caret.has_value());
    const FieldGeometry g = fx.ContentBox();
    EXPECT_GE(caret->X, g.ContentLeft - 1.0f);
    EXPECT_LE(caret->X + caret->W, g.ContentLeft + g.ContentWidth + 1.0f);

    // The round trip is the real assertion: click where the caret was drawn and
    // the pointer path must name the index it was drawn for. That only holds
    // when the glyphs, the caret and the hit map read one scroll offset.
    fx.ClickAtLogicalX(caret->X / fx.ContentScale);
    EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(typed.size()))
        << "click at the rendered caret must return the caret's own index";
}

TEST(TextInputGeometryTests, HomeReleasesTheScrollInTheSameFrame)
{
    TextFieldFixture fx;
    const std::string longText = "The quick brown fox jumps over the lazy dog";
    const bool inited = fx.Init(1.0f, 14.0f, 90.0f, 26.0f, longText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    fx.PlaceCaretAt(static_cast<int>(longText.size()));
    const float scrolledOffset = fx.Input->GetTextScrollX();
    ASSERT_GT(scrolledOffset, 0.0f) << "field must have scrolled";

    const auto glyphsScrolled = fx.GlyphPrims();
    ASSERT_FALSE(glyphsScrolled.empty()) << "no glyphs emitted";
    const float firstGlyphScrolledX = glyphsScrolled.front().X;

    // One key, one frame — the settle has no second chance to catch up.
    fx.Input->OnKey(Input::kKeyCode_Home, 0, nullptr);
    fx.Pump();

    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f)
        << "the caret is back at byte 0, so there is nothing to scroll past";

    const FieldGeometry g = fx.ContentBox();
    const auto caret = fx.CaretPrim();
    ASSERT_TRUE(caret.has_value());
    EXPECT_NEAR(caret->X, g.ContentLeft, 1.0f);

    // And the glyph run slid right by exactly the offset that was released —
    // the emitted geometry consumed the settled value, in physical px.
    const auto glyphsReset = fx.GlyphPrims();
    ASSERT_FALSE(glyphsReset.empty());
    EXPECT_NEAR(glyphsReset.front().X - firstGlyphScrolledX,
                scrolledOffset * fx.ContentScale, 0.5f)
        << "the run must move with the scroll it was placed against";
}

// ---------------------------------------------------------------------------
// Measurement consistency between the shaping and measuring entry points
// ---------------------------------------------------------------------------

// Glyph placement measures with ShapeText while the caret map and alignment
// measure with MeasureText. The two must agree on the width, or the caret
// drifts away from the glyphs the further along the string it goes.
TEST(TextInputGeometryTests, ShapedWidthMatchesMeasuredWidth)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 260.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);

    const char* kStrings[] = {"Hello World", "0.0000000000000000", "iiiiiiii",
                              "WWWWWWWW", "a\xC3\xA9\xE4\xB8\xAD", " leading and trailing "};
    const float kSizes[] = {10.0f, 14.0f, 18.0f, 21.0f, 28.0f};

    for (const char* s : kStrings)
    {
        for (float px : kSizes)
        {
            SCOPED_TRACE(::testing::Message() << "text='" << s << "' px=" << px);

            const auto measured = fx.Font()->MeasureText(s, px);
            Rendering::Text::FontAtlas::ShapeResult shaped;
            fx.Font()->ShapeText(s, px, shaped, 0xFF000000u);

            EXPECT_NEAR(shaped.metrics.width, measured.metrics.width, 0.01f)
                << "ShapeText and MeasureText disagree on the run width";

            ASSERT_EQ(measured.caretXByByte.size(), std::string(s).size() + 1);
            EXPECT_NEAR(measured.caretXByByte.back(), measured.metrics.width, 0.01f)
                << "the last caret position must be the end of the run";
            EXPECT_FLOAT_EQ(measured.caretXByByte.front(), 0.0f);

            for (size_t i = 1; i < measured.caretXByByte.size(); ++i)
            {
                EXPECT_GE(measured.caretXByByte[i], measured.caretXByByte[i - 1] - 0.001f)
                    << "caret map must be non-decreasing at byte " << i;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Single-line contract
// ---------------------------------------------------------------------------

// Spaces are break opportunities; a value without them cannot expose a
// wrapping bug, so the overflow text below deliberately has several.
static const std::string kOverflowingText = "The quick brown fox jumps over the lazy dog";

// TextInput is a single-line editor: Yoga measures it as one line
// (AllowWrapForMeasure is false) and its caret map, selection overlay and
// hit-testing all index one shaped run. If the render path wrapped it at the
// content width instead, the glyphs would stack onto rows the caret model
// cannot address — the caret and selection would be drawn against a line that
// is not the one on screen, the value would top-align instead of centering,
// and the horizontal scroll would never engage.
TEST(TextInputGeometryTests, OverflowingTextStaysOnOneLine)
{
    for (float scale : kContentScales)
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 90.0f, 26.0f, kOverflowingText);
        REQUIRE_FIXTURE(fx, inited);
        fx.Focus();
        fx.PlaceCaretAt(0);

        const FieldGeometry g = fx.ContentBox();
        const float px = fx.PhysicalPixelSize();
        const auto measure = fx.Font()->MeasureText(kOverflowingText, px);
        ASSERT_GT(measure.metrics.width, g.ContentWidth) << "this case must overflow";

        const auto glyphs = fx.GlyphPrims();
        ASSERT_FALSE(glyphs.empty());

        // Glyphs on one baseline still have different ink tops (an ascender
        // starts higher than an x-height letter), so the row test is the total
        // vertical span: one line cannot exceed its own line box by much,
        // while a second row would push the span past a full line height.
        float minTop = glyphs.front().Y;
        float maxBottom = glyphs.front().Y + glyphs.front().H;
        for (const auto& gp : glyphs)
        {
            minTop = std::min(minTop, gp.Y);
            maxBottom = std::max(maxBottom, gp.Y + gp.H);
        }
        const float lineHeight = std::max(1.0f, fx.Font()->GetFontLineMetrics(px).height);
        EXPECT_LT(maxBottom - minTop, 1.5f * lineHeight)
            << "glyphs span " << (maxBottom - minTop) << "px against a " << lineHeight
            << "px line — the value wrapped onto extra rows";

        // ...and that one line is the caret's line.
        const auto caret = fx.CaretPrim();
        ASSERT_TRUE(caret.has_value());
        constexpr float kDilationP = 1.5f;
        EXPECT_GE(minTop, caret->Y - kDilationP);
        EXPECT_LE(maxBottom, caret->Y + caret->H + kDilationP);
    }
}

// Overflow is handled by scrolling the single line, which is only coherent if
// the value never wrapped in the first place.
TEST(TextInputGeometryTests, OverflowingTextScrollsInsteadOfWrapping)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 90.0f, 26.0f, kOverflowingText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    fx.PlaceCaretAt(0);
    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f)
        << "caret at the start shows the start of the value";

    fx.PlaceCaretAt(static_cast<int>(kOverflowingText.size()));
    EXPECT_GT(fx.Input->GetTextScrollX(), 0.0f)
        << "caret at the end of overflowing text must scroll the view";

    fx.PlaceCaretAt(0);
    EXPECT_FLOAT_EQ(fx.Input->GetTextScrollX(), 0.0f)
        << "caret back at the start must scroll the view back";
}

// ---------------------------------------------------------------------------
// Click, focus and select-all interaction
//
// The contract these pin is Chrome's, verified against a real <input>:
//
//   mouse click into an unfocused free-text field -> caret at the click, no
//                                                    selection
//   Tab into any field                            -> whole value selected
//   mouse click into a value/property field       -> whole value selected
//                                                    (deliberate IDE
//                                                     divergence: click and
//                                                     retype)
//   Shift+click                                   -> extends the selection
//   Ctrl/Cmd+click                                -> plain click
//
// Clicking an unfocused field runs two things that both want to own the caret:
// OnPointerDown places it where the user clicked, and the focus-gain handler
// one frame later may select the whole value. The free-text policy suppresses
// that select-all rather than racing it.
// ---------------------------------------------------------------------------

TEST(TextInputGeometryTests, ClickingAnUnfocusedFreeTextFieldPlacesTheCaretWhereClicked)
{
    // Scale 1.0 hides DPI bugs: a click X that gets scaled twice, or not at
    // all, still lands on the right index there. The second scale is what makes
    // the logical -> physical conversion testable.
    for (float scale : {1.0f, 1.5f})
    {
        SCOPED_TRACE(::testing::Message() << "contentScale=" << scale);

        TextFieldFixture fx;
        const bool inited = fx.Init(scale, 14.0f, 220.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);

        const int len = static_cast<int>(kSampleText.size());
        // Ordered so consecutive clicks stay outside TextInput's double-click
        // window (400ms, 20px), which selects all instead of placing a caret.
        for (int target : {0, len, len / 2})
        {
            SCOPED_TRACE(::testing::Message() << "target=" << target);

            fx.Focus();
            const std::optional<float> clickX = fx.LogicalXForCaretIndex(target);
            ASSERT_TRUE(clickX.has_value()) << "the rendered caret aims the click";

            // Blur, then park the caret somewhere else, so a click that fails
            // to move it cannot pass by sitting on the answer already.
            fx.Blur();
            fx.PlaceCaretAt(target == 0 ? len : 0);

            fx.ClickThroughManagerAtLogicalX(*clickX);

            EXPECT_EQ(fx.Ui->GetFocusedElementId(), "field") << "the click must focus the field";
            EXPECT_EQ(fx.Input->GetCaretIndex(), target);
            EXPECT_EQ(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd())
                << "mouse focus on free text leaves nothing selected";
        }
    }
}

// The consequence of keeping the click's caret: the next keystroke edits at
// that point instead of replacing the value.
TEST(TextInputGeometryTests, TypingAfterMouseFocusInsertsAtTheClickedCaret)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);

    const int target = 5; // "Hello| World"
    fx.Focus();
    const std::optional<float> clickX = fx.LogicalXForCaretIndex(target);
    ASSERT_TRUE(clickX.has_value());

    fx.Blur();
    fx.PlaceCaretAt(0);
    fx.ClickThroughManagerAtLogicalX(*clickX);

    fx.Input->OnChar(static_cast<unsigned int>('!'));
    fx.Pump();

    EXPECT_EQ(fx.Input->GetValue(), "Hello! World");
    EXPECT_EQ(fx.Input->GetCaretIndex(), target + 1);
}

// Typing straight after Tab focus replaces the whole value, which is what the
// keyboard select-all exists to enable.
TEST(TextInputGeometryTests, TypingAfterKeyboardFocusReplacesTheWholeValue)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);

    fx.FocusViaTab();

    fx.Input->OnChar(static_cast<unsigned int>('Z'));
    fx.Pump();

    EXPECT_EQ(fx.Input->GetValue(), "Z");
    EXPECT_EQ(fx.Input->GetCaretIndex(), 1);
}

// Numeric value/property fields keep select-all on mouse focus. That diverges
// from the browser on purpose: an inspector property is edited by clicking it
// and retyping, the convention every DCC tool uses.
TEST(TextInputGeometryTests, MouseFocusOnAValueFieldStillSelectsTheWholeValue)
{
    TextFieldFixture fx;
    const bool inited = fx.InitValueField(1.0f, 14.0f, 220.0f, 26.0f, 1234567);
    REQUIRE_FIXTURE(fx, inited);

    const int len = static_cast<int>(fx.Input->GetValue().size());
    ASSERT_GT(len, 0);

    // Click just inside the leading edge: without select-all the caret would
    // land at the start, so the two outcomes cannot be confused.
    constexpr float kJustInsideLeadingEdgePx = 6.0f;
    fx.ClickThroughManagerAtLogicalX(fx.Input->GetLayoutX() + kJustInsideLeadingEdgePx);

    EXPECT_EQ(fx.Ui->GetFocusedElementId(), "field") << "the click must focus the field";
    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 0);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), len);
    EXPECT_EQ(fx.Input->GetCaretIndex(), len);
}

// Once focused, a click places the caret and leaves nothing selected — no
// gesture turns a plain click into a range.
TEST(TextInputGeometryTests, ClickingAnAlreadyFocusedFieldPlacesTheCaret)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const int target = 4;
    fx.PlaceCaretAt(target);
    const auto caret = fx.CaretPrim();
    ASSERT_TRUE(caret.has_value());

    fx.ClickAtLogicalX(caret->X / fx.ContentScale);

    EXPECT_EQ(fx.Input->GetCaretIndex(), target);
    EXPECT_EQ(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd())
        << "a plain click leaves an empty selection at the caret";
}

// Shift+click is the extend gesture: the range runs from the caret the click
// started at to the one it landed on.
TEST(TextInputGeometryTests, ShiftClickExtendsTheSelectionFromTheCaret)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    constexpr int kAnchor = 2;
    constexpr int kTarget = 9;
    const std::optional<float> targetX = fx.LogicalXForCaretIndex(kTarget);
    ASSERT_TRUE(targetX.has_value());

    fx.PlaceCaretAt(kAnchor);
    fx.HoldModifier(Input::kKeyCode_LeftShift, Input::kModShift);
    fx.DispatchPointerDown(*targetX);
    fx.ReleaseModifiers();

    EXPECT_EQ(fx.Input->GetCaretIndex(), kTarget);
    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), kAnchor);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), kTarget);
}

// Ctrl/Cmd+click is not an extend gesture on any platform — it repositions the
// caret exactly like a plain click.
TEST(TextInputGeometryTests, CtrlClickOnAFocusedFieldPlacesTheCaretInsteadOfExtending)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    constexpr int kAnchor = 2;
    constexpr int kTarget = 9;
    const std::optional<float> targetX = fx.LogicalXForCaretIndex(kTarget);
    ASSERT_TRUE(targetX.has_value());

    fx.PlaceCaretAt(kAnchor);
    fx.HoldModifier(Input::kKeyCode_LeftControl, Input::kModControl);
    fx.DispatchPointerDown(*targetX);
    fx.ReleaseModifiers();

    EXPECT_EQ(fx.Input->GetCaretIndex(), kTarget);
    EXPECT_EQ(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd())
        << "Ctrl+click must collapse the selection at the click, not extend to it";
}

// Same for the click that also takes focus: the modifier changes nothing about
// where the caret ends up.
TEST(TextInputGeometryTests, CtrlClickOnAnUnfocusedFieldPlacesTheCaret)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);

    constexpr int kAnchor = 2;
    constexpr int kTarget = 9;
    fx.Focus();
    const std::optional<float> targetX = fx.LogicalXForCaretIndex(kTarget);
    ASSERT_TRUE(targetX.has_value());

    fx.PlaceCaretAt(kAnchor);
    fx.Blur();
    fx.HoldModifier(Input::kKeyCode_LeftControl, Input::kModControl);
    fx.ClickThroughManagerAtLogicalX(*targetX);
    fx.ReleaseModifiers();

    EXPECT_EQ(fx.Ui->GetFocusedElementId(), "field") << "the click must focus the field";
    EXPECT_EQ(fx.Input->GetCaretIndex(), kTarget);
    EXPECT_EQ(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd())
        << "Ctrl+click into an unfocused field must not select a range";
}

// ---------------------------------------------------------------------------
// Click -> caret boundary
//
// Measured against Chrome/150 (real page.mouse.click() on a real <input>,
// devicePixelRatio 1, every glyph sampled at 25% and 75% of its advance):
// a click resolves to the *nearest* caret boundary. At 25% of glyph i that is
// boundary i; at 75% it is boundary i+1 — 284 sample points across 18 strings,
// no exception.
//
// The midpoint is the tipping point of that rule, so a test that samples there
// cannot distinguish it from "the last boundary at or before the pointer".
// These sample both halves.
//
// "25% of glyph i" is a claim about the glyph's advance, so the sample X is
// built from independently measured advances rather than from the caret map
// hit-testing scans — see LogicalXAcrossCharacter for why that distinction has
// teeth.
// ---------------------------------------------------------------------------

namespace
{
struct GlyphCell
{
    const char* Value;
    int ByteIndex;     // first byte of the glyph cell
    int NextByteIndex; // first byte of the next cell
    const char* What;
};

// Glyphs whose two halves resolve to different boundaries — which is every
// glyph, and is the whole content of the oracle's Part A.
constexpr GlyphCell kGlyphCells[] = {
    {"Player Character", 5, 6, "last letter of the first word"},
    {"Player Character", 6, 7, "the space between the words"},
    {"Player Character", 10, 11, "well inside the second word"},
    {"hello, world", 5, 6, "the comma"},
    {"1.5 2.5", 3, 4, "the space between the numbers"},
    {"a,,b", 1, 2, "the first of two commas"},
};
} // namespace

TEST(TextInputGeometryTests, ClickResolvesToTheNearestCaretBoundary)
{
    for (float scale : {1.0f, 1.5f})
    {
        for (const GlyphCell& cell : kGlyphCells)
        {
            SCOPED_TRACE(::testing::Message()
                         << "contentScale=" << scale << " value='" << cell.Value
                         << "' glyph=[" << cell.ByteIndex << "," << cell.NextByteIndex << ") ("
                         << cell.What << ")");

            TextFieldFixture fx;
            const bool inited = fx.Init(scale, 14.0f, 260.0f, 26.0f, cell.Value);
            REQUIRE_FIXTURE(fx, inited);
            fx.Focus();

            const std::optional<float> leftX =
                fx.LogicalXAcrossCharacter(cell.ByteIndex, cell.NextByteIndex, kLeftHalf);
            const std::optional<float> rightX =
                fx.LogicalXAcrossCharacter(cell.ByteIndex, cell.NextByteIndex, kRightHalf);
            ASSERT_TRUE(leftX.has_value() && rightX.has_value())
                << "the rendered caret aims the clicks";

            fx.ClickAtLogicalX(*leftX);
            EXPECT_EQ(fx.Input->GetCaretIndex(), cell.ByteIndex)
                << "a pointer in the left half of a glyph is nearest that glyph's own boundary";

            fx.ClickAtLogicalX(*rightX);
            EXPECT_EQ(fx.Input->GetCaretIndex(), cell.NextByteIndex)
                << "a pointer in the right half of a glyph is nearest the *next* boundary";
        }
    }
}

// Chrome's caret stops are grapheme-cluster boundaries, not code-point
// boundaries: in "nai<U+0308>ve" it jumps from UTF-8 byte 2 straight to byte 5,
// refusing byte 3 even though that is a valid code-point boundary. HarfBuzz
// gives the base and its combining mark one cluster, and the caret map
// backfills every byte of a cluster to the cluster's X, so the nearest-boundary
// scan tie-breaks to the lowest index of the run and can never return an
// interior byte.
TEST(TextInputGeometryTests, ClickInsideACombiningMarkClusterLandsOnTheClusterBoundary)
{
    // n a i U+0308 v e SPACE c a f e U+0301
    const std::string value = "nai\xCC\x88ve cafe\xCC\x81";
    constexpr int kClusterBegin = 2; // 'i'
    constexpr int kClusterEnd = 5;   // 'v', past the combining mark

    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 260.0f, 26.0f, value);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const std::optional<float> leftX =
        fx.LogicalXAcrossCharacter(kClusterBegin, kClusterEnd, kLeftHalf);
    const std::optional<float> rightX =
        fx.LogicalXAcrossCharacter(kClusterBegin, kClusterEnd, kRightHalf);
    ASSERT_TRUE(leftX.has_value() && rightX.has_value());

    fx.ClickAtLogicalX(*leftX);
    EXPECT_EQ(fx.Input->GetCaretIndex(), kClusterBegin);

    fx.ClickAtLogicalX(*rightX);
    EXPECT_EQ(fx.Input->GetCaretIndex(), kClusterEnd)
        << "byte 3 is a code-point boundary inside the cluster and must not be reachable";
}

// ---------------------------------------------------------------------------
// Double-click word selection
//
// The boundary -> selection half of the contract is pinned in
// TextSegmentationTests. What these cover is the composition: the click X
// resolves to a boundary by the rule above, and that boundary selects the
// oracle's range. Both values on every row are Chrome's, read straight off the
// oracle's Part A and Part B tables.
// ---------------------------------------------------------------------------

namespace
{
struct DoubleClickCase
{
    const char* Value;
    int ByteIndex;     // first byte of the glyph cell the pointer is inside
    int NextByteIndex; // first byte of the next cell

    // Oracle Part A gives the boundary each half resolves to; Part B gives the
    // selection that boundary produces. The left half resolves to ByteIndex and
    // the right half to NextByteIndex, so only the two selections vary.
    int LeftHalfBegin;
    int LeftHalfEnd;
    int RightHalfBegin;
    int RightHalfEnd;
    const char* What;
};

// Every row but the last is one of the glyphs the oracle lists as diverging:
// its two halves select different ranges, so a rule that answered "the glyph
// under the pointer" for the whole cell gets the right half wrong. The last row
// is the control — both halves stay inside one word and must agree.
constexpr DoubleClickCase kDoubleClickCases[] = {
    {"Player Character", 5, 6, 0, 7, 6, 7, "last letter of the word, then its trailing space"},
    {"Player Character", 6, 7, 6, 7, 7, 16, "the space, then the word after it"},
    {"hello, world", 5, 6, 5, 7, 6, 7, "the comma takes the space; the space alone does not"},
    {"foo-bar baz", 3, 4, 3, 4, 4, 8, "the hyphen, then the word after it"},
    {"1.5 2.5", 3, 4, 3, 4, 4, 7, "the space between the numbers"},
    {"a,,b", 1, 2, 1, 2, 2, 3, "punctuation does not group into runs"},
    {"camelCase word", 8, 9, 0, 10, 9, 10, "last letter of the word, then its trailing space"},
    {"a  b", 0, 1, 0, 3, 1, 3, "the word takes the whole run; the run alone is itself"},
    {"Player Character", 10, 11, 7, 16, 7, 16, "control: both halves stay inside one word"},
};
} // namespace

TEST(TextInputGeometryTests, DoubleClickSelectionMatchesTheOracleInBothHalvesOfAGlyph)
{
    for (float scale : {1.0f, 1.5f})
    {
        for (const DoubleClickCase& testCase : kDoubleClickCases)
        {
            for (int half = 0; half < 2; ++half)
            {
                const bool leftHalf = half == 0;
                const float fraction = leftHalf ? kLeftHalf : kRightHalf;
                const int expectedBegin = leftHalf ? testCase.LeftHalfBegin : testCase.RightHalfBegin;
                const int expectedEnd = leftHalf ? testCase.LeftHalfEnd : testCase.RightHalfEnd;

                SCOPED_TRACE(::testing::Message()
                             << "contentScale=" << scale << " value='" << testCase.Value
                             << "' glyph=[" << testCase.ByteIndex << "," << testCase.NextByteIndex
                             << ") at " << fraction << " (" << testCase.What << ")");

                TextFieldFixture fx;
                const bool inited = fx.Init(scale, 14.0f, 260.0f, 26.0f, testCase.Value);
                REQUIRE_FIXTURE(fx, inited);
                fx.Focus();

                const std::optional<float> clickX = fx.LogicalXAcrossCharacter(
                    testCase.ByteIndex, testCase.NextByteIndex, fraction);
                ASSERT_TRUE(clickX.has_value()) << "the rendered caret aims the click";

                // Park the caret away from the answer so a double-click that
                // fails to select anything cannot pass by accident.
                fx.PlaceCaretAt(0);
                fx.DoubleClickAtLogicalX(*clickX);

                EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
                          expectedBegin);
                EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
                          expectedEnd);
                EXPECT_EQ(fx.Input->GetCaretIndex(), expectedEnd);
            }
        }
    }
}

// A double-click in the middle of a long value must not spill into the rest of
// it — the regression this replaces selected everything.
TEST(TextInputGeometryTests, DoubleClickLeavesTheRestOfTheValueUnselected)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText); // "Hello World"
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const std::optional<float> clickX = fx.LogicalXAcrossCharacter(8, 9, kLeftHalf); // inside "World"
    ASSERT_TRUE(clickX.has_value());

    fx.PlaceCaretAt(0);
    fx.DoubleClickAtLogicalX(*clickX);

    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 6);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
              static_cast<int>(kSampleText.size()));
}

// Platforms deliver a MouseMove at the press position after a click. The word
// a double-click selected has to survive it — the drag path would otherwise
// collapse the selection back to the caret under the pointer.
TEST(TextInputGeometryTests, DoubleClickSelectionSurvivesAStationaryDrag)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText); // "Hello World"
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const std::optional<float> clickX = fx.LogicalXAcrossCharacter(8, 9, kLeftHalf); // inside "World"
    ASSERT_TRUE(clickX.has_value());

    fx.PlaceCaretAt(0);
    fx.DoubleClickAtLogicalX(*clickX);
    fx.DragToLogicalX(*clickX);

    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 6);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
              static_cast<int>(kSampleText.size()));
}

// A drag that really moves still takes over from the press's selection.
TEST(TextInputGeometryTests, DraggingAwayAfterADoubleClickResumesRangeSelection)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);
    fx.Focus();

    const std::optional<float> clickX = fx.LogicalXAcrossCharacter(8, 9, kLeftHalf);
    const std::optional<float> startX = fx.LogicalXForCaretIndex(0);
    ASSERT_TRUE(clickX.has_value());
    ASSERT_TRUE(startX.has_value());

    fx.PlaceCaretAt(0);
    fx.DoubleClickAtLogicalX(*clickX);
    fx.DragToLogicalX(*startX);

    EXPECT_EQ(fx.Input->GetCaretIndex(), 0);
    EXPECT_EQ(fx.Input->GetSelectionEnd(), 0) << "a real drag must move the selection edge";
}

// ---------------------------------------------------------------------------
// Select-all-on-mouse-focus as a per-instance mode
//
// The per-class defaults are what the two policy tests above already cover.
// These pin that the default is a starting value rather than a fixed property
// of the type: an inspector's free-text field can ask for the value-field
// behaviour, and a numeric field can decline it.
// ---------------------------------------------------------------------------

TEST(TextInputGeometryTests, FreeTextFieldOptedInSelectsTheWholeValueOnMouseFocus)
{
    TextFieldFixture fx;
    const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
    REQUIRE_FIXTURE(fx, inited);

    fx.Field->SetSelectsAllOnMouseFocus(true);

    // Click just inside the leading edge: without select-all the caret would
    // land at the start, so the two outcomes cannot be confused.
    constexpr float kJustInsideLeadingEdgePx = 6.0f;
    fx.ClickThroughManagerAtLogicalX(fx.Input->GetLayoutX() + kJustInsideLeadingEdgePx);

    EXPECT_EQ(fx.Ui->GetFocusedElementId(), "field") << "the click must focus the field";
    EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 0);
    EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
              static_cast<int>(kSampleText.size()));
    EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(kSampleText.size()));
}

TEST(TextInputGeometryTests, ValueFieldOptedOutKeepsTheClickedCaret)
{
    TextFieldFixture fx;
    const bool inited = fx.InitValueField(1.0f, 14.0f, 220.0f, 26.0f, 1234567);
    REQUIRE_FIXTURE(fx, inited);

    fx.ValueField->SetSelectsAllOnMouseFocus(false);

    constexpr int kTarget = 3; // "123|4567"
    fx.Focus();
    const std::optional<float> clickX = fx.LogicalXForCaretIndex(kTarget);
    ASSERT_TRUE(clickX.has_value());

    fx.Blur();
    fx.PlaceCaretAt(0);
    fx.ClickThroughManagerAtLogicalX(*clickX);

    EXPECT_EQ(fx.Ui->GetFocusedElementId(), "field") << "the click must focus the field";
    EXPECT_EQ(fx.Input->GetCaretIndex(), kTarget);
    EXPECT_EQ(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd())
        << "opting out of select-all must leave the click's caret alone";
}

// Tab supplies no click position worth preserving, so it selects the whole
// value whatever the mouse-focus mode says.
TEST(TextInputGeometryTests, TabFocusSelectsTheWholeValueInEveryMode)
{
    for (bool selectsAllOnMouseFocus : {false, true})
    {
        SCOPED_TRACE(::testing::Message()
                     << "selectsAllOnMouseFocus=" << selectsAllOnMouseFocus);

        TextFieldFixture fx;
        const bool inited = fx.Init(1.0f, 14.0f, 220.0f, 26.0f, kSampleText);
        REQUIRE_FIXTURE(fx, inited);

        fx.Field->SetSelectsAllOnMouseFocus(selectsAllOnMouseFocus);
        fx.FocusViaTab();

        ASSERT_EQ(fx.Ui->GetFocusedElementId(), "field") << "Tab must reach the field";
        EXPECT_TRUE(fx.Ui->IsFocusViaKeyboard());
        EXPECT_EQ(std::min(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()), 0);
        EXPECT_EQ(std::max(fx.Input->GetSelectionStart(), fx.Input->GetSelectionEnd()),
                  static_cast<int>(kSampleText.size()));
        EXPECT_EQ(fx.Input->GetCaretIndex(), static_cast<int>(kSampleText.size()));
    }

    TextFieldFixture value;
    const bool valueInited = value.InitValueField(1.0f, 14.0f, 220.0f, 26.0f, 1234567);
    REQUIRE_FIXTURE(value, valueInited);

    value.ValueField->SetSelectsAllOnMouseFocus(false);
    value.FocusViaTab();

    const int len = static_cast<int>(value.Input->GetValue().size());
    ASSERT_EQ(value.Ui->GetFocusedElementId(), "field") << "Tab must reach the field";
    EXPECT_EQ(std::min(value.Input->GetSelectionStart(), value.Input->GetSelectionEnd()), 0);
    EXPECT_EQ(std::max(value.Input->GetSelectionStart(), value.Input->GetSelectionEnd()), len);
}
