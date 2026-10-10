#include "UI/UIManager.h"
#include "UIManager_Internal.h"
#include "UI/UIElement.h"
#include "UI/ResolvedStyle.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/TreeView.h"
#include "UIAttributeAccess.h"

#include "Logger/Logger.h"

#include <string>
#include <vector>
#include <unordered_map>

using namespace GameEngine;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA

void UIManager::EmitDebugCapture()
{
    auto escapeJson = [](const std::string& s) -> std::string
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out.push_back((char)c);
                }
                break;
            }
        }
        return out;
    };
    auto writeString = [&](const std::string& s)
    {
        m_DebugCaptureOut << "\"" << escapeJson(s) << "\"";
    };
    auto writeBool = [&](bool v)
    { m_DebugCaptureOut << (v ? "true" : "false"); };
    auto getClipFor = [&](UIElement* el, float& cx, float& cy, float& cw, float& ch) -> bool
    {
        if (!el)
            return false;
        auto it = m_DebugClipCache.find(el);
        if (it == m_DebugClipCache.end())
            return false;
        cx = it->second.X;
        cy = it->second.Y;
        cw = it->second.W;
        ch = it->second.H;
        return true;
    };

    auto findLabelUnder = [](UIElement* root) -> UIElement*
    {
        if (!root)
            return nullptr;
        std::vector<UIElement*> stack;
        stack.reserve(16);
        stack.push_back(root);
        while (!stack.empty())
        {
            UIElement* el = stack.back();
            stack.pop_back();
            if (!el)
                continue;
            if (el != root && el->HasClass("grid-title"))
            {
                return el;
            }
            for (const auto& ch : el->GetChildren())
            {
                if (ch)
                    stack.push_back(ch.get());
            }
        }
        return nullptr;
    };

    auto findThumbUnder = [](UIElement* root) -> UIElement*
    {
        if (!root)
            return nullptr;
        std::vector<UIElement*> stack;
        stack.reserve(16);
        stack.push_back(root);
        while (!stack.empty())
        {
            UIElement* el = stack.back();
            stack.pop_back();
            if (!el)
                continue;
            if (el != root && el->HasClass("thumb"))
            {
                return el;
            }
            for (const auto& ch : el->GetChildren())
            {
                if (ch)
                    stack.push_back(ch.get());
            }
        }
        return nullptr;
    };

    auto truncateText = [](const std::string& s) -> std::string
    {
        constexpr size_t kMax = 64;
        if (s.size() <= kMax)
            return s;
        return s.substr(0, kMax - 3) + "...";
    };


    auto writeElementProbe = [&](const char* probeId)
    {
        m_DebugCaptureOut << "{";
        m_DebugCaptureOut << "\"id\":";
        writeString(probeId ? probeId : "");

        UIElement* el = (m_Root && probeId) ? m_Root->FindById(probeId) : nullptr;
        m_DebugCaptureOut << ",\"exists\":";
        writeBool(el != nullptr);
        if (el)
        {
            m_DebugCaptureOut << ",\"instanceId\":" << el->GetInstanceId();
            m_DebugCaptureOut << ",\"tag\":";
            writeString(UIAttributeAccess::GetDebugTypeName(*el));
            m_DebugCaptureOut << ",\"layout\":[" << el->GetLayoutX() << "," << el->GetLayoutY() << ","
                              << el->GetLayoutWidth() << "," << el->GetLayoutHeight() << "]";

            const ResolvedStyle& cs = el->GetResolvedStyle();
            const bool styleVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
            m_DebugCaptureOut << ",\"styleVisible\":";
            writeBool(styleVisible);
            m_DebugCaptureOut << ",\"opacity\":" << cs.Visual.Opacity;
            m_DebugCaptureOut << ",\"displayMode\":" << static_cast<int>(cs.Layout.DisplayMode);

            float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
            if (getClipFor(el, cx, cy, cw, ch))
            {
                m_DebugCaptureOut << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";
            }
        }
        m_DebugCaptureOut << "}";
    };

    static thread_local std::vector<GridView*> grids;
    static thread_local std::vector<TreeView*> trees;
    uint32_t sliderClassCount = 0;
    uint32_t colorPickerClassCount = 0;
    grids.clear();
    trees.clear();
    if (m_Root)
    {
        static thread_local std::vector<UIElement*> scanStack;
        scanStack.clear();
        scanStack.reserve(256);
        scanStack.push_back(m_Root.get());
        while (!scanStack.empty())
        {
            UIElement* el = scanStack.back();
            scanStack.pop_back();
            if (!el)
                continue;
            if (auto* gv = dynamic_cast<GridView*>(el))
            {
                grids.push_back(gv);
            }
            if (auto* tv = dynamic_cast<TreeView*>(el))
            {
                trees.push_back(tv);
            }
            if (el->HasClass("slider"))
                ++sliderClassCount;
            if (el->HasClass("colorpicker-view") || el->HasClass("colorpicker-svbox") || el->HasClass("colorpicker-huebar"))
                ++colorPickerClassCount;
            for (const auto& ch : el->GetChildren())
            {
                if (ch)
                    scanStack.push_back(ch.get());
            }
            if (auto* m = dynamic_cast<Mount*>(el))
            {
                if (UIElement* tgt = m->GetTarget())
                    scanStack.push_back(tgt);
            }
        }
    }

    UIElement* colorPickerDialog = m_Root ? m_Root->FindById("colorpicker-dialog-picker") : nullptr;
    UIElement* colorPickerPalette = m_Root ? m_Root->FindById("colorpicker-palette") : nullptr;
    const size_t paletteSwatchCount = colorPickerPalette ? colorPickerPalette->GetChildren().size() : 0u;

    static constexpr const char* kColorPickerProbeIds[] =
        {
            "colorpicker-dialog-picker",
            "colorpicker-mode-toggle",
            "colorpicker-color-area",
            "colorpicker-wheel",
            "colorpicker-value-slider",
            "colorpicker-svbox",
            "colorpicker-svbox-marker",
            "colorpicker-huebar",
            "colorpicker-huebar-marker",
            "colorpicker-swatch",
            "colorpicker-alpha-value",
            "colorpicker-hdr-value",
            "colorpicker-hex",
            "colorpicker-palette-label",
            "colorpicker-palette"};
    constexpr size_t kColorPickerProbeCount = sizeof(kColorPickerProbeIds) / sizeof(kColorPickerProbeIds[0]);

    m_DebugCaptureOut << std::fixed << std::setprecision(3);
    m_DebugCaptureOut << "{";
    m_DebugCaptureOut << "\"kind\":\"ui_debug_frame\",";
    m_DebugCaptureOut << "\"seq\":" << m_DebugCaptureSeq++ << ",";
    m_DebugCaptureOut << "\"time\":" << m_Time << ",";
    m_DebugCaptureOut << "\"hoverId\":";
    writeString(GetHoveredElementDebugName());
    m_DebugCaptureOut << ",";
    m_DebugCaptureOut << "\"focusId\":";
    writeString(m_FocusId);
    m_DebugCaptureOut << ",";
    m_DebugCaptureOut << "\"captureId\":";
    writeString(m_CaptureId);
    m_DebugCaptureOut << ",";

    m_DebugCaptureOut << "\"managerState\":{";
    m_DebugCaptureOut << "\"globalStylesheets\":" << m_GlobalStylesheets.size() << ",";
    m_DebugCaptureOut << "\"fileStylesheets\":" << m_FileStylesheets.size() << ",";
    m_DebugCaptureOut << "\"externalTextures\":" << m_ExternalRGRegistrations.size() << ",";
    m_DebugCaptureOut << "\"externalDeviceTextures\":" << m_ExternalDeviceTextures.size() << ",";
    m_DebugCaptureOut << "\"bgTextureCache\":" << m_BgTextureCache.size() << ",";
    m_DebugCaptureOut << "\"rootChildCount\":" << (m_Root ? m_Root->GetChildren().size() : 0u);
    m_DebugCaptureOut << "},";

    const auto renderDiag = GetRenderSlotDiagnostics();
    m_DebugCaptureOut << "\"renderSlots\":{";
    m_DebugCaptureOut << "\"activeWindowTargetId\":" << renderDiag.ActiveWindowTargetId << ",";
    m_DebugCaptureOut << "\"deviceFrameIndex\":" << renderDiag.DeviceFrameIndex << ",";
    m_DebugCaptureOut << "\"targetFrameIndex\":" << renderDiag.TargetFrameIndex << ",";
    m_DebugCaptureOut << "\"uiFrameSlot\":" << renderDiag.UiFrameSlot << ",";
    m_DebugCaptureOut << "\"rgPoolSlot\":" << renderDiag.RgPoolSlot;
    m_DebugCaptureOut << "},";

    m_DebugCaptureOut << "\"colorPickerProbe\":{";
    m_DebugCaptureOut << "\"present\":";
    writeBool(colorPickerDialog != nullptr);
    m_DebugCaptureOut << ",";
    m_DebugCaptureOut << "\"sliderClassCount\":" << sliderClassCount << ",";
    m_DebugCaptureOut << "\"colorPickerClassCount\":" << colorPickerClassCount << ",";
    m_DebugCaptureOut << "\"paletteSwatchCount\":" << paletteSwatchCount << ",";
    m_DebugCaptureOut << "\"elements\":[";
    for (size_t i = 0; i < kColorPickerProbeCount; ++i)
    {
        if (i != 0)
            m_DebugCaptureOut << ",";
        writeElementProbe(kColorPickerProbeIds[i]);
    }
    m_DebugCaptureOut << "]";
    m_DebugCaptureOut << "},";

    m_DebugCaptureOut << "\"gridViews\":[";
    bool firstGrid = true;
    for (auto* gv : grids)
    {
        if (!gv)
            continue;
        if (!firstGrid)
            m_DebugCaptureOut << ",";
        firstGrid = false;

        const std::string gid = gv->GetId().empty()
                                    ? (UIAttributeAccess::GetDebugTypeName(*gv) + "#" + std::to_string(gv->GetInstanceId()))
                                    : gv->GetId();

        static thread_local std::vector<UIElement*> cells;
        cells.clear();
        cells.reserve(64);
        {
            static thread_local std::vector<UIElement*> cellStack;
            cellStack.clear();
            cellStack.reserve(128);
            cellStack.push_back(gv);
            while (!cellStack.empty())
            {
                UIElement* el = cellStack.back();
                cellStack.pop_back();
                if (!el)
                    continue;
                if (el->HasClass("grid-cell"))
                    cells.push_back(el);
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        cellStack.push_back(ch.get());
                }
            }
        }

        int totalCells = 0;
        int visibleCells = 0;
        int invisibleCells = 0;
        int labelMissing = 0;
        int labelHidden = 0;
        int labelClipped = 0;

        struct CellSample
        {
            UIElement* cell = nullptr;
            UIElement* label = nullptr;
            float x = 0.0f;
            float y = 0.0f;
            // Visible intersection with the effective clip (typically the ScrollView viewport).
            float visX = 0.0f;
            float visY = 0.0f;
            float visW = 0.0f;
            float visH = 0.0f;
            bool intersectsClip = false;
        };
        static thread_local std::vector<CellSample> samples;
        samples.clear();
        samples.reserve(cells.size());

        for (UIElement* cell : cells)
        {
            if (!cell)
                continue;
            ++totalCells;
            const ResolvedStyle& cs = cell->GetResolvedStyle();
            const bool cellVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
            if (cellVisible)
                ++visibleCells;
            else
                ++invisibleCells;

            UIElement* label = findLabelUnder(cell);
            if (!label)
            {
                ++labelMissing;
            }
            else
            {
                const std::string text = label->GetTextContent();
                if (text.empty())
                    ++labelMissing;

                const ResolvedStyle& ls = label->GetResolvedStyle();
                const bool labelVisible = ls.Layout.DisplayMode != DisplayMode::None && ls.Visual.Visible && ls.Visual.Opacity > 0.01f;
                if (!labelVisible)
                    ++labelHidden;

                const float cx = cell->GetLayoutX();
                const float cy = cell->GetLayoutY();
                const float cw = cell->GetLayoutWidth();
                const float ch = cell->GetLayoutHeight();
                const float lx = label->GetLayoutX();
                const float ly = label->GetLayoutY();
                const float lw = label->GetLayoutWidth();
                const float lh = label->GetLayoutHeight();
                if (lw > 0.0f && lh > 0.0f)
                {
                    const bool over =
                        (lx < cx - 0.5f) ||
                        (ly < cy - 0.5f) ||
                        (lx + lw > cx + cw + 0.5f) ||
                        (ly + lh > cy + ch + 0.5f);
                    if (over)
                        ++labelClipped;
                }
                // Prefer sampling cells that actually intersect the viewport clip. This avoids
                // capturing only offscreen pool items when the grid is scrolled far down.
                float clipX = 0.0f, clipY = 0.0f, clipW = 0.0f, clipH = 0.0f;
                float visX = 0.0f, visY = 0.0f, visW = 0.0f, visH = 0.0f;
                bool intersects = false;
                if (getClipFor(cell, clipX, clipY, clipW, clipH) && clipW > 0.0f && clipH > 0.0f)
                {
                    const float x0 = std::max(cx, clipX);
                    const float y0 = std::max(cy, clipY);
                    const float x1 = std::min(cx + cw, clipX + clipW);
                    const float y1 = std::min(cy + ch, clipY + clipH);
                    visX = x0;
                    visY = y0;
                    visW = std::max(0.0f, x1 - x0);
                    visH = std::max(0.0f, y1 - y0);
                    intersects = (visW > 1.0f && visH > 1.0f);
                }
                samples.push_back(CellSample{cell, label, cx, cy, visX, visY, visW, visH, intersects});
            }
        }

        std::sort(samples.begin(), samples.end(), [](const CellSample& a, const CellSample& b)
                  {
            if (a.y == b.y)
                return a.x < b.x;
            return a.y < b.y; });

        m_DebugCaptureOut << "{";
        m_DebugCaptureOut << "\"id\":";
        writeString(gid);
        m_DebugCaptureOut << ",";
        m_DebugCaptureOut << "\"layout\":[" << gv->GetLayoutX() << "," << gv->GetLayoutY() << "," << gv->GetLayoutWidth()
                          << "," << gv->GetLayoutHeight() << "],";
        m_DebugCaptureOut << "\"scrollY\":" << gv->GetScrollOffsetY() << ",";
        m_DebugCaptureOut << "\"viewportW\":" << gv->GetViewportWidth() << ",";
        m_DebugCaptureOut << "\"viewportH\":" << gv->GetViewportHeight() << ",";
        m_DebugCaptureOut << "\"contentW\":" << gv->GetContentWidth() << ",";
        m_DebugCaptureOut << "\"contentH\":" << gv->GetContentHeight() << ",";
        m_DebugCaptureOut << "\"columns\":" << gv->GetColumnCount() << ",";
        m_DebugCaptureOut << "\"firstRow\":" << gv->GetFirstVisibleRow() << ",";
        m_DebugCaptureOut << "\"desiredCells\":" << gv->GetDesiredCellCount() << ",";
        m_DebugCaptureOut << "\"lastColumns\":" << gv->GetLastKnownColumns() << ",";
        m_DebugCaptureOut << "\"lastContentHeightPx\":" << gv->GetLastContentHeightPx() << ",";
        m_DebugCaptureOut << "\"lastItemCount\":" << gv->GetLastKnownItemCount() << ",";
        m_DebugCaptureOut << "\"cellSummary\":{";
        m_DebugCaptureOut << "\"total\":" << totalCells << ",";
        m_DebugCaptureOut << "\"visible\":" << visibleCells << ",";
        m_DebugCaptureOut << "\"invisible\":" << invisibleCells << ",";
        m_DebugCaptureOut << "\"labelMissing\":" << labelMissing << ",";
        m_DebugCaptureOut << "\"labelHidden\":" << labelHidden << ",";
        m_DebugCaptureOut << "\"labelClipped\":" << labelClipped;
        m_DebugCaptureOut << "},";
        m_DebugCaptureOut << "\"cells\":[";

        // Emit a mix of top-visible and bottom-visible cells so captures show what the user sees,
        // even when scrolled to the very bottom.
        static thread_local std::vector<size_t> sampleIndices;
        sampleIndices.clear();
        sampleIndices.reserve(12);
        static thread_local std::vector<size_t> visibleIdx;
        visibleIdx.clear();
        visibleIdx.reserve(samples.size());
        for (size_t i = 0; i < samples.size(); ++i)
        {
            if (samples[i].intersectsClip)
                visibleIdx.push_back(i);
        }
        auto pushUnique = [&](size_t idx)
        {
            for (size_t v : sampleIndices)
                if (v == idx)
                    return;
            sampleIndices.push_back(idx);
        };
        const size_t half = 6;
        if (!visibleIdx.empty())
        {
            for (size_t i = 0; i < visibleIdx.size() && i < half; ++i)
                pushUnique(visibleIdx[i]);
            for (size_t k = 0; k < visibleIdx.size() && k < half; ++k)
            {
                const size_t idx = visibleIdx[visibleIdx.size() - 1 - k];
                pushUnique(idx);
            }
        }
        // Fallback fill: if we still have few samples, take from the start of all cells.
        for (size_t i = 0; i < samples.size() && sampleIndices.size() < 12; ++i)
            pushUnique(i);

        for (size_t si = 0; si < sampleIndices.size(); ++si)
        {
            const CellSample& s = samples[sampleIndices[si]];
            if (si != 0)
                m_DebugCaptureOut << ",";
            UIElement* cell = s.cell;
            UIElement* label = s.label;
            const std::string cellId = cell->GetId().empty()
                                           ? (UIAttributeAccess::GetDebugTypeName(*cell) + "#" + std::to_string(cell->GetInstanceId()))
                                           : cell->GetId();
            const ResolvedStyle& cs = cell->GetResolvedStyle();
            const bool cellVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
            const float cellOpacity = cs.Visual.Opacity;

            const std::string labelText = label ? truncateText(label->GetTextContent()) : std::string();
            const ResolvedStyle* ls = label ? &label->GetResolvedStyle() : nullptr;
            const bool labelVisible = !ls || (ls->Layout.DisplayMode != DisplayMode::None && ls->Visual.Visible && ls->Visual.Opacity > 0.01f);
            const float labelOpacity = ls ? ls->Visual.Opacity : 1.0f;

            m_DebugCaptureOut << "{";
            m_DebugCaptureOut << "\"id\":";
            writeString(cellId);
            m_DebugCaptureOut << ",";
            m_DebugCaptureOut << "\"visible\":";
            writeBool(cellVisible);
            m_DebugCaptureOut << ",";
            m_DebugCaptureOut << "\"opacity\":" << cellOpacity << ",";
            m_DebugCaptureOut << "\"layout\":[" << cell->GetLayoutX() << "," << cell->GetLayoutY() << ","
                              << cell->GetLayoutWidth() << "," << cell->GetLayoutHeight() << "]";
            if (s.intersectsClip)
            {
                m_DebugCaptureOut << ",\"visibleRect\":[" << s.visX << "," << s.visY << "," << s.visW << "," << s.visH << "]";
            }

            float cx = 0, cy = 0, cw = 0, ch = 0;
            if (getClipFor(cell, cx, cy, cw, ch))
            {
                m_DebugCaptureOut << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";
            }


            if (label)
            {
                m_DebugCaptureOut << ",\"label\":{";
                m_DebugCaptureOut << "\"text\":";
                writeString(labelText);
                m_DebugCaptureOut << ",";
                m_DebugCaptureOut << "\"visible\":";
                writeBool(labelVisible);
                m_DebugCaptureOut << ",";
                m_DebugCaptureOut << "\"opacity\":" << labelOpacity << ",";
                m_DebugCaptureOut << "\"layout\":[" << label->GetLayoutX() << "," << label->GetLayoutY() << ","
                                  << label->GetLayoutWidth() << "," << label->GetLayoutHeight() << "]";

                float lx = 0, ly = 0, lw = 0, lh = 0;
                if (getClipFor(label, lx, ly, lw, lh))
                {
                    m_DebugCaptureOut << ",\"clip\":[" << lx << "," << ly << "," << lw << "," << lh << "]";
                }
                m_DebugCaptureOut << "}";
            }
            m_DebugCaptureOut << "}";
        }
        m_DebugCaptureOut << "]";
        m_DebugCaptureOut << "}";
    }
    m_DebugCaptureOut << "],";

    m_DebugCaptureOut << "\"treeViews\":[";
    bool firstTree = true;
    for (auto* tv : trees)
    {
        if (!tv)
            continue;
        if (!firstTree)
            m_DebugCaptureOut << ",";
        firstTree = false;

        const std::string tid = tv->GetId().empty()
                                    ? (UIAttributeAccess::GetDebugTypeName(*tv) + "#" + std::to_string(tv->GetInstanceId()))
                                    : tv->GetId();

        // Collect pooled rows under this TreeView.
        // PERF: debug capture is optional, but reuse containers when enabled.
        static thread_local std::vector<UIElement*> rows;
        rows.clear();
        rows.reserve(128);
        {
            static thread_local std::vector<UIElement*> rowStack;
            rowStack.clear();
            rowStack.reserve(128);
            rowStack.push_back(tv);
            while (!rowStack.empty())
            {
                UIElement* el = rowStack.back();
                rowStack.pop_back();
                if (!el)
                    continue;
                if (el->HasClass("tree-item"))
                    rows.push_back(el);
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        rowStack.push_back(ch.get());
                }
            }
        }

        auto findTreeTitleUnder = [](UIElement* root) -> UIElement*
        {
            if (!root)
                return nullptr;
            static thread_local std::vector<UIElement*> stack;
            stack.clear();
            stack.reserve(16);
            stack.push_back(root);
            while (!stack.empty())
            {
                UIElement* el = stack.back();
                stack.pop_back();
                if (!el)
                    continue;
                if (el != root && el->HasClass("tree-title"))
                    return el;
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        stack.push_back(ch.get());
                }
            }
            return nullptr;
        };
        auto findTreeFoldoutUnder = [](UIElement* root) -> UIElement*
        {
            if (!root)
                return nullptr;
            static thread_local std::vector<UIElement*> stack;
            stack.clear();
            stack.reserve(16);
            stack.push_back(root);
            while (!stack.empty())
            {
                UIElement* el = stack.back();
                stack.pop_back();
                if (!el)
                    continue;
                if (el != root && el->HasClass("foldout"))
                    return el;
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        stack.push_back(ch.get());
                }
            }
            return nullptr;
        };

        m_DebugCaptureOut << "{";
        m_DebugCaptureOut << "\"id\":";
        writeString(tid);
        m_DebugCaptureOut << ",";
        m_DebugCaptureOut << "\"layout\":[" << tv->GetLayoutX() << "," << tv->GetLayoutY() << "," << tv->GetLayoutWidth()
                          << "," << tv->GetLayoutHeight() << "],";
        m_DebugCaptureOut << "\"scrollX\":" << tv->GetScrollOffsetX() << ",";
        m_DebugCaptureOut << "\"scrollY\":" << tv->GetScrollOffsetY() << ",";
        m_DebugCaptureOut << "\"viewportW\":" << tv->GetViewportWidth() << ",";
        m_DebugCaptureOut << "\"viewportH\":" << tv->GetViewportHeight() << ",";
        m_DebugCaptureOut << "\"contentW\":" << tv->GetContentWidth() << ",";
        m_DebugCaptureOut << "\"contentH\":" << tv->GetContentHeight() << ",";
        m_DebugCaptureOut << "\"firstIndex\":" << tv->GetFirstVisibleIndex() << ",";
        m_DebugCaptureOut << "\"desiredRows\":" << tv->GetDesiredRowCount() << ",";
        m_DebugCaptureOut << "\"flatSize\":" << tv->GetLastKnownFlatSize() << ",";
        m_DebugCaptureOut << "\"lastViewportWPx\":" << tv->GetLastViewportWPx() << ",";

        struct RowSample
        {
            UIElement* row = nullptr;
            UIElement* title = nullptr;
            UIElement* foldout = nullptr;
            float x = 0.0f;
            float y = 0.0f;
            float visX = 0.0f;
            float visY = 0.0f;
            float visW = 0.0f;
            float visH = 0.0f;
            bool intersectsClip = false;
        };
        static thread_local std::vector<RowSample> samples;
        samples.clear();
        samples.reserve(rows.size());
        for (UIElement* row : rows)
        {
            if (!row)
                continue;
            UIElement* title = findTreeTitleUnder(row);
            UIElement* fold = findTreeFoldoutUnder(row);
            const float rx = row->GetLayoutX();
            const float ry = row->GetLayoutY();
            const float rw = row->GetLayoutWidth();
            const float rh = row->GetLayoutHeight();
            float clipX = 0.0f, clipY = 0.0f, clipW = 0.0f, clipH = 0.0f;
            float visX = 0.0f, visY = 0.0f, visW = 0.0f, visH = 0.0f;
            bool intersects = false;
            if (getClipFor(row, clipX, clipY, clipW, clipH) && clipW > 0.0f && clipH > 0.0f)
            {
                const float x0 = std::max(rx, clipX);
                const float y0 = std::max(ry, clipY);
                const float x1 = std::min(rx + rw, clipX + clipW);
                const float y1 = std::min(ry + rh, clipY + clipH);
                visX = x0;
                visY = y0;
                visW = std::max(0.0f, x1 - x0);
                visH = std::max(0.0f, y1 - y0);
                intersects = (visW > 1.0f && visH > 1.0f);
            }
            samples.push_back(RowSample{row, title, fold, rx, ry, visX, visY, visW, visH, intersects});
        }
        std::sort(samples.begin(), samples.end(), [](const RowSample& a, const RowSample& b)
                  {
            if (a.y == b.y)
                return a.x < b.x;
            return a.y < b.y; });

        static thread_local std::vector<size_t> sampleIndices;
        sampleIndices.clear();
        sampleIndices.reserve(10);
        static thread_local std::vector<size_t> visibleIdx;
        visibleIdx.clear();
        visibleIdx.reserve(samples.size());
        for (size_t i = 0; i < samples.size(); ++i)
            if (samples[i].intersectsClip)
                visibleIdx.push_back(i);
        auto pushUnique = [&](size_t idx)
        {
            for (size_t v : sampleIndices)
                if (v == idx)
                    return;
            sampleIndices.push_back(idx);
        };
        const size_t half = 5;
        if (!visibleIdx.empty())
        {
            for (size_t i = 0; i < visibleIdx.size() && i < half; ++i)
                pushUnique(visibleIdx[i]);
            for (size_t k = 0; k < visibleIdx.size() && k < half; ++k)
                pushUnique(visibleIdx[visibleIdx.size() - 1 - k]);
        }
        for (size_t i = 0; i < samples.size() && sampleIndices.size() < 10; ++i)
            pushUnique(i);

        m_DebugCaptureOut << "\"rows\":[";
        for (size_t si = 0; si < sampleIndices.size(); ++si)
        {
            const RowSample& s = samples[sampleIndices[si]];
            if (si != 0)
                m_DebugCaptureOut << ",";

            UIElement* row = s.row;
            UIElement* title = s.title;
            const std::string rowId = row->GetId().empty()
                                          ? (UIAttributeAccess::GetDebugTypeName(*row) + "#" + std::to_string(row->GetInstanceId()))
                                          : row->GetId();
            const ResolvedStyle& rowStyle = row->GetResolvedStyle();
            const bool rowVisible = rowStyle.Layout.DisplayMode != DisplayMode::None && rowStyle.Visual.Visible && rowStyle.Visual.Opacity > 0.01f;
            const float rowOpacity = rowStyle.Visual.Opacity;
            const std::string titleText = title ? truncateText(title->GetTextContent()) : std::string();

            m_DebugCaptureOut << "{";
            m_DebugCaptureOut << "\"id\":";
            writeString(rowId);
            m_DebugCaptureOut << ",";
            m_DebugCaptureOut << "\"visible\":";
            writeBool(rowVisible);
            m_DebugCaptureOut << ",";
            m_DebugCaptureOut << "\"opacity\":" << rowOpacity << ",";
            m_DebugCaptureOut << "\"layout\":[" << row->GetLayoutX() << "," << row->GetLayoutY() << ","
                              << row->GetLayoutWidth() << "," << row->GetLayoutHeight() << "]";
            if (s.intersectsClip)
            {
                m_DebugCaptureOut << ",\"visibleRect\":[" << s.visX << "," << s.visY << "," << s.visW << "," << s.visH << "]";
            }
            float cx = 0, cy = 0, cw = 0, ch = 0;
            if (getClipFor(row, cx, cy, cw, ch))
                m_DebugCaptureOut << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";

            if (!titleText.empty())
            {
                m_DebugCaptureOut << ",\"title\":{";
                m_DebugCaptureOut << "\"text\":";
                writeString(titleText);
                m_DebugCaptureOut << ",\"layout\":[" << title->GetLayoutX() << "," << title->GetLayoutY() << ","
                                  << title->GetLayoutWidth() << "," << title->GetLayoutHeight() << "]";
                float tcx = 0, tcy = 0, tcw = 0, tch = 0;
                if (getClipFor(title, tcx, tcy, tcw, tch))
                    m_DebugCaptureOut << ",\"clip\":[" << tcx << "," << tcy << "," << tcw << "," << tch << "]";
                m_DebugCaptureOut << "}";
            }

            m_DebugCaptureOut << "}";
        }
        m_DebugCaptureOut << "]";

        m_DebugCaptureOut << "}";
    }
    m_DebugCaptureOut << "]";
    m_DebugCaptureOut << "}\n";
    m_DebugCaptureOut.flush();
}

#endif

