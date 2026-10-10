#pragma once

#include "UI/UIStyle.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine
{

// Matches StylePropertyId::Unknown when used as "transition: all ..."
static constexpr StylePropertyId kTransitionAll = StylePropertyId::Unknown;

struct TransitionSpec
{
    static constexpr size_t kInlineCapacity = 2;

    TransitionSpec() = default;
    TransitionSpec(TransitionSpec&&) = default;
    TransitionSpec& operator=(TransitionSpec&&) = default;

    TransitionSpec(const TransitionSpec& other)
        : m_Count(other.m_Count)
    {
        for (size_t i = 0; i < kInlineCapacity; ++i)
            m_Inline[i] = other.m_Inline[i];
        if (other.m_Overflow)
            m_Overflow = std::make_unique<std::vector<TransitionEntry>>(*other.m_Overflow);
    }

    TransitionSpec& operator=(const TransitionSpec& other)
    {
        if (this != &other)
        {
            m_Count = other.m_Count;
            for (size_t i = 0; i < kInlineCapacity; ++i)
                m_Inline[i] = other.m_Inline[i];
            if (other.m_Overflow)
                m_Overflow = std::make_unique<std::vector<TransitionEntry>>(*other.m_Overflow);
            else
                m_Overflow.reset();
        }
        return *this;
    }

    bool IsEmpty() const { return m_Count == 0; }
    size_t Count() const { return m_Count; }

    const TransitionEntry* Find(StylePropertyId prop) const
    {
        for (size_t i = 0; i < m_Count; ++i)
        {
            const TransitionEntry& e = (i < kInlineCapacity) ? m_Inline[i] : (*m_Overflow)[i - kInlineCapacity];
            if (e.Property == prop || e.Property == kTransitionAll)
                return &e;
        }
        return nullptr;
    }

    void Add(const TransitionEntry& e)
    {
        if (m_Count < kInlineCapacity)
        {
            m_Inline[m_Count] = e;
        }
        else
        {
            if (!m_Overflow)
                m_Overflow = std::make_unique<std::vector<TransitionEntry>>();
            m_Overflow->push_back(e);
        }
        ++m_Count;
    }

    void Clear()
    {
        m_Count = 0;
        if (m_Overflow)
            m_Overflow->clear();
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const
    {
        for (size_t i = 0; i < m_Count; ++i)
        {
            const TransitionEntry& e = (i < kInlineCapacity) ? m_Inline[i] : (*m_Overflow)[i - kInlineCapacity];
            fn(e);
        }
    }

private:
    TransitionEntry m_Inline[kInlineCapacity]{};
    size_t m_Count = 0;
    std::unique_ptr<std::vector<TransitionEntry>> m_Overflow;
};

} // namespace GameEngine
