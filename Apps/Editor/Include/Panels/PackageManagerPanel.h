#pragma once

#include "UI/Controls/DockPanel.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

class Label;
class TextField;
class UIElement;

// One row of the Package Manager list — a display snapshot the provider
// builds from the current PackageResolver::Resolve output merged with the
// CURRENT Packages/manifest.json (pending edits show immediately) and the
// project-open mount state (so drift from live mounts is explicit).
struct PackageManagerEntry
{
    std::string Name;
    std::string Version;
    std::string SourceKind; // "embedded" / "file" / "git" / "engine"
    std::string Spec;       // raw manifest spec text ("" for implicit engine packages)
    std::string GitCommit;  // full resolved sha, git packages only
    std::string GitRef;     // requested ref from the manifest spec, git packages only
    bool Enabled = false;   // manifest state (not the session mount state)
    bool Mounted = false;   // session mount state (mounts update on project open)
    // Declared in the manifest but failed to resolve (dead file: path, bad
    // spec, cold git cache with no network) — shows the MISSING badge and the
    // Locate / re-pin recovery actions.
    bool Missing = false;
    // Mounted, but the current resolution points at different content (re-pin
    // changed the commit, relocate changed the dir) — applies on reopen.
    bool MountDrift = false;
    std::string Error; // resolver error owned by this row (Missing rows)
    int32_t Priority = 0;
    int ModuleCount = 0;
};

// Full panel snapshot: rows plus the resolution errors/warnings that are not
// owned by a specific row (cycles, alias collisions, lock write failures).
struct PackageManagerSnapshot
{
    std::vector<PackageManagerEntry> Entries;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
    // Mounted aliases whose package left the resolution entirely (removed
    // dependencies) — still mounted until reopen.
    int StaleMountCount = 0;
};

// The panel's callbacks into EditorApplication. Every action edits the
// project manifest / lock only and re-resolves for display — live mounts
// never change mid-session (the P3 drift model); the panel surfaces the
// reopen notice instead of pretending to hot-apply. All return false with
// outError set on failure; the panel shows the error inline.
struct PackageManagerActions
{
    // Enable/disable via the manifest disabled[] array.
    std::function<bool(const std::string& name, bool disable, std::string& outError)> Toggle;
    // Add from free text: a filesystem path OR a git spec ("git+https://...#ref").
    // outName receives the package name learned from its package.json.
    std::function<bool(const std::string& input, std::string& outName, std::string& outError)> Add;
    // Add a local package via the native folder dialog (modal; blocks).
    // outCancelled distinguishes user-cancel from failure.
    std::function<bool(std::string& outName, std::string& outError, bool& outCancelled)> AddBrowse;
    // Delete the dependency from manifest + lock.
    std::function<bool(const std::string& name, std::string& outError)> Remove;
    // Re-run the resolver over the whole set (git: refill at locked pins;
    // file/embedded: re-scan package.json edits).
    std::function<bool(std::string& outError)> Refresh;
    // Read-only ls-remote compare of the locked pin vs the ref's current head.
    std::function<bool(const std::string& name, std::string& outStatus, std::string& outError)>
        CheckUpdates;
    // Re-pin a git dependency to a ref (tag/branch/sha). Passing its current
    // ref means "update to latest on that ref" (the pin is dropped either way).
    std::function<bool(const std::string& name, const std::string& ref, std::string& outError)> Repin;
    // Rewrite a file: dependency's path (text form of Locate).
    std::function<bool(const std::string& name, const std::string& path, std::string& outError)>
        Relocate;
    // Locate via the native folder dialog.
    std::function<bool(const std::string& name, std::string& outError, bool& outCancelled)>
        RelocateBrowse;
};

// Package Manager panel: a toolbar of global actions (add local/git by text or
// browse, refresh-all) over a master-detail body — a left list of resolved
// packages (name, version, kind pill, status badges; no controls) and a right
// detail view of the selected package (spec/ref/pin info + its action cluster:
// enable/disable, remove, git re-pin/update/check, relocate missing file:
// packages). All actions are manifest/lock edits; mounts and code modules
// update on the next project open, which the panel says explicitly.
class PackageManagerPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "box-icon"; }

    PackageManagerPanel();

    using SnapshotProvider = std::function<PackageManagerSnapshot()>;

    // Called by EditorApplication during panel wiring (both outlive the panel).
    void SetDataSource(SnapshotProvider provider, PackageManagerActions actions);
    void RefreshList();

  private:
    void BuildLayout();
    void RebuildRows(const std::vector<PackageManagerEntry>& entries);
    UIElement* BuildRow(const PackageManagerEntry& entry);
    // Rebuild the right pane from the selected entry (empty-state when nothing
    // selected or the selection left the resolution).
    void RebuildDetail(const std::vector<PackageManagerEntry>& entries);
    void BuildDetailInfo(UIElement& detail, const PackageManagerEntry& entry);
    void BuildDetailActions(UIElement& detail, const PackageManagerEntry& entry);
    // Select a package (drives the detail pane); clears any pending remove
    // confirm and rebuilds on the next frame.
    void SelectPackage(const std::string& name);
    // Run `action`, put its outcome on the action-status line, then rebuild
    // the list on the next frame (handlers run inside rows being torn down).
    void RunAction(const std::function<bool(std::string& outError)>& action,
                   const std::string& successText);
    void SetActionStatus(const std::string& text, bool isError);
    void OnAddClicked(const std::string& input);

    SnapshotProvider m_Provider;
    PackageManagerActions m_Actions;

    Label* m_StatusLabel = nullptr;
    Label* m_ActionStatusLabel = nullptr;
    TextField* m_AddField = nullptr;
    UIElement* m_IssuesBox = nullptr;
    UIElement* m_ListContainer = nullptr;   // ScrollView viewport for list rows
    UIElement* m_DetailContainer = nullptr; // ScrollView viewport for detail

    // Selected package shown in the detail pane; empty = none.
    std::string m_SelectedPackage;
    // Two-click remove guard: the package whose Remove is awaiting confirm.
    std::string m_ConfirmRemovePackage;
    std::string m_ActionStatus;
    bool m_ActionStatusIsError = false;
};

} // namespace GameEngine
