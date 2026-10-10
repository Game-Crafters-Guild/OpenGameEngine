#pragma once

#include "ECS/Systems.h"

namespace GameEngine
{

/// Native ISystem that bridges ECS update into the managed GameSystemRunner.
/// Used in standalone (non-editor) builds to tick C# GameSystem and IEntitySystem
/// instances once per frame. In editor builds, PlayModeDriver fulfills this role.
///
/// Resolution order:
///  1. NativeAOT: look for GameEngine.Scripts.native.dll next to the executable,
///     load it via LoadLibrary/dlopen, and resolve exported symbols directly.
///  2. CoreCLR: resolve [UnmanagedCallersOnly] exports via the CoreCLR host.
class ManagedSystemBridge : public ECS::ISystem
{
public:
    ManagedSystemBridge() = default;
    ~ManagedSystemBridge() override;

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "ManagedSystemBridge"; }

private:
    using InitializeFn = int (*)(uint64_t worldHandle);
    using TickFn = int (*)(float deltaTime);
    using ShutdownFn = int (*)();

    InitializeFn m_Initialize = nullptr;
    TickFn m_Tick = nullptr;
    ShutdownFn m_Shutdown = nullptr;
    bool m_Initialized = false;
    bool m_ResolveFailed = false;
    bool m_UsingNativeAOT = false;
    bool m_LoggedFirstTick = false;
    void* m_NativeAOTHandle = nullptr;

    void EnsureResolved();
    bool TryLoadNativeAOT();
    void ResolveCoreClr();
};

} // namespace GameEngine
