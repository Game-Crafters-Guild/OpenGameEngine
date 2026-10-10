#pragma once

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Editor/Entities/CopiedEntityRuntimeRefs.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
struct EntityDuplicateResult
{
    std::vector<ECS::EntityHandle> NewRoots;
};

inline Components::Name MakeDuplicateNameComponent(const std::string& text)
{
    Components::Name n{};
    std::memset(n.value, 0, sizeof(n.value));
    std::strncpy(n.value, text.c_str(), sizeof(n.value) - 1);
    return n;
}

inline std::string MakeUniqueDuplicateName(const std::string& originalName,
                                           std::unordered_set<std::string>& usedNames)
{
    std::string base = originalName;
    if (base.size() >= 4 && base.back() == ')')
    {
        const size_t open = base.rfind(" (");
        if (open != std::string::npos && open + 2 < base.size() - 1)
        {
            bool allDigits = true;
            for (size_t i = open + 2; i < base.size() - 1; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(base[i])))
                {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits)
                base = base.substr(0, open);
        }
    }

    for (int suffix = 1; suffix < 100000; ++suffix)
    {
        std::string candidate = base + " (" + std::to_string(suffix) + ")";
        if (usedNames.find(candidate) == usedNames.end())
        {
            usedNames.insert(candidate);
            return candidate;
        }
    }

    return originalName;
}

inline EntityDuplicateResult DuplicateEntitySubtreeRoots(ECS::World& world,
                                                         const std::vector<ECS::EntityHandle>& selected)
{
    EntityDuplicateResult result{};
    if (selected.empty())
        return result;

    std::unordered_set<std::uint32_t> selectedSet;
    selectedSet.reserve(selected.size());

    std::vector<ECS::EntityHandle> roots;
    roots.reserve(selected.size());
    for (ECS::EntityHandle h : selected)
    {
        if (!h.IsValid() || !world.IsValid(h))
            continue;
        selectedSet.insert(h.index);
        roots.push_back(h);
    }
    if (roots.empty())
        return result;

    std::vector<ECS::EntityHandle> topRoots;
    topRoots.reserve(roots.size());
    for (ECS::EntityHandle root : roots)
    {
        bool ancestorSelected = false;
        ECS::EntityHandle walk = root;
        for (;;)
        {
            const auto* p = world.GetComponent<Components::Parent>(walk);
            if (!p || !p->parent.IsValid() || !world.IsValid(p->parent))
                break;
            if (selectedSet.count(p->parent.index))
            {
                ancestorSelected = true;
                break;
            }
            walk = p->parent;
        }
        if (!ancestorSelected)
            topRoots.push_back(root);
    }

    // RuntimeOnlyEntity children are generator output (spline placement pieces,
    // fence posts and spans), not part of the user's entity. Cloning them would
    // hand the duplicate a stale copy that its own generator then duplicates
    // again; the clone regenerates its own from the recipe it carries.
    std::unordered_map<std::uint32_t, std::vector<ECS::EntityHandle>> childMap;
    childMap.reserve(128);
    // Duplication copies the scene as authored, disabled branches included.
    world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle e, const Components::Parent& p)
        {
            if (e.IsValid() && world.IsValid(e) && p.parent.IsValid() && world.IsValid(p.parent) &&
                !world.HasComponent<Components::RuntimeOnlyEntity>(e))
                childMap[p.parent.index].push_back(e);
        });

    std::unordered_set<std::string> usedNames;
    world.Query<ECS::Read<Components::Name>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle, const Components::Name& n)
        {
            usedNames.insert(std::string(n.View()));
        });

    result.NewRoots.reserve(topRoots.size());

    // One repair channel for the whole duplication: entities that shared a
    // runtime-owned resource keep sharing one, even across separately selected
    // roots that belong to the same model instance.
    CopiedEntityRuntimeRefs runtimeRefs;

    for (ECS::EntityHandle root : topRoots)
    {
        struct CloneEntry
        {
            ECS::EntityHandle original;
            ECS::EntityHandle newParent;
        };

        std::vector<CloneEntry> queue;
        queue.push_back({root, {}});
        ECS::EntityHandle clonedRoot{};

        size_t idx = 0;
        while (idx < queue.size())
        {
            auto [orig, newParent] = queue[idx++];

            ECS::Entity entity(&world, orig);
            ECS::Entity cloned = entity.Clone();
            ECS::EntityHandle clonedH = cloned.GetHandle();
            if (!clonedH.IsValid())
                continue;

            // Clone() copies every component, including the SceneEntityTag that holds the scene
            // "id". A duplicate must not inherit the original's id — that produces two
            // [entity id="..."] blocks on save and a scene that fails to reload. Drop it so the
            // clone is treated like a fresh entity and gets a unique id assigned on save.
            if (world.HasComponent<Components::SceneEntityTag>(clonedH))
                world.RemoveComponentImmediate<Components::SceneEntityTag>(clonedH);

            if (idx == 1)
                clonedRoot = clonedH;

            runtimeRefs.RecordClone(orig, clonedH);
            runtimeRefs.Repair(world, clonedH);

            if (newParent.IsValid())
            {
                Components::Parent parentComp{};
                parentComp.parent = newParent;
                world.AddComponentImmediate(clonedH, parentComp);
            }

            auto it = childMap.find(orig.index);
            if (it != childMap.end())
            {
                for (ECS::EntityHandle child : it->second)
                    queue.push_back({child, clonedH});
            }
        }

        if (clonedRoot.IsValid())
        {
            const auto* originalName = world.GetComponent<Components::Name>(root);
            const std::string baseName = originalName ? std::string(originalName->View()) : std::string{};
            world.AddComponentImmediate(clonedRoot, MakeDuplicateNameComponent(MakeUniqueDuplicateName(baseName, usedNames)));
            result.NewRoots.push_back(clonedRoot);
        }
    }
    runtimeRefs.RepairPayloadReferences(world);

    return result;
}
}
