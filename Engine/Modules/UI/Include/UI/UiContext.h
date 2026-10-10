#pragma once

#include "UI/UiDispatcher.h"
#include "Scheduler/Scheduler.h"

namespace GameEngine { namespace UI {

// Lightweight per-thread UI context that binds dispatcher and scheduler
// for the currently executing UIManager.
struct UiContext {
    IUiDispatcher* Dispatcher = nullptr;
    GameEngine::Scheduler::IScheduler* Scheduler = nullptr;
};

// Returns the thread-local UiContext pointer if set, otherwise nullptr.
UiContext* GetTlsUiContext() noexcept;

// Sets the thread-local UiContext pointer for the current thread.
void SetTlsUiContext(UiContext* ctx) noexcept;

// RAII guard to set TLS UiContext for the lifetime of the scope.
class UiContextScope {
public:
    UiContextScope(IUiDispatcher* disp, GameEngine::Scheduler::IScheduler* sched) noexcept;
    ~UiContextScope() noexcept;
private:
    UiContext m_Context{};
    UiContext* m_Prev = nullptr;
};

}} // namespace GameEngine::UI

