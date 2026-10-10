#include "UI/UiContext.h"

namespace GameEngine { namespace UI {

static thread_local UiContext* g_TlsUiContext = nullptr;

UiContext* GetTlsUiContext() noexcept { return g_TlsUiContext; }
void SetTlsUiContext(UiContext* ctx) noexcept { g_TlsUiContext = ctx; }

UiContextScope::UiContextScope(IUiDispatcher* disp, GameEngine::Scheduler::IScheduler* sched) noexcept {
    m_Context.Dispatcher = disp;
    m_Context.Scheduler = sched;
    m_Prev = g_TlsUiContext;
    g_TlsUiContext = &m_Context;
}

UiContextScope::~UiContextScope() noexcept {
    g_TlsUiContext = m_Prev;
}

}} // namespace GameEngine::UI

