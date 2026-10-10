#pragma once

// Demo: query a UIDocument, register a click, drive the HP bar.
// Uses GameUI::GetHost / FindElementById (the #1051 accessor, not EngineCore::GetGameUIHost).

#include "GameSDK/System.h"

#include "Components/UI/UIDocument.h"
#include "Engine/GameUI/GameplayUI.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <memory>
#include <string>

namespace Demo
{

struct HpBarSystem : GameEngine::ECS::SystemBase
{
    uint64_t m_EntityId = 0;
    // Identity of the button m_Token was minted on, not a bool "wired" latch. An
    // EventHandlerToken is an element-local key with no element identity in it, so it is only
    // meaningful on the element that issued it: replay one on the button a .uxml reload put in
    // the old one's place and it unregisters whichever handler happens to hold that key there.
    // Instance ids are never reused, so comparing them is what tells "still wired" apart from
    // "wired to an element that is gone".
    uint64_t m_ButtonInstanceId = 0;
    std::shared_ptr<int> m_Hp = std::make_shared<int>(100);
    GameEngine::UIElement::EventHandlerToken m_Token{};

    static constexpr int kLowHealthPercent = 50;

    static void ApplyHp(uint64_t entityId, int hp)
    {
        using namespace GameEngine;
        if (UIElement* fill = GameUI::FindElementById(entityId, "hp-fill"))
        {
            fill->Overrides().Set(Style::Width, StyleLength::Percent(static_cast<float>(hp)));
            // State goes on as a CSS class; the stylesheet owns what it looks like.
            if (hp <= kLowHealthPercent)
                fill->AddClass("low-health");
            else
                fill->RemoveClass("low-health");
        }
        if (auto* label = dynamic_cast<Label*>(GameUI::FindElementById(entityId, "hp-label")))
            label->SetText(std::to_string(hp) + " / 100");
    }

    void Unwire()
    {
        using namespace GameEngine;
        if (m_ButtonInstanceId != 0)
        {
            UIElement* button = GameUI::FindElementById(m_EntityId, "damage-btn");
            // The id can resolve to a REPLACEMENT button, which never saw this token.
            if (button && button->GetInstanceId() == m_ButtonInstanceId)
                button->UnregisterEventHandler(m_Token);
        }
        m_Token = {};
        m_ButtonInstanceId = 0;
        m_EntityId = 0;
    }

    void Reset()
    {
        *m_Hp = 100;
        if (m_EntityId != 0)
            ApplyHp(m_EntityId, *m_Hp);
    }

    void OnStart(GameEngine::ECS::World&) { Reset(); }

    void OnDestroy(GameEngine::ECS::World&)
    {
        // Reset first: Unwire zeroes m_EntityId, and Reset only pushes to the UI
        // while it still knows the entity.
        Reset();
        Unwire();
    }

    void OnUpdate(GameEngine::ECS::World& world, float)
    {
        using namespace GameEngine;
        if (!GameUI::GetHost())
            return;

        // Ask every tick whether the button we subscribed to is still THAT button, instead of
        // latching once. ApplyHp re-resolves its elements on every call, so the bar and label
        // keep working after a .uxml reload replaced the subtree — which is exactly what makes
        // a subscription left on the destroyed button invisible until someone clicks.
        if (m_ButtonInstanceId != 0)
        {
            UIElement* wired = GameUI::FindElementById(m_EntityId, "damage-btn");
            if (wired && wired->GetInstanceId() == m_ButtonInstanceId)
                return;
            Unwire();
        }

        world.Query<ECS::Read<Components::UIDocument>>().Each(
            [&](ECS::EntityHandle e, const Components::UIDocument&) {
            if (m_ButtonInstanceId != 0)
                return;
            UIElement* button = GameUI::FindElementById(e.id, "damage-btn");
            if (!button)
                return;
            m_EntityId = e.id;
            ApplyHp(m_EntityId, *m_Hp);
            auto hp = m_Hp;
            const uint64_t entityId = m_EntityId;
            // kEventButtonClick, not kEventMouseUp: the button dispatches this only when the press
            // began on it and the release is still inside, so a release that drifted onto the
            // button is correctly not a click.
            m_Token = button->RegisterEventHandler(kEventButtonClick, [hp, entityId](UIEvent&) {
                *hp = std::max(0, *hp - 10);
                ApplyHp(entityId, *hp);
            });
            m_ButtonInstanceId = button->GetInstanceId();
        });
    }
};

} // namespace Demo
