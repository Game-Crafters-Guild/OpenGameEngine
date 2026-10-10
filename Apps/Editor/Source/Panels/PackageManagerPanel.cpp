#include "Panels/PackageManagerPanel.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{

// Master (list) vs detail (controls) split. Detail is wider so its action
// cluster has room — the "buttons don't fit" complaint came from cramming the
// controls into the row width.
constexpr float kListPaneWeight = 0.42f;
constexpr float kDetailPaneWeight = 0.58f;
constexpr size_t kGitCommitShortLen = 12;

std::unique_ptr<Label> MakeLabel(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

std::unique_ptr<Label> MakePill(const std::string& variant, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass("pkg-pill");
    label->AddClass(variant);
    label->SetText(text);
    return label;
}

std::unique_ptr<UIElement> MakeInfoRow(const std::string& key, const std::string& value)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("pkg-info-row");
    row->AddChild(MakeLabel("pkg-info-key", key));
    row->AddChild(MakeLabel("pkg-info-val", value));
    return row;
}

std::unique_ptr<Button> MakeActionButton(const std::string& id, const std::string& text)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->SetText(text);
    button->AddClass("small");
    button->AddClass("secondary");
    return button;
}

// "file" is the internal source kind for a local folder dependency; users read
// it as "local".
std::string KindLabel(const std::string& sourceKind)
{
    return sourceKind == "file" ? "local" : sourceKind;
}

} // namespace

PackageManagerPanel::PackageManagerPanel()
    : DockPanel("Packages")
{
    AddClass("package-manager-panel");
    // The panel carries its own scoped styles (fields, list rows, pills, detail)
    // instead of borrowing inspector CSS context that isn't present here.
    RequestSubtreeStyleAssetPath("UI/panels/PackageManagerPanel.css", "editor");
    BuildLayout();
}

void PackageManagerPanel::BuildLayout()
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("pkg-root");

    // Toolbar: global actions — add (text path / git URL), browse, refresh-all.
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("pkg-toolbar");

    auto addField = std::make_unique<TextField>();
    addField->SetId("PackageAddField");
    addField->AddClass("pkg-field");
    addField->AddClass("pkg-add-field");
    addField->SetTooltip("Package folder path or git URL (git+https://host/repo.git#ref)");
    m_AddField = addField.get();
    toolbar->AddChild(std::move(addField));

    auto addButton = MakeActionButton("PackageAddButton", "Add");
    addButton->SetTooltip("Add the package at this path or git URL (git+https://...#ref)");
    addButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_AddField)
            OnAddClicked(m_AddField->GetValue());
    });
    toolbar->AddChild(std::move(addButton));

    auto browseButton = MakeActionButton("PackageAddBrowseButton", "Browse...");
    browseButton->SetTooltip("Add a local package via the folder dialog");
    browseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!m_Actions.AddBrowse)
            return;
        RunAction(
            [this](std::string& outError) {
                std::string name;
                bool cancelled = false;
                if (!m_Actions.AddBrowse(name, outError, cancelled))
                    return false;
                if (!cancelled)
                    SetActionStatus("Added '" + name + "' — mounts on project reopen.", false);
                return true;
            },
            std::string());
    });
    toolbar->AddChild(std::move(browseButton));

    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("pkg-toolbar-spacer");
    toolbar->AddChild(std::move(spacer));

    auto refreshButton = MakeActionButton("PackageRefreshButton", "Refresh");
    refreshButton->SetTooltip(
        "Re-resolve all packages (git: refill at locked pins; local: re-scan package.json)");
    refreshButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!m_Actions.Refresh)
            return;
        RunAction([this](std::string& outError) { return m_Actions.Refresh(outError); },
                  "Re-resolved.");
    });
    toolbar->AddChild(std::move(refreshButton));
    root->AddChild(std::move(toolbar));

    // Status line: package count / reopen notice.
    auto status = std::make_unique<Label>();
    status->AddClass("pkg-status");
    status->SetText("No project open.");
    m_StatusLabel = status.get();
    root->AddChild(std::move(status));

    // Inline outcome of the last action (add/remove/pin/check/...).
    auto actionStatus = std::make_unique<Label>();
    actionStatus->SetId("PackageActionStatus");
    actionStatus->AddClass("pkg-action-status");
    actionStatus->SetText("");
    m_ActionStatusLabel = actionStatus.get();
    root->AddChild(std::move(actionStatus));

    // Resolution errors/warnings that no row owns (cycles, alias collisions).
    auto issues = std::make_unique<UIElement>();
    issues->AddClass("pkg-issues");
    m_IssuesBox = issues.get();
    root->AddChild(std::move(issues));

    // Body: master list | splitter | detail.
    auto body = std::make_unique<UIElement>();
    body->AddClass("pkg-body");

    auto listPane = std::make_unique<WeightedPane>(kListPaneWeight);
    listPane->AddClass("pkg-pane");
    {
        auto listScroll = std::make_unique<ScrollView>();
        listScroll->AddClass("pkg-list");
        m_ListContainer = listScroll->GetViewport();
        listPane->AddChild(std::move(listScroll));
    }
    body->AddChild(std::move(listPane));

    auto splitter = std::make_unique<Splitter>();
    splitter->AddClass("splitter");
    splitter->AddClass("row");
    splitter->AddClass("pkg-splitter");
    body->AddChild(std::move(splitter));

    auto detailPane = std::make_unique<WeightedPane>(kDetailPaneWeight);
    detailPane->AddClass("pkg-pane");
    {
        auto detailScroll = std::make_unique<ScrollView>();
        detailScroll->AddClass("pkg-detail");
        m_DetailContainer = detailScroll->GetViewport();
        detailPane->AddChild(std::move(detailScroll));
    }
    body->AddChild(std::move(detailPane));

    root->AddChild(std::move(body));
    AddChild(std::move(root));
}

void PackageManagerPanel::SetDataSource(SnapshotProvider provider, PackageManagerActions actions)
{
    m_Provider = std::move(provider);
    m_Actions = std::move(actions);
    RefreshList();
}

void PackageManagerPanel::SetActionStatus(const std::string& text, bool isError)
{
    m_ActionStatus = text;
    m_ActionStatusIsError = isError;
    if (isError && !text.empty())
        Logger::Log::Error("PackageManager: {}", text);
}

void PackageManagerPanel::SelectPackage(const std::string& name)
{
    if (m_SelectedPackage == name)
        return;
    m_SelectedPackage = name;
    m_ConfirmRemovePackage.clear();
    // Defer — the mouse-down handler runs inside the row about to be torn down.
    PostAction([this]() { RefreshList(); });
}

void PackageManagerPanel::RunAction(const std::function<bool(std::string& outError)>& action,
                                    const std::string& successText)
{
    std::string error;
    if (!action(error))
        SetActionStatus(error, true);
    else if (!successText.empty())
        SetActionStatus(successText, false);
    // Defer the rebuild — the click handler runs inside a row being torn down.
    PostAction([this]() { RefreshList(); });
}

void PackageManagerPanel::OnAddClicked(const std::string& input)
{
    if (input.empty())
    {
        SetActionStatus("Type a package path or a git URL (git+https://host/repo.git#ref).", true);
        PostAction([this]() { RefreshList(); });
        return;
    }
    if (!m_Actions.Add)
        return;
    RunAction(
        [this, input](std::string& outError) {
            std::string name;
            if (!m_Actions.Add(input, name, outError))
                return false;
            if (m_AddField)
                m_AddField->SetValue(std::string());
            SetActionStatus("Added '" + name + "' — mounts on project reopen.", false);
            return true;
        },
        std::string());
}

void PackageManagerPanel::RefreshList()
{
    if (!m_ListContainer)
        return;

    if (m_ActionStatusLabel)
    {
        m_ActionStatusLabel->SetText(m_ActionStatus);
        m_ActionStatusLabel->RemoveClass("error");
        if (m_ActionStatusIsError)
            m_ActionStatusLabel->AddClass("error");
    }

    while (m_IssuesBox && !m_IssuesBox->GetChildren().empty())
        m_IssuesBox->RemoveChild(m_IssuesBox->GetChildren().back().get());

    if (!m_Provider)
    {
        if (m_StatusLabel)
        {
            m_StatusLabel->SetText("No project open.");
            m_StatusLabel->RemoveClass("notice");
        }
        RebuildRows({});
        RebuildDetail({});
        return;
    }

    const PackageManagerSnapshot snapshot = m_Provider();
    const auto& entries = snapshot.Entries;

    // The reopen notice reflects real manifest-vs-mount drift: a pending
    // enable/disable, an added package not yet mounted, re-pinned/relocated
    // content, or a removed package still mounted.
    const bool anyDrift =
        snapshot.StaleMountCount > 0 ||
        std::any_of(entries.begin(), entries.end(), [](const PackageManagerEntry& e) {
            return e.MountDrift || (!e.Missing && e.Enabled != e.Mounted);
        });
    if (m_StatusLabel)
    {
        if (entries.empty())
            m_StatusLabel->SetText("No packages in Packages/manifest.json.");
        else if (anyDrift)
            m_StatusLabel->SetText(std::to_string(entries.size()) +
                                   " package(s) — changes saved; REOPEN the project to apply "
                                   "(mounts and code modules update on open).");
        else
            m_StatusLabel->SetText(std::to_string(entries.size()) + " package(s) resolved.");
        m_StatusLabel->RemoveClass("notice");
        if (anyDrift)
            m_StatusLabel->AddClass("notice");
    }

    if (m_IssuesBox)
    {
        for (const std::string& error : snapshot.Errors)
            m_IssuesBox->AddChild(MakeLabel("pkg-issue-error", error));
        for (const std::string& warning : snapshot.Warnings)
            m_IssuesBox->AddChild(MakeLabel("pkg-issue-warn", warning));
    }

    RebuildRows(entries);
    RebuildDetail(entries);
}

void PackageManagerPanel::RebuildRows(const std::vector<PackageManagerEntry>& entries)
{
    if (!m_ListContainer)
        return;
    m_ListContainer->RemoveAllChildren();
    for (const PackageManagerEntry& entry : entries)
        m_ListContainer->AddChild(std::unique_ptr<UIElement>(BuildRow(entry)));
}

UIElement* PackageManagerPanel::BuildRow(const PackageManagerEntry& entry)
{
    auto row = std::make_unique<UIElement>();
    row->SetId("pkg-row-" + entry.Name);
    row->AddClass("pkg-row");
    row->AddClass(entry.Enabled ? "enabled" : "disabled");
    if (entry.Name == m_SelectedPackage)
        row->AddClass("selected");
    if (entry.Missing)
        row->AddClass("missing");
    row->SetTooltip("Select to manage this package");

    // Line 1: name + version, then the kind pill pushed to the right.
    auto main = std::make_unique<UIElement>();
    main->AddClass("pkg-row-main");
    main->AddChild(MakeLabel("pkg-row-name", entry.Name));
    main->AddChild(MakeLabel("pkg-row-version", entry.Version));
    auto rowSpacer = std::make_unique<UIElement>();
    rowSpacer->AddClass("pkg-row-spacer");
    main->AddChild(std::move(rowSpacer));
    main->AddChild(MakePill(entry.SourceKind, KindLabel(entry.SourceKind)));
    row->AddChild(std::move(main));

    // Line 2: status badges — only present when the row has state to flag.
    const bool showDisabled = !entry.Enabled && !entry.Missing;
    const bool showDrift =
        !entry.Missing && (entry.MountDrift || (entry.Enabled != entry.Mounted));
    if (entry.Missing || showDisabled || showDrift)
    {
        auto badges = std::make_unique<UIElement>();
        badges->AddClass("pkg-row-badges");
        if (entry.Missing)
        {
            auto badge = std::make_unique<Label>();
            badge->SetId("pkg-missing-" + entry.Name);
            badge->AddClass("pkg-badge");
            badge->AddClass("missing");
            badge->SetText("MISSING");
            badges->AddChild(std::move(badge));
        }
        if (showDisabled)
        {
            auto badge = MakeLabel("pkg-badge", "DISABLED");
            badge->AddClass("disabled");
            badges->AddChild(std::move(badge));
        }
        if (showDrift)
        {
            auto badge = MakeLabel("pkg-badge", "REOPEN");
            badge->AddClass("drift");
            badges->AddChild(std::move(badge));
        }
        row->AddChild(std::move(badges));
    }

    const std::string name = entry.Name;
    row->RegisterEventHandler(kEventMouseDown, [this, name](UIEvent& e) {
        if (e.Button != 0)
            return;
        SelectPackage(name);
        e.Stop();
    });

    return row.release();
}

void PackageManagerPanel::RebuildDetail(const std::vector<PackageManagerEntry>& entries)
{
    if (!m_DetailContainer)
        return;
    m_DetailContainer->RemoveAllChildren();

    const PackageManagerEntry* selected = nullptr;
    for (const PackageManagerEntry& entry : entries)
    {
        if (entry.Name == m_SelectedPackage)
        {
            selected = &entry;
            break;
        }
    }

    if (!selected)
    {
        m_DetailContainer->AddChild(MakeLabel(
            "pkg-detail-empty", entries.empty()
                                    ? "No packages to show."
                                    : "Select a package to view its details and controls."));
        return;
    }

    const PackageManagerEntry& entry = *selected;

    auto head = std::make_unique<UIElement>();
    head->AddClass("pkg-detail-head");
    head->AddChild(MakeLabel("pkg-detail-name", entry.Name));
    head->AddChild(MakeLabel("pkg-detail-version", entry.Version));
    m_DetailContainer->AddChild(std::move(head));

    auto kindRow = std::make_unique<UIElement>();
    kindRow->AddClass("pkg-detail-kindrow");
    kindRow->AddChild(MakePill(entry.SourceKind, KindLabel(entry.SourceKind)));
    kindRow->AddChild(MakeLabel("pkg-detail-state", entry.Enabled ? "enabled" : "disabled"));
    m_DetailContainer->AddChild(std::move(kindRow));

    BuildDetailInfo(*m_DetailContainer, entry);
    BuildDetailActions(*m_DetailContainer, entry);
}

void PackageManagerPanel::BuildDetailInfo(UIElement& detail, const PackageManagerEntry& entry)
{
    if (!entry.Spec.empty())
        detail.AddChild(MakeInfoRow("spec", entry.Spec));
    if (entry.SourceKind == "git")
    {
        if (!entry.GitRef.empty())
            detail.AddChild(MakeInfoRow("ref", entry.GitRef));
        if (!entry.GitCommit.empty())
            detail.AddChild(MakeInfoRow("commit", entry.GitCommit.substr(0, kGitCommitShortLen)));
    }
    if (!entry.Missing)
    {
        detail.AddChild(MakeInfoRow("priority", std::to_string(entry.Priority)));
        detail.AddChild(MakeInfoRow("modules", std::to_string(entry.ModuleCount) + " module(s)"));
    }

    // Drift / reopen / missing notice.
    if (entry.Missing)
    {
        auto note = MakeLabel("pkg-detail-state",
                              entry.Error.empty()
                                  ? "Not found — relocate the folder or remove the dependency."
                                  : entry.Error);
        note->AddClass("error");
        detail.AddChild(std::move(note));
    }
    else if (entry.MountDrift)
    {
        auto note =
            MakeLabel("pkg-detail-state", "Re-resolved to new content — applies on project reopen.");
        note->AddClass("notice");
        detail.AddChild(std::move(note));
    }
    else if (entry.Enabled != entry.Mounted)
    {
        auto note = MakeLabel("pkg-detail-state", entry.Mounted
                                                      ? "Still mounted until project reopen."
                                                      : "Mounts on project reopen.");
        note->AddClass("notice");
        detail.AddChild(std::move(note));
    }
}

void PackageManagerPanel::BuildDetailActions(UIElement& detail, const PackageManagerEntry& entry)
{
    auto actions = std::make_unique<UIElement>();
    actions->AddClass("pkg-actions");

    const std::string name = entry.Name;

    auto toggle = MakeActionButton("pkg-toggle-" + name, entry.Enabled ? "Disable" : "Enable");
    const bool disable = entry.Enabled;
    toggle->RegisterEventHandler(kEventButtonClick, [this, name, disable](UIEvent&) {
        if (!m_Actions.Toggle)
            return;
        RunAction([this, name, disable](std::string& outError)
                  { return m_Actions.Toggle(name, disable, outError); },
                  std::string(disable ? "Disabled '" : "Enabled '") + name +
                      "' — applies on project reopen.");
    });
    actions->AddChild(std::move(toggle));

    // Engine packages are implicit (staged next to the executable): there is no
    // manifest entry to remove — disabling is the opt-out.
    if (entry.SourceKind != "engine")
    {
        const bool confirming = m_ConfirmRemovePackage == name;
        auto remove =
            MakeActionButton("pkg-remove-" + name, confirming ? "Confirm remove" : "Remove");
        if (confirming)
            remove->AddClass("confirm");
        remove->SetTooltip(confirming
                               ? "Deletes the dependency from Packages/manifest.json and its lock pin"
                               : "Click twice to remove from the project manifest");
        remove->RegisterEventHandler(kEventButtonClick, [this, name, confirming](UIEvent&) {
            if (!confirming)
            {
                m_ConfirmRemovePackage = name;
                PostAction([this]() { RefreshList(); });
                return;
            }
            m_ConfirmRemovePackage.clear();
            if (!m_Actions.Remove)
                return;
            RunAction([this, name](std::string& outError)
                      { return m_Actions.Remove(name, outError); },
                      "Removed '" + name + "' — unmounts on project reopen.");
        });
        actions->AddChild(std::move(remove));
    }

    if (entry.SourceKind == "git")
    {
        auto check = MakeActionButton("pkg-check-" + name, "Check updates");
        check->SetTooltip("Compare the locked pin against the ref's current remote commit");
        check->RegisterEventHandler(kEventButtonClick, [this, name](UIEvent&) {
            if (!m_Actions.CheckUpdates)
                return;
            RunAction(
                [this, name](std::string& outError) {
                    std::string status;
                    if (!m_Actions.CheckUpdates(name, status, outError))
                        return false;
                    SetActionStatus(status, false);
                    return true;
                },
                std::string());
        });
        actions->AddChild(std::move(check));

        const std::string currentRef = entry.GitRef;
        auto update = MakeActionButton("pkg-update-" + name, "Update to latest");
        update->SetTooltip("Drop the lock pin and re-acquire the newest commit on '" + currentRef +
                           "'");
        update->RegisterEventHandler(kEventButtonClick, [this, name, currentRef](UIEvent&) {
            if (!m_Actions.Repin)
                return;
            RunAction([this, name, currentRef](std::string& outError)
                      { return m_Actions.Repin(name, currentRef, outError); },
                      "Re-pinned '" + name + "' to latest on '" + currentRef +
                          "' — applies on project reopen.");
        });
        actions->AddChild(std::move(update));

        auto refField = std::make_unique<TextField>();
        refField->SetId("pkg-ref-" + name);
        refField->AddClass("pkg-field");
        refField->AddClass("pkg-ref-field");
        refField->SetValue(currentRef);
        refField->SetTooltip("Tag, branch, or commit sha to pin to");
        TextField* refFieldPtr = refField.get();
        actions->AddChild(std::move(refField));

        auto pin = MakeActionButton("pkg-pin-" + name, "Pin");
        pin->SetTooltip("Re-pin to this ref (manifest #ref changes; lock pin re-resolves)");
        pin->RegisterEventHandler(kEventButtonClick, [this, name, refFieldPtr](UIEvent&) {
            if (!m_Actions.Repin)
                return;
            const std::string ref = refFieldPtr ? refFieldPtr->GetValue() : std::string();
            RunAction([this, name, ref](std::string& outError)
                      { return m_Actions.Repin(name, ref, outError); },
                      "Re-pinned '" + name + "' to '" + ref + "' — applies on project reopen.");
        });
        actions->AddChild(std::move(pin));
    }

    // Locate: recover a file: package whose directory moved. Offered for missing
    // non-git rows (embedded rows relocate into file: specs too).
    if (entry.Missing && entry.SourceKind != "git" && entry.SourceKind != "engine")
    {
        auto pathField = std::make_unique<TextField>();
        pathField->SetId("pkg-path-" + name);
        pathField->AddClass("pkg-field");
        pathField->AddClass("pkg-path-field");
        pathField->SetTooltip("New folder containing this package's package.json");
        TextField* pathFieldPtr = pathField.get();
        actions->AddChild(std::move(pathField));

        auto relocate = MakeActionButton("pkg-relocate-" + name, "Set Path");
        relocate->SetTooltip("Rewrite the manifest path to this folder (name must match)");
        relocate->RegisterEventHandler(kEventButtonClick, [this, name, pathFieldPtr](UIEvent&) {
            if (!m_Actions.Relocate)
                return;
            const std::string path = pathFieldPtr ? pathFieldPtr->GetValue() : std::string();
            RunAction([this, name, path](std::string& outError)
                      { return m_Actions.Relocate(name, path, outError); },
                      "Relocated '" + name + "' — mounts on project reopen.");
        });
        actions->AddChild(std::move(relocate));

        auto locate = MakeActionButton("pkg-locate-" + name, "Locate...");
        locate->SetTooltip("Pick the package's new folder");
        locate->RegisterEventHandler(kEventButtonClick, [this, name](UIEvent&) {
            if (!m_Actions.RelocateBrowse)
                return;
            RunAction(
                [this, name](std::string& outError) {
                    bool cancelled = false;
                    if (!m_Actions.RelocateBrowse(name, outError, cancelled))
                        return false;
                    if (!cancelled)
                        SetActionStatus("Relocated '" + name + "' — mounts on project reopen.",
                                        false);
                    return true;
                },
                std::string());
        });
        actions->AddChild(std::move(locate));
    }

    detail.AddChild(std::move(actions));
}

} // namespace GameEngine
