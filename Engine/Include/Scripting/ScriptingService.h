#pragma once

#include "Scripting/ScriptingABI.h"
#include <functional>
#include <mutex>
#include <unordered_set>

namespace GameEngine {


class ScriptingService {
public:
    static ScriptingService& Get() { static ScriptingService s; return s; }

    // Optional native-side hook: invoked immediately before a domain unload is attempted.
    // Note: callback may be invoked from the thread that initiated the unload.
    void SetOnDomainWillUnload(std::function<void(GE_DomainHandle domain)> cb)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_OnDomainWillUnload = std::move(cb);
    }

    void NotifyDomainWillUnload(GE_DomainHandle domain)
    {
        std::function<void(GE_DomainHandle)> cb;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            cb = m_OnDomainWillUnload;
        }
        if (cb)
        {
            cb(domain);
        }
    }

    // The process's current runtime and editor domains (one process hosts one project)
    GE_Result SwapRuntimeDomain(GE_DomainHandle newDomain) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_RuntimeDomain = newDomain;
        return GE_Result_Ok;
    }
    GE_Result SwapEditorDomain(GE_DomainHandle newDomain) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_EditorDomain = newDomain;
        return GE_Result_Ok;
    }
    GE_Result UnloadDomain(GE_DomainHandle domain) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_RuntimeDomain == domain) m_RuntimeDomain = 0;
        if (m_EditorDomain == domain) m_EditorDomain = 0;
        m_LiveDomains.erase(domain);
        return GE_Result_Ok;
    }
    GE_Result GetCurrentRuntimeDomain(GE_DomainHandle* outDomain) {
        if (!outDomain) return GE_Result_InvalidArg;
        std::lock_guard<std::mutex> lock(m_Mutex);
        *outDomain = m_RuntimeDomain;
        return GE_Result_Ok;
    }
    GE_Result GetCurrentEditorDomain(GE_DomainHandle* outDomain) {
        if (!outDomain) return GE_Result_InvalidArg;
        std::lock_guard<std::mutex> lock(m_Mutex);
        *outDomain = m_EditorDomain;
        return GE_Result_Ok;
    }

    GE_Result MarkDomainLive(GE_DomainHandle domain) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (domain != 0) m_LiveDomains.insert(domain);
        return GE_Result_Ok;
    }

    bool IsDomainLoaded(GE_DomainHandle domain) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_LiveDomains.find(domain) != m_LiveDomains.end();
    }

    // Lifecycle helpers (call managed callbacks if registered)
    GE_Result NotifyBeforeUnload() {
        void* fn = nullptr;
        if (GE_GetManagedCallback(GE_CB_OnBeforeUnload, &fn) == GE_Result_Ok && fn) {
            using Fn = void(GE_CDECL*)(void);
            reinterpret_cast<Fn>(fn)();
        }
        return GE_Result_Ok;
    }
    GE_Result NotifyAfterLoad() {
        void* fn = nullptr;
        if (GE_GetManagedCallback(GE_CB_OnAfterLoad, &fn) == GE_Result_Ok && fn) {
            using Fn = void(GE_CDECL*)(void);
            reinterpret_cast<Fn>(fn)();
        }
        return GE_Result_Ok;
    }

private:
    std::mutex m_Mutex;
    std::function<void(GE_DomainHandle)> m_OnDomainWillUnload;
    std::unordered_set<GE_DomainHandle> m_LiveDomains; // domains considered loaded/valid
    GE_DomainHandle m_RuntimeDomain{0};
    GE_DomainHandle m_EditorDomain{0};
};

} // namespace GameEngine

