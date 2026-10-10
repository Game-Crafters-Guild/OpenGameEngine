#pragma once

#include "UI/Interaction/Types.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_set>
#include <vector>

namespace GameEngine::UI::Interaction
{
struct ISelectionModel
{
    virtual ~ISelectionModel() = default;

    // Primary selection anchor used for Shift-range gestures.
    virtual ItemId GetAnchor() const = 0;
    virtual void SetAnchor(ItemId id) = 0;

    virtual bool IsSelected(ItemId id) const = 0;
    virtual std::vector<ItemId> GetSelection() const = 0; // stable order not guaranteed

    virtual void Clear() = 0;
    virtual void SetSingle(ItemId id) = 0;
    virtual void Toggle(ItemId id) = 0;

    // Range operations are computed by the view/adapter using current visible order;
    // the selection model is only asked to apply a set.
    virtual void SetSelection(const std::vector<ItemId>& ids, ItemId anchor) = 0;

    // Notifications (simple; can evolve to a signal type later).
    virtual void SetOnChanged(std::function<void()> cb) = 0;
};

class SelectionModel final : public ISelectionModel
{
  public:
    ItemId GetAnchor() const override { return m_Anchor; }
    void SetAnchor(ItemId id) override
    {
        if (m_Anchor == id)
            return;
        m_Anchor = id;
        NotifyChanged();
    }

    bool IsSelected(ItemId id) const override { return m_SelectedSet.find(id) != m_SelectedSet.end(); }

    std::vector<ItemId> GetSelection() const override
    {
        std::vector<ItemId> out;
        out.reserve(m_SelectedSet.size());
        for (ItemId id : m_SelectedSet)
            out.push_back(id);
        return out;
    }

    void Clear() override
    {
        if (m_SelectedSet.empty() && m_Anchor == 0)
            return;
        m_SelectedSet.clear();
        m_Anchor = 0;
        NotifyChanged();
    }

    void SetSingle(ItemId id) override
    {
        m_SelectedSet.clear();
        if (id != 0)
            m_SelectedSet.insert(id);
        m_Anchor = id;
        NotifyChanged();
    }

    void Toggle(ItemId id) override
    {
        if (id == 0)
            return;
        auto it = m_SelectedSet.find(id);
        if (it == m_SelectedSet.end())
        {
            m_SelectedSet.insert(id);
            m_Anchor = id;
        }
        else
        {
            m_SelectedSet.erase(it);
            if (m_Anchor == id)
                m_Anchor = 0;
        }
        NotifyChanged();
    }

    void SetSelection(const std::vector<ItemId>& ids, ItemId anchor) override
    {
        m_SelectedSet.clear();
        for (ItemId id : ids)
        {
            if (id != 0)
                m_SelectedSet.insert(id);
        }
        m_Anchor = anchor;
        NotifyChanged();
    }

    void SetOnChanged(std::function<void()> cb) override { m_OnChanged = std::move(cb); }

  private:
    void NotifyChanged()
    {
        if (m_OnChanged)
            m_OnChanged();
    }

    std::unordered_set<ItemId> m_SelectedSet;
    ItemId m_Anchor = 0;
    std::function<void()> m_OnChanged;
};
} // namespace GameEngine::UI::Interaction

