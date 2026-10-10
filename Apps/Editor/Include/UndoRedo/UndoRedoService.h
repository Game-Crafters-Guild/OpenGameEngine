#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Core/CpuProfiler.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine::Editor
{
// Editor-side undo/redo stack with support for:
// - executing commands
// - compound commands (grouping)
// - interactive edits (drag/scrub) that capture before/after snapshots once
class UndoRedoService
{
  public:
    struct Config
    {
        std::size_t maxUndo = 1024;
    };

    // Use default-initialized config by default; callers may override.
    UndoRedoService() = default;
    explicit UndoRedoService(Config cfg) : m_Config(std::move(cfg)) {}

    bool CanUndo() const { return !m_Undo.empty(); }
    bool CanRedo() const { return !m_Redo.empty(); }

    std::size_t GetUndoCount() const { return m_Undo.size(); }
    std::size_t GetRedoCount() const { return m_Redo.size(); }

    // Returns the display name for the next undo/redo command (top of stack),
    // or nullptr if the stack is empty.
    const char* PeekUndoName() const
    {
        if (m_Undo.empty() || !m_Undo.back())
            return nullptr;
        return m_Undo.back()->GetName();
    }
    const char* PeekRedoName() const
    {
        if (m_Redo.empty() || !m_Redo.back())
            return nullptr;
        return m_Redo.back()->GetName();
    }

    // Optional notification hook for UI to refresh menu labels, etc.
    void SetOnHistoryChanged(std::function<void()> cb) { m_OnHistoryChanged = std::move(cb); }

    // Optional hook fired at the START of any world-mutating operation — Execute,
    // CommitAlreadyApplied, Undo, Redo, and BeginInteractiveEdit — before any
    // state is captured or applied. The editor uses it to drain an in-flight
    // deferred scene build first, so an edit / undo / redo never captures or
    // restores a half-resolved world (an unresolved MeshRenderer would otherwise
    // bake meshGpuHandleId==0 into undo history and re-hide the entity forever).
    void SetBeforeMutation(std::function<void()> cb) { m_BeforeMutation = std::move(cb); }

    // Each entry's id: assigned when the entry is first pushed, never reused, and carried
    // with the entry through Undo, Redo and DetachHistory/AttachHistory, so a caller can
    // name a step whose index every commit, undo and trim moves. 0 for an index out of
    // range. A command merged into the top entry keeps that entry's id.
    // Undo: index 0 = oldest, index (count-1) = most recent (next to be undone).
    std::uint64_t GetUndoEntryIdAt(std::size_t index) const
    {
        return index < m_UndoIds.size() ? m_UndoIds[index] : 0;
    }
    // Redo: index 0 = next to redo (most recently undone), index (count-1) = oldest redo.
    std::uint64_t GetRedoEntryIdAt(std::size_t index) const
    {
        return index < m_RedoIds.size() ? m_RedoIds[m_RedoIds.size() - 1 - index] : 0;
    }

    // An interactive edit (a drag or a scrub) is open: BeginInteractiveEdit returned it
    // and it has not committed, cancelled or been destroyed. Its "before" snapshot
    // predates anything committed meanwhile.
    bool HasOpenInteractiveEdit() const { return *m_OpenInteractiveEdits > 0; }

    // Read-only name/timestamp accessors for history panel display.
    // Undo: index 0 = oldest, index (count-1) = most recent (next to be undone).
    const char* GetUndoNameAt(std::size_t index) const
    {
        if (index >= m_Undo.size() || !m_Undo[index])
            return nullptr;
        return m_Undo[index]->GetName();
    }
    std::chrono::system_clock::time_point GetUndoTimestampAt(std::size_t index) const
    {
        if (index >= m_UndoTimestamps.size())
            return {};
        return m_UndoTimestamps[index];
    }
    const char* GetUndoTypeNameAt(std::size_t index) const
    {
        if (index >= m_Undo.size() || !m_Undo[index])
            return nullptr;
        return m_Undo[index]->GetTypeName();
    }
    // Redo: index 0 = next to redo (most recently undone), index (count-1) = oldest redo.
    const char* GetRedoNameAt(std::size_t index) const
    {
        if (index >= m_Redo.size())
            return nullptr;
        const std::size_t rIndex = m_Redo.size() - 1 - index;
        return m_Redo[rIndex] ? m_Redo[rIndex]->GetName() : nullptr;
    }
    const char* GetRedoTypeNameAt(std::size_t index) const
    {
        if (index >= m_Redo.size())
            return nullptr;
        const std::size_t rIndex = m_Redo.size() - 1 - index;
        return m_Redo[rIndex] ? m_Redo[rIndex]->GetTypeName() : nullptr;
    }

    // Capture/restore history (used for Play Mode isolation, tests, and tools).
    // Note: active compound edits are cleared when detaching.
    // undoTimestamps and undoIds run in lockstep with undo, redoIds with redo (one entry
    // per command).
    struct History
    {
        std::vector<std::unique_ptr<IEditorCommand>> undo;
        std::vector<std::chrono::system_clock::time_point> undoTimestamps;
        std::vector<std::uint64_t> undoIds;
        std::vector<std::unique_ptr<IEditorCommand>> redo;
        std::vector<std::uint64_t> redoIds;
    };

    History DetachHistory()
    {
        History h{};
        h.undo = std::move(m_Undo);
        h.undoTimestamps = std::move(m_UndoTimestamps);
        h.undoIds = std::move(m_UndoIds);
        h.redo = std::move(m_Redo);
        h.redoIds = std::move(m_RedoIds);
        m_Undo.clear();
        m_UndoTimestamps.clear();
        m_UndoIds.clear();
        m_Redo.clear();
        m_RedoIds.clear();
        m_CompoundStack.clear();
        NotifyHistoryChanged();
        return h;
    }

    void AttachHistory(History&& h)
    {
        m_Undo = std::move(h.undo);
        m_UndoTimestamps = std::move(h.undoTimestamps);
        m_UndoTimestamps.resize(m_Undo.size());
        m_UndoIds = std::move(h.undoIds);
        m_UndoIds.resize(m_Undo.size());
        m_Redo = std::move(h.redo);
        m_RedoIds = std::move(h.redoIds);
        m_RedoIds.resize(m_Redo.size());
        m_CompoundStack.clear();
        NotifyHistoryChanged();
    }

    void Clear()
    {
        m_Undo.clear();
        m_UndoTimestamps.clear();
        m_UndoIds.clear();
        ClearRedo();
        m_CompoundStack.clear();
        NotifyHistoryChanged();
    }

    void Execute(std::unique_ptr<IEditorCommand> cmd)
    {
        if (!cmd)
            return;

        FireBeforeMutation();
        cmd->Do();
        CommitInternal(std::move(cmd));
    }

    // Push a command that has already been applied to the world (typical for
    // interactive edits where Preview(...) updates state live).
    void CommitAlreadyApplied(std::unique_ptr<IEditorCommand> cmd)
    {
        if (!cmd)
            return;
        FireBeforeMutation();
        CommitInternal(std::move(cmd));
    }

    void Undo()
    {
        if (m_Undo.empty())
            return;

        FireBeforeMutation();
        auto cmd = std::move(m_Undo.back());
        m_Undo.pop_back();
        if (!m_UndoTimestamps.empty())
            m_UndoTimestamps.pop_back();
        const std::uint64_t id = m_UndoIds.back();
        m_UndoIds.pop_back();

        if (cmd)
            cmd->Undo();

        m_Redo.push_back(std::move(cmd));
        m_RedoIds.push_back(id);
        NotifyHistoryChanged();
    }

    void Redo()
    {
        if (m_Redo.empty())
            return;

        FireBeforeMutation();
        auto cmd = std::move(m_Redo.back());
        m_Redo.pop_back();
        const std::uint64_t id = m_RedoIds.back();
        m_RedoIds.pop_back();

        if (cmd)
            cmd->Redo();

        m_Undo.push_back(std::move(cmd));
        m_UndoTimestamps.push_back(std::chrono::system_clock::now());
        m_UndoIds.push_back(id);
        NotifyHistoryChanged();
    }

    // --- Compound commands -------------------------------------------------

    void BeginCompound(std::string name)
    {
        m_CompoundStack.push_back(std::make_unique<CompoundCommand>(std::move(name)));
    }

    void EndCompound()
    {
        if (m_CompoundStack.empty())
            return;

        std::unique_ptr<CompoundCommand> finished = std::move(m_CompoundStack.back());
        m_CompoundStack.pop_back();

        if (!finished || finished->Empty())
            return;

        // Nested compounds: add to parent compound rather than pushing to stack.
        if (!m_CompoundStack.empty())
        {
            m_CompoundStack.back()->Add(std::move(finished));
            return;
        }

        // Pushing a compound is a committed action; redo stack is invalidated.
        ClearRedo();
        m_Undo.push_back(std::move(finished));
        // Keep m_UndoTimestamps and m_UndoIds in lockstep with m_Undo (Execute/Redo do the
        // same); otherwise the trailing compound entries read back as epoch-0 timestamps.
        m_UndoTimestamps.push_back(std::chrono::system_clock::now());
        m_UndoIds.push_back(m_NextEntryId++);
        TrimUndo();
        NotifyHistoryChanged();
    }

    // --- Interactive snapshot edits ---------------------------------------

    struct SnapshotTarget
    {
        using Snapshot = std::vector<std::uint8_t>;

        // Optional label (debug/UI).
        std::string debugLabel;

        // Capture current state into outSnapshot.
        std::function<bool(Snapshot& outSnapshot)> Capture;

        // Apply a captured snapshot.
        std::function<bool(const Snapshot& snapshot)> Apply;

        // Optional notification hook for Preview/Commit.
        std::function<void(EditorChangeNotifications::ChangeKind kind)> Notify;
    };

    class InteractiveEdit
    {
      public:
        InteractiveEdit() = default;
        InteractiveEdit(const InteractiveEdit&) = delete;
        InteractiveEdit& operator=(const InteractiveEdit&) = delete;

        InteractiveEdit(InteractiveEdit&& other) noexcept
        {
            *this = std::move(other);
        }
        InteractiveEdit& operator=(InteractiveEdit&& other) noexcept
        {
            if (this == &other)
                return *this;

            // If we already own an active edit, cancel it to avoid leaving state
            // un-undoable.
            if (m_Active && !m_Committed && m_AutoCancelOnDestruct)
            {
                Cancel();
            }

            m_Service = other.m_Service;
            m_OpenCount = std::move(other.m_OpenCount);
            m_Name = std::move(other.m_Name);
            m_Target = std::move(other.m_Target);
            m_Before = std::move(other.m_Before);
            m_Active = other.m_Active;
            m_Committed = other.m_Committed;
            m_AutoCancelOnDestruct = other.m_AutoCancelOnDestruct;

            other.m_Service = nullptr;
            other.m_Active = false;
            other.m_Committed = false;
            return *this;
        }

        ~InteractiveEdit()
        {
            if (m_Active && !m_Committed && m_AutoCancelOnDestruct)
            {
                Cancel();
            }
        }

        explicit operator bool() const { return m_Active; }

        // Apply a live preview update (drag/scrub step). The caller performs the
        // domain-specific mutation; we only emit notifications.
        template <typename ApplyFn>
        void Preview(ApplyFn&& applyFn)
        {
            GE_CPU_PROFILE_SCOPE("UndoRedo.InteractiveEdit.Preview");
            if (!m_Active)
                return;

            {
                GE_CPU_PROFILE_SCOPE("UndoRedo.InteractiveEdit.Preview.Apply");
                applyFn();
            }

            if (m_Target.Notify)
            {
                GE_CPU_PROFILE_SCOPE("UndoRedo.InteractiveEdit.Preview.Notify");
                m_Target.Notify(EditorChangeNotifications::ChangeKind::Preview);
            }
        }

        // Commit the edit as a single undoable command. Assumes the world is
        // already in the desired final state (from Preview).
        void Commit()
        {
            if (!m_Active || m_Committed || !m_Service)
                return;

            SnapshotTarget::Snapshot after;
            if (!m_Target.Capture || !m_Target.Capture(after))
            {
                // Cannot capture; do not push an undo entry.
                Close();
                return;
            }

            if (after != m_Before)
            {
                // Command will be replayed for redo, so it must contain the apply/notify hooks.
                auto cmd = std::make_unique<SnapshotCommand>(
                    m_Name,
                    SnapshotCommand::ApplyFn{m_Target.Apply},
                    SnapshotCommand::NotifyFn{m_Target.Notify},
                    std::move(m_Before),
                    std::move(after));

                m_Service->CommitAlreadyApplied(std::move(cmd));
            }

            if (m_Target.Notify)
            {
                m_Target.Notify(EditorChangeNotifications::ChangeKind::Commit);
            }

            Close();
        }

        // Cancel the edit by restoring the captured "before" snapshot.
        void Cancel()
        {
            if (!m_Active || m_Committed)
                return;

            if (m_Target.Apply)
            {
                (void)m_Target.Apply(m_Before);
            }
            if (m_Target.Notify)
            {
                // Use Commit (not UndoRedo) so observers that already show
                // the reverted value via their own updateUI callback don't
                // trigger a redundant full rebuild.
                m_Target.Notify(EditorChangeNotifications::ChangeKind::Commit);
            }

            Close();
        }

      private:
        friend class UndoRedoService;

        InteractiveEdit(UndoRedoService* svc, std::string name, SnapshotTarget target, SnapshotTarget::Snapshot before)
            : m_Service(svc), m_OpenCount(svc->m_OpenInteractiveEdits), m_Name(std::move(name)),
              m_Target(std::move(target)), m_Before(std::move(before))
        {
            m_Active = true;
            ++*m_OpenCount;
        }

        // Ends the edit: it no longer counts toward HasOpenInteractiveEdit.
        void Close()
        {
            m_Committed = true;
            if (m_OpenCount)
                --*m_OpenCount;
            m_OpenCount.reset();
        }

        UndoRedoService* m_Service = nullptr;
        // The service's count of open edits, shared so that an edit outliving its service
        // (a panel destroyed after the editor's history) still closes safely.
        std::shared_ptr<std::size_t> m_OpenCount;
        std::string m_Name;
        SnapshotTarget m_Target{};
        SnapshotTarget::Snapshot m_Before;
        bool m_Active = false;
        bool m_Committed = false;
        bool m_AutoCancelOnDestruct = true;
    };

    InteractiveEdit BeginInteractiveEdit(std::string name, SnapshotTarget target)
    {
        // Resolve any in-flight scene build before the "before" snapshot is
        // captured, or a mid-pump edit would bake unresolved components into it.
        FireBeforeMutation();
        SnapshotTarget::Snapshot before;
        if (!target.Capture || !target.Capture(before))
        {
            return {};
        }
        return InteractiveEdit(this, std::move(name), std::move(target), std::move(before));
    }

  private:
    // --- Internal command implementations ----------------------------------

    class CompoundCommand final : public IEditorCommand
    {
      public:
        explicit CompoundCommand(std::string name) : m_Name(std::move(name)) {}

        const char* GetName() const override { return m_Name.c_str(); }
        const char* GetTypeName() const override { return "Compound"; }

        void Do() override
        {
            for (auto& c : m_Commands)
            {
                if (c)
                    c->Do();
            }
        }

        void Undo() override
        {
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
            {
                if (*it)
                    (*it)->Undo();
            }
        }

        void Redo() override
        {
            for (auto& c : m_Commands)
            {
                if (c)
                    c->Redo();
            }
        }

        void Add(std::unique_ptr<IEditorCommand> cmd)
        {
            if (cmd)
                m_Commands.push_back(std::move(cmd));
        }

        bool Empty() const { return m_Commands.empty(); }

      private:
        std::string m_Name;
        std::vector<std::unique_ptr<IEditorCommand>> m_Commands;
    };

    class SnapshotCommand final : public IEditorCommand
    {
      public:
        using Snapshot = SnapshotTarget::Snapshot;
        using ApplyFn = std::function<bool(const Snapshot&)>;
        using NotifyFn = std::function<void(EditorChangeNotifications::ChangeKind)>;

        SnapshotCommand(std::string name, ApplyFn apply, NotifyFn notify, Snapshot before, Snapshot after)
            : m_Name(std::move(name)), m_Apply(std::move(apply)), m_Notify(std::move(notify)),
              m_Before(std::move(before)), m_After(std::move(after))
        {
        }

        const char* GetName() const override { return m_Name.c_str(); }
        const char* GetTypeName() const override { return "Snapshot"; }

        void Do() override
        {
            ApplySnapshot(m_After);
        }

        void Undo() override
        {
            ApplySnapshot(m_Before);
        }

        void Redo() override
        {
            ApplySnapshot(m_After);
        }

      private:
        void ApplySnapshot(const Snapshot& s)
        {
            if (m_Apply)
            {
                (void)m_Apply(s);
            }
            if (m_Notify)
            {
                m_Notify(EditorChangeNotifications::ChangeKind::UndoRedo);
            }
        }

        std::string m_Name;
        ApplyFn m_Apply;
        NotifyFn m_Notify;
        Snapshot m_Before;
        Snapshot m_After;
    };

    void CommitInternal(std::unique_ptr<IEditorCommand> cmd)
    {
        if (!cmd)
            return;

        // If inside a compound, add there and do not touch stacks.
        if (!m_CompoundStack.empty())
        {
            m_CompoundStack.back()->Add(std::move(cmd));
            return;
        }

        // Try merge with last undo entry if supported.
        if (!m_Undo.empty() && m_Undo.back() && cmd &&
            m_Undo.back()->CanMergeWith(*cmd) &&
            m_Undo.back()->MergeWith(*cmd))
        {
            // Merged into previous command; redo history invalidated.
            ClearRedo();
            NotifyHistoryChanged();
            return;
        }

        ClearRedo();
        m_Undo.push_back(std::move(cmd));
        m_UndoTimestamps.push_back(std::chrono::system_clock::now());
        m_UndoIds.push_back(m_NextEntryId++);
        TrimUndo();
        NotifyHistoryChanged();
    }

    void TrimUndo()
    {
        const std::size_t max = (m_Config.maxUndo == 0) ? 0 : m_Config.maxUndo;
        if (max == 0)
            return;
        if (m_Undo.size() <= max)
            return;
        const std::size_t extra = m_Undo.size() - max;
        m_Undo.erase(m_Undo.begin(), m_Undo.begin() + static_cast<std::ptrdiff_t>(extra));
        if (m_UndoTimestamps.size() > max)
            m_UndoTimestamps.erase(m_UndoTimestamps.begin(),
                                   m_UndoTimestamps.begin() + static_cast<std::ptrdiff_t>(m_UndoTimestamps.size() - max));
        m_UndoIds.erase(m_UndoIds.begin(), m_UndoIds.begin() + static_cast<std::ptrdiff_t>(extra));
    }

    void ClearRedo()
    {
        m_Redo.clear();
        m_RedoIds.clear();
    }

    void NotifyHistoryChanged()
    {
        if (m_OnHistoryChanged)
        {
            m_OnHistoryChanged();
        }
    }

    void FireBeforeMutation()
    {
        if (m_BeforeMutation)
        {
            m_BeforeMutation();
        }
    }

    Config m_Config{};
    std::vector<std::unique_ptr<IEditorCommand>> m_Undo;
    std::vector<std::chrono::system_clock::time_point> m_UndoTimestamps;
    std::vector<std::uint64_t> m_UndoIds; // lockstep with m_Undo
    std::vector<std::unique_ptr<IEditorCommand>> m_Redo;
    std::vector<std::uint64_t> m_RedoIds; // lockstep with m_Redo
    std::uint64_t m_NextEntryId = 1;
    std::shared_ptr<std::size_t> m_OpenInteractiveEdits = std::make_shared<std::size_t>(0);
    std::vector<std::unique_ptr<CompoundCommand>> m_CompoundStack;
    std::function<void()> m_OnHistoryChanged;
    std::function<void()> m_BeforeMutation;
};

} // namespace GameEngine::Editor
