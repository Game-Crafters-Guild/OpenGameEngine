// Whether a UI-tree dump is allowed to claim it can prove an id absent.
//
// The dump is a plain nested object, so nothing in it distinguishes "this id is not in the
// UI" from "this walk stopped early". Two independent things make it stop: the depth limit,
// and the hidden filter — which drops whole subtrees, not just the hidden node. Both are on
// by default (maxDepth 5, includeHidden false), so an incomplete dump is the normal reply
// rather than an edge case, and a caller reading absence out of one is reading a filter.
//
// These pin the completeness bookkeeping itself. The failure it exists to prevent was
// observed: a present element reported absent by a dump that gave no sign it had been cut.
#include <gtest/gtest.h>

#include "DebugServer/UITreeDump.h"

#include "UI/Internal/LayoutAccess.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>

using namespace GameEngine;
using nlohmann::json;

namespace
{

UIElement* AddChild(UIElement& parent, const std::string& id)
{
    auto child = std::make_unique<UIElement>();
    child->SetId(id);
    UIElement* raw = child.get();
    parent.AddChild(std::move(child));
    UILayoutAccess::SetLastLayoutRect(*raw, 0.0f, 0.0f, 10.0f, 10.0f);
    return raw;
}

void Hide(UIElement& element)
{
    element.GetMutableResolvedStyle().Visual.Visible = false;
}

// An element that reports text, without pulling a real Label (and its font machinery) into a
// test about the dump's bookkeeping. GetTextContent is the hook the dump reads.
class TextElement : public UIElement
{
public:
    explicit TextElement(std::string text) : m_Text(std::move(text)) {}
    const std::string& GetTextContent() const override { return m_Text; }

private:
    std::string m_Text;
};

TextElement* AddTextChild(UIElement& parent, const std::string& id, const std::string& text)
{
    auto child = std::make_unique<TextElement>(text);
    child->SetId(id);
    TextElement* raw = child.get();
    parent.AddChild(std::move(child));
    UILayoutAccess::SetLastLayoutRect(*raw, 0.0f, 0.0f, 10.0f, 10.0f);
    return raw;
}

// The dumped node carrying this id, or null. Walks children rather than searching the serialized
// text, because these tests assert on a per-node flag rather than on mere presence.
const json* FindNode(const json& tree, const std::string& id)
{
    if (!tree.is_object())
        return nullptr;
    if (tree.value("id", std::string()) == id)
        return &tree;
    if (const auto it = tree.find("ch"); it != tree.end()) // the dump's children key
        for (const auto& child : *it)
            if (const json* hit = FindNode(child, id))
                return hit;
    return nullptr;
}

// Does the serialized dump contain this id anywhere? This is exactly the search a caller runs
// before concluding an element is missing.
bool DumpContainsId(const json& tree, const std::string& id)
{
    return tree.dump().find("\"" + id + "\"") != std::string::npos;
}

json Dump(UIElement& root, const Editor::UITreeDumpOptions& options,
          Editor::UITreeDumpLookupAdvice advice = Editor::UITreeDumpLookupAdvice::RootIdAvailable)
{
    Editor::UITreeDumpCompleteness completeness;
    json tree = Editor::BuildUITreeDump(&root, options, completeness);
    Editor::AnnotateDumpCompleteness(tree, completeness, options, advice);
    return tree;
}

} // namespace

TEST(UITreeDumpTests, AFullyWalkedVisibleTreeReportsACompleteSearch)
{
    UIElement root;
    root.SetId("root");
    AddChild(*AddChild(root, "panel"), "button");

    const json tree = Dump(root, {/*MaxDepth=*/10, true, false});

    EXPECT_TRUE(tree.value("searchIsComplete", false));
    EXPECT_FALSE(tree.contains("truncatedAtDepth"));
    EXPECT_FALSE(tree.contains("hiddenElementsOmitted"));
    EXPECT_TRUE(DumpContainsId(tree, "button"));
}

TEST(UITreeDumpTests, DepthTruncationIsReportedAndDeniesCompleteness)
{
    UIElement root;
    UIElement* cursor = &root;
    for (int depth = 0; depth < 8; ++depth)
        cursor = AddChild(*cursor, "level" + std::to_string(depth));

    const json tree = Dump(root, {/*MaxDepth=*/3, true, false});

    EXPECT_FALSE(DumpContainsId(tree, "level7")) << "sanity: the deep id must be outside this dump";
    EXPECT_FALSE(tree.value("searchIsComplete", true))
        << "a dump that cannot see the deep id must not claim a complete search";
    EXPECT_EQ(tree.value("truncatedAtDepth", -1), 3);
    EXPECT_NE(tree.value("completenessNote", std::string()).find("maxDepth=3"), std::string::npos);
}

// The blocker this file was written for. The hidden filter removes ids from the dump without
// touching the depth counter, so a full-depth walk could report a complete search while having
// dropped whole subtrees — and the tool text tells callers to trust that field.
TEST(UITreeDumpTests, HiddenSubtreesOmittedDenyCompletenessEvenAtFullDepth)
{
    UIElement root;
    root.SetId("root");
    UIElement* toolButtons = AddChild(root, "InlineToolButtons");
    AddChild(*toolButtons, "InlineSelectBtn");
    Hide(*toolButtons);

    const json tree = Dump(root, {/*MaxDepth=*/50, true, /*IncludeHidden=*/false});

    EXPECT_FALSE(DumpContainsId(tree, "InlineSelectBtn"))
        << "sanity: a hidden container's children are dropped with it";
    EXPECT_FALSE(tree.value("searchIsComplete", true))
        << "depth was unlimited, but hidden subtrees were dropped — the search is not complete";
    EXPECT_TRUE(tree.value("hiddenElementsOmitted", false));
    EXPECT_NE(tree.value("completenessNote", std::string()).find("includeHidden"), std::string::npos)
        << "the note must name the parameter that would reveal them";
}

TEST(UITreeDumpTests, IncludingHiddenElementsRestoresACompleteSearch)
{
    UIElement root;
    UIElement* toolButtons = AddChild(root, "InlineToolButtons");
    AddChild(*toolButtons, "InlineSelectBtn");
    Hide(*toolButtons);

    const json tree = Dump(root, {/*MaxDepth=*/50, true, /*IncludeHidden=*/true});

    EXPECT_TRUE(DumpContainsId(tree, "InlineSelectBtn"));
    EXPECT_TRUE(tree.value("searchIsComplete", false));
}

// Both causes at once must be reported separately, because they need different fixes: one
// asks for more depth, the other for hidden elements.
TEST(UITreeDumpTests, DepthAndHiddenOmissionsAreReportedIndependently)
{
    UIElement root;
    UIElement* hiddenBranch = AddChild(root, "hiddenBranch");
    Hide(*hiddenBranch);
    UIElement* cursor = AddChild(root, "deepBranch");
    for (int depth = 0; depth < 6; ++depth)
        cursor = AddChild(*cursor, "deep" + std::to_string(depth));

    const json tree = Dump(root, {/*MaxDepth=*/2, true, /*IncludeHidden=*/false});

    EXPECT_FALSE(tree.value("searchIsComplete", true));
    EXPECT_EQ(tree.value("truncatedAtDepth", -1), 2);
    EXPECT_TRUE(tree.value("hiddenElementsOmitted", false));
    const std::string note = tree.value("completenessNote", std::string());
    EXPECT_NE(note.find("maxDepth=2"), std::string::npos) << note;
    EXPECT_NE(note.find("includeHidden"), std::string::npos) << note;
}

// get_panel_tree has no rootId parameter, so its note must not prescribe one.
TEST(UITreeDumpTests, DepthOnlyAdviceDoesNotPrescribeARootIdParameter)
{
    UIElement root;
    UIElement* cursor = &root;
    for (int depth = 0; depth < 4; ++depth)
        cursor = AddChild(*cursor, "level" + std::to_string(depth));

    const Editor::UITreeDumpOptions options{/*MaxDepth=*/1, true, false};
    const json withRootId = Dump(root, options, Editor::UITreeDumpLookupAdvice::RootIdAvailable);
    const json depthOnly = Dump(root, options, Editor::UITreeDumpLookupAdvice::DepthOnly);

    EXPECT_NE(withRootId.value("completenessNote", std::string()).find("rootId"), std::string::npos);
    EXPECT_EQ(depthOnly.value("completenessNote", std::string()).find("rootId"), std::string::npos)
        << "get_panel_tree's note must not send callers to a parameter it does not accept";
}

// A self-hidden subtree root yields no node at all. The handlers turn that into an explicit
// error; what is pinned here is that the builder really does produce nothing, which is what
// makes the bare reply indistinguishable from "no such element" without that handling.
TEST(UITreeDumpTests, ASelfHiddenRootProducesNoNode)
{
    UIElement root;
    root.SetId("assets-grid");
    Hide(root);

    Editor::UITreeDumpCompleteness completeness;
    const json tree = Editor::BuildUITreeDump(&root, {/*MaxDepth=*/10, true, /*IncludeHidden=*/false}, completeness);

    EXPECT_TRUE(tree.is_null());
    EXPECT_TRUE(completeness.HiddenOmitted);
    EXPECT_TRUE(Editor::IsHiddenForDump(root)) << "the handlers key their hidden error on this predicate";
}

// An element with no id, classes, text, layout or surviving children also emits nothing, so
// a null dump does not by itself mean "hidden" — the handlers must not report it as such.
// Keeping these two causes apart is why the hidden test above is keyed on a predicate rather
// than on the dump being empty.
TEST(UITreeDumpTests, AFeaturelessVisibleElementAlsoProducesNoNodeButIsNotHidden)
{
    UIElement root;

    Editor::UITreeDumpCompleteness completeness;
    const json tree = Editor::BuildUITreeDump(&root, {/*MaxDepth=*/10, true, /*IncludeHidden=*/false}, completeness);

    EXPECT_TRUE(tree.is_null());
    EXPECT_FALSE(completeness.HiddenOmitted);
    EXPECT_FALSE(Editor::IsHiddenForDump(root))
        << "a visible element that simply has nothing to report must not be reported as hidden";
}

// TEXT is the third thing a dump can omit, and the one that stayed silent longest. The walk cut
// every text value at 80 characters and marked it only with an ellipsis, so a caller searching the
// dump for a substring past the cut read a false absence — the same failure the depth and hidden
// bookkeeping above exists to prevent, on a different axis.
//
// It is tracked PER NODE rather than folded into searchIsComplete on purpose: searchIsComplete is
// the answer to "can this dump prove an id absent", and a truncated text value does not make an id
// unfindable. Folding it in would make the completeness flag false for dumps whose id search really
// is complete.
TEST(UITreeDumpTests, TextUnderTheLimitIsReturnedWholeAndUnflagged)
{
    UIElement root;
    root.SetId("root");
    const std::string text(100, 'a'); // over the OLD 80-char cap, under the current default
    AddTextChild(root, "label", text);

    const json tree = Dump(root, {/*MaxDepth=*/10, true, false});

    const json* node = FindNode(tree, "label");
    ASSERT_NE(node, nullptr) << "sanity: the text node must be in the dump";
    EXPECT_EQ(node->value("text", std::string()), text)
        << "a 100-character value must survive whole; the previous 80-char cap silently truncated "
           "ordinary tooltip and asset-path text";
    EXPECT_FALSE(node->contains("textTruncated"))
        << "nothing was cut, so nothing may claim it was";
    EXPECT_TRUE(tree.value("searchIsComplete", false));
}

TEST(UITreeDumpTests, TextOverTheLimitIsCutAndFlagged)
{
    UIElement root;
    root.SetId("root");
    const std::string text(Editor::kDefaultUITreeMaxTextLength + 50, 'b');
    AddTextChild(root, "label", text);

    const json tree = Dump(root, {/*MaxDepth=*/10, true, false});

    const json* node = FindNode(tree, "label");
    ASSERT_NE(node, nullptr);
    const std::string dumped = node->value("text", std::string());
    EXPECT_LT(dumped.size(), text.size()) << "sanity: this value must actually have been cut";
    EXPECT_EQ(dumped.size(), static_cast<size_t>(Editor::kDefaultUITreeMaxTextLength) + 3)
        << "the cut keeps the limit's worth of text plus the ellipsis";
    EXPECT_TRUE(node->value("textTruncated", false))
        << "a cut with no flag is exactly the false-absence this field exists to prevent";
    // The id search is unaffected by a text cut, so the completeness verdict must not change.
    EXPECT_TRUE(tree.value("searchIsComplete", false))
        << "truncated text does not make an id unfindable; searchIsComplete answers a different "
           "question and must stay true";
}

TEST(UITreeDumpTests, MaxTextLengthZeroReturnsWholeTextHoweverLong)
{
    UIElement root;
    root.SetId("root");
    const std::string text(Editor::kDefaultUITreeMaxTextLength * 4, 'c');
    AddTextChild(root, "label", text);

    const json tree = Dump(root, {/*MaxDepth=*/10, true, false, /*MaxTextLength=*/0});

    const json* node = FindNode(tree, "label");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->value("text", std::string()), text) << "0 means no limit";
    EXPECT_FALSE(node->contains("textTruncated"));
}
