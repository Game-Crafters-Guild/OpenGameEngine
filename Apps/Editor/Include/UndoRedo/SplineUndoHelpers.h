#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineComponent.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include "ECS/Entity.h"

#include <cstring>
#include <functional>
#include <string>

namespace GameEngine::Editor::SplineUndo
{

constexpr uint32_t kSplineSnapMagic = 0x53504C31u;

inline bool EncodeSplineConfig(const GameEngine::Spline::SplineData* src,
                               UndoRedoService::SnapshotTarget::Snapshot& out)
{
    if (!src)
        return false;
    const uint32_t count = static_cast<uint32_t>(src->Points.size());
    if (count > 500000u)
        return false;

    const size_t pointBytes =
        sizeof(GameEngine::Spline::SplineControlPoint) * static_cast<size_t>(count);
    out.resize(12u + pointBytes);

    uint8_t* p = out.data();
    std::memcpy(p, &kSplineSnapMagic, 4);
    p += 4;
    std::memcpy(p, &count, 4);
    p += 4;
    const uint8_t typeByte = static_cast<uint8_t>(src->Type);
    std::memcpy(p, &typeByte, 1);
    p += 1;
    const uint8_t closedByte = src->Closed ? uint8_t{1} : uint8_t{0};
    std::memcpy(p, &closedByte, 1);
    p += 1;

    uint16_t pad = 0;
    std::memcpy(p, &pad, 2);
    p += 2;

    if (count > 0u && !src->Points.empty())
        std::memcpy(p, src->Points.data(), pointBytes);
    return true;
}

inline bool DecodeSplineConfig(const UndoRedoService::SnapshotTarget::Snapshot& in,
                               GameEngine::Spline::SplineData& outDst)
{
    if (in.size() < 12u)
        return false;
    uint32_t magic = 0;
    std::memcpy(&magic, in.data(), 4);
    if (magic != kSplineSnapMagic)
        return false;
    uint32_t count = 0;
    std::memcpy(&count, in.data() + 4, 4);
    if (count > 500000u)
        return false;

    const size_t expected =
        12u + sizeof(GameEngine::Spline::SplineControlPoint) * static_cast<size_t>(count);
    if (in.size() != expected)
        return false;

    uint8_t typeByte = 0;
    std::memcpy(&typeByte, in.data() + 8, 1);
    uint8_t closedByte = 0;
    std::memcpy(&closedByte, in.data() + 9, 1);

    outDst = {};
    outDst.Type = static_cast<GameEngine::Spline::SplineType>(typeByte);
    outDst.Closed = closedByte != 0;
    outDst.Points.resize(count);
    if (count > 0u)
    {
        std::memcpy(outDst.Points.data(), in.data() + 12,
                    sizeof(GameEngine::Spline::SplineControlPoint) * static_cast<size_t>(count));
    }
    return true;
}

inline UndoRedoService::SnapshotTarget MakeSplineEditableSnapshotTarget(
    SplineECS::SplineService* splineService,
    SplineECS::SplineHandle splineHandle,
    ECS::World* world,
    ECS::EntityHandle entity,
    EditorChangeNotifications* notifications,
    const std::string& label)
{
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [splineService, splineHandle](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        auto* d = splineService ? splineService->GetSplineData(splineHandle) : nullptr;
        if (!d)
            return false;
        return EncodeSplineConfig(d, out);
    };

    target.Apply = [splineService, splineHandle](const UndoRedoService::SnapshotTarget::Snapshot& snap) -> bool
    {
        GameEngine::Spline::SplineData restored{};
        if (!DecodeSplineConfig(snap, restored))
            return false;
        auto* d = splineService ? splineService->GetSplineData(splineHandle) : nullptr;
        if (!d)
            return false;
        *d = std::move(restored);
        d->MarkDirty();
        splineService->RebuildCache(splineHandle);
        return true;
    };

    target.Notify = [world, entity, notifications](EditorChangeNotifications::ChangeKind kind)
    {
        if (notifications)
            notifications->NotifyComponentChange<Components::SplineComponent>(world, entity, kind);
    };

    return target;
}

/// One spline-data edit pushed as a single undo entry (inspector fields, spline tool taps, etc.).
inline void CommitSplineDataOneShot(
    SplineECS::SplineService* splineService,
    SplineECS::SplineHandle splineHandle,
    ECS::World* world,
    ECS::EntityHandle entity,
    EditorChangeNotifications* notifications,
    UndoRedoService* undo,
    const std::string& editName,
    const std::function<void(GameEngine::Spline::SplineData*)>& mutate)
{
    if (!splineService || !mutate)
        return;

    auto* data = splineService->GetSplineData(splineHandle);
    if (!data)
        return;

    if (!undo)
    {
        mutate(data);
        data->MarkDirty();
        splineService->RebuildCache(splineHandle);
        if (notifications)
        {
            notifications->NotifyComponentChange<Components::SplineComponent>(
                world, entity, EditorChangeNotifications::ChangeKind::Commit);
        }
        return;
    }

    UndoRedoService::InteractiveEdit edit = undo->BeginInteractiveEdit(
        editName,
        MakeSplineEditableSnapshotTarget(splineService, splineHandle, world, entity, notifications, editName));

    data = splineService->GetSplineData(splineHandle);
    if (!data)
        return;

    if (!edit)
    {
        mutate(data);
        data->MarkDirty();
        splineService->RebuildCache(splineHandle);
        if (notifications)
        {
            notifications->NotifyComponentChange<Components::SplineComponent>(
                world, entity, EditorChangeNotifications::ChangeKind::Commit);
        }
        return;
    }

    mutate(data);
    data->MarkDirty();
    splineService->RebuildCache(splineHandle);
    edit.Commit();
}

} // namespace GameEngine::Editor::SplineUndo
