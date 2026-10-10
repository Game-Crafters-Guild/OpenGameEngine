#pragma once

#include <nlohmann/json.hpp>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

// Text longer than this is cut short, with the node flagged so the cut is visible. Generous enough
// that ordinary labels, paths and tooltips survive whole: the previous 80 silently truncated both,
// and a caller searching for a substring past the cut read a false absence. 0 means no limit.
//
// A cut is recorded PER NODE (textTruncated) and deliberately does NOT feed searchIsComplete below:
// that field answers "can this dump prove an id absent", and truncating a text value does not make
// an id unfindable. Folding the two together would deny completeness to dumps whose id search is in
// fact complete.
constexpr int kDefaultUITreeMaxTextLength = 512;

struct UITreeDumpOptions
{
    int MaxDepth = 5;
    bool IncludeLayout = true;
    bool IncludeHidden = false;
    int MaxTextLength = kDefaultUITreeMaxTextLength;
};

// What the walk had to leave out. Both causes make the dump an incomplete answer to "is this
// id present?", and they are tracked apart because a caller fixes them differently: one by
// asking for more depth, the other by asking for hidden elements.
struct UITreeDumpCompleteness
{
    bool TruncatedAtDepth = false;
    bool HiddenOmitted = false;

    bool IsComplete() const { return !TruncatedAtDepth && !HiddenOmitted; }
};

// Which advice the completeness note may offer. get_ui_tree resolves a single id at any depth
// through its rootId parameter; get_panel_tree has no such parameter, and a note telling its
// callers to use one would prescribe a parameter that does not exist.
enum class UITreeDumpLookupAdvice
{
    RootIdAvailable,
    DepthOnly,
};

// Whether the dump filters this element out when IncludeHidden is false. Shared with the
// handlers so "the walk produced nothing because the element is hidden" and "the walk
// produced nothing because the element had nothing to report" stay distinguishable — a
// featureless element emits no node either way.
bool IsHiddenForDump(const UIElement& element);

// Builds the compact JSON subtree the debug server reports, recording what it dropped.
nlohmann::json BuildUITreeDump(UIElement* root, const UITreeDumpOptions& options,
                               UITreeDumpCompleteness& outCompleteness);

// Stamps the reply with whether a search over it can prove an id absent.
//
// The tree is otherwise a plain nested object: nothing in it distinguishes "this id is not in
// the UI" from "this dump does not reach that far" or "this dump omits hidden elements", and
// both omissions are the normal case rather than an edge one — the default depth of 5 stops
// well above panel content, and the default excludes every hidden subtree. `searchIsComplete`
// is the field to assert on; it is true only when neither omission occurred.
void AnnotateDumpCompleteness(nlohmann::json& tree, const UITreeDumpCompleteness& completeness,
                              const UITreeDumpOptions& options, UITreeDumpLookupAdvice advice);

} // namespace GameEngine::Editor
