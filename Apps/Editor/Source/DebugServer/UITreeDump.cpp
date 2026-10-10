#include "DebugServer/UITreeDump.h"

#include "UI/UIElement.h"

#include <string>

namespace GameEngine::Editor
{

using nlohmann::json;

namespace
{

// Builds a compact JSON subtree for a UI element hierarchy. Only non-empty/non-default
// fields are emitted, to keep the output readable.
json BuildNode(UIElement* element, int depth, const UITreeDumpOptions& options,
               UITreeDumpCompleteness& outCompleteness)
{
    if (!element)
        return nullptr;

    const bool hidden = IsHiddenForDump(*element);

    // Skip hidden subtrees unless explicitly requested. This drops the whole subtree, not
    // just the hidden node, so it removes ids that are present in the UI — recorded, because
    // a dump missing them cannot be read as proof that they do not exist.
    if (hidden && !options.IncludeHidden)
    {
        outCompleteness.HiddenOmitted = true;
        return nullptr;
    }

    json node;

    // Mark hidden elements so the caller knows they're not visible.
    if (hidden)
        node["hidden"] = true;

    const auto& id = element->GetId();
    if (!id.empty())
        node["id"] = id;

    const auto& classes = element->GetClasses();
    if (!classes.empty())
    {
        // Join classes into a single space-separated string instead of an array.
        std::string joined;
        for (const auto& cls : classes)
        {
            if (!joined.empty())
                joined += ' ';
            joined += cls;
        }
        node["cls"] = std::move(joined);
    }

    std::string text = element->GetTextContent();
    if (!text.empty())
    {
        // A cut is FLAGGED, not just marked with an ellipsis: the dump is routinely searched for a
        // substring, and text that ended mid-string with no signal answered "absent" for something
        // present. The ellipsis is for human readers; textTruncated is what a caller can assert on.
        const size_t limit = options.MaxTextLength > 0 ? static_cast<size_t>(options.MaxTextLength)
                                                       : text.size();
        if (text.size() > limit)
        {
            text.resize(limit);
            text += "...";
            node["textTruncated"] = true;
        }
        node["text"] = std::move(text);
    }

    if (options.IncludeLayout)
    {
        const float w = element->GetLayoutWidth();
        const float h = element->GetLayoutHeight();
        // Only include position/size if the element has actual dimensions.
        if (w > 0.0f || h > 0.0f)
        {
            node["x"] = static_cast<int>(element->GetLayoutX());
            node["y"] = static_cast<int>(element->GetLayoutY());
            node["w"] = static_cast<int>(w);
            node["h"] = static_cast<int>(h);
        }
    }

    if (depth < options.MaxDepth)
    {
        json children = json::array();
        for (auto& child : element->GetChildren())
        {
            if (!child)
                continue;
            json childNode = BuildNode(child.get(), depth + 1, options, outCompleteness);
            if (!childNode.is_null())
                children.push_back(std::move(childNode));
        }
        // Follow mount targets (portal-like non-owned children used by the dock system).
        if (UIElement* mounted = element->GetMountTarget())
        {
            json mountedNode = BuildNode(mounted, depth + 1, options, outCompleteness);
            if (!mountedNode.is_null())
                children.push_back(std::move(mountedNode));
        }
        if (!children.empty())
            node["ch"] = std::move(children);
    }
    else
    {
        // At max depth, indicate truncated children with a count.
        size_t childCount = element->GetChildren().size();
        if (element->GetMountTarget())
            childCount += 1;
        if (childCount > 0)
        {
            node["childCount"] = childCount;
            outCompleteness.TruncatedAtDepth = true;
        }
    }

    return node;
}

} // namespace

bool IsHiddenForDump(const UIElement& element)
{
    const auto& style = element.GetResolvedStyle();
    return !style.Visual.Visible || style.Layout.DisplayMode == DisplayMode::None;
}

json BuildUITreeDump(UIElement* root, const UITreeDumpOptions& options, UITreeDumpCompleteness& outCompleteness)
{
    return BuildNode(root, 0, options, outCompleteness);
}

void AnnotateDumpCompleteness(json& tree, const UITreeDumpCompleteness& completeness,
                              const UITreeDumpOptions& options, UITreeDumpLookupAdvice advice)
{
    if (!tree.is_object())
        return;

    tree["searchIsComplete"] = completeness.IsComplete();
    if (completeness.IsComplete())
        return;

    std::string note = "This tree cannot prove an id is absent: ";
    if (completeness.TruncatedAtDepth)
    {
        tree["truncatedAtDepth"] = options.MaxDepth;
        note += "children were dropped at maxDepth=" + std::to_string(options.MaxDepth) + " (raise maxDepth)";
    }
    if (completeness.HiddenOmitted)
    {
        tree["hiddenElementsOmitted"] = true;
        if (completeness.TruncatedAtDepth)
            note += ", and ";
        note += "hidden subtrees were skipped (pass includeHidden:true)";
    }
    note += '.';
    if (advice == UITreeDumpLookupAdvice::RootIdAvailable)
        note += " To check one id regardless of depth, pass it as rootId; that lookup searches the whole tree.";

    tree["completenessNote"] = std::move(note);
}

} // namespace GameEngine::Editor
