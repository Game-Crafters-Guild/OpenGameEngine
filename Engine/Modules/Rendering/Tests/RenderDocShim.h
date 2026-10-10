#pragma once

// RenderDoc capture helper.
// It first tries the official in-application API via RENDERDOC_GetAPI if the header
// is available on the include path. If not, it falls back to legacy exported
// entrypoints (which some builds omit).

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dlfcn.h>
  #include <unistd.h>
#endif

// Try to find the official header in common locations so no project-wide include path is needed.
#if __has_include(<renderdoc_app.h>)
  #define HAS_RENDERDOC_APP_HEADER 1
  #include <renderdoc_app.h>
#elif __has_include("renderdoc_app.h")
  #define HAS_RENDERDOC_APP_HEADER 1
  #include "renderdoc_app.h"
#elif __has_include("ThirdParty/renderdoc_app.h")
  #define HAS_RENDERDOC_APP_HEADER 1
  #include "ThirdParty/renderdoc_app.h"
#elif __has_include("../ThirdParty/renderdoc_app.h")
  #define HAS_RENDERDOC_APP_HEADER 1
  #include "../ThirdParty/renderdoc_app.h"
#endif

namespace TestUtils {

class RenderDocShim {
public:
    RenderDocShim()
        : m_Loaded(false), m_Mod(nullptr)
        , m_Start(nullptr), m_End(nullptr), m_Trigger(nullptr)
#if defined(HAS_RENDERDOC_APP_HEADER)
        , m_GetAPI(nullptr), m_Api(nullptr)
#endif
    {}

    // Init that prefers injected module and optionally avoids LoadLibrary
    bool Init(bool injectedOnly = false) {
#if defined(_WIN32)
        const char* explicitPath = std::getenv("GE_RENDERDOC_DLL_PATH");
        if (explicitPath && !injectedOnly) {
            m_Mod = (void*)LoadLibraryA(explicitPath);
            if (!m_Mod) std::printf("[RDOC] LoadLibrary failed for GE_RENDERDOC_DLL_PATH=%s\n", explicitPath);
        }
        if (!m_Mod) m_Mod = (void*)GetModuleHandleA("renderdoc.dll");
        if (!m_Mod && !injectedOnly) m_Mod = (void*)LoadLibraryA("renderdoc.dll");
        if (m_Mod) {
            char buf[MAX_PATH] = {0};
            if (GetModuleFileNameA((HMODULE)m_Mod, buf, MAX_PATH)) {
                std::printf("[RDOC] Using module: %s\n", buf);
            }
        }
#else
        const char* explicitPath = std::getenv("GE_RENDERDOC_DLL_PATH");
        if (explicitPath && !injectedOnly) m_Mod = dlopen(explicitPath, RTLD_NOW);
        if (!m_Mod) m_Mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (!m_Mod && !injectedOnly) m_Mod = dlopen("librenderdoc.so", RTLD_NOW);
#endif
        if (!m_Mod) return false;

#if defined(HAS_RENDERDOC_APP_HEADER)
        // Official API path
        using PFN_GetAPI = int (*)(int, void**);
    #if defined(_WIN32)
        m_GetAPI = (void*)GetProcAddress((HMODULE)m_Mod, "RENDERDOC_GetAPI");
    #else
        m_GetAPI = dlsym(m_Mod, "RENDERDOC_GetAPI");
    #endif
        if (m_GetAPI) {
            void* out = nullptr;
            PFN_GetAPI getAPI = (PFN_GetAPI)m_GetAPI;
            if (getAPI(eRENDERDOC_API_Version_1_6_0, &out) == 1 && out) {
                m_Api = (RENDERDOC_API_1_6_0*)out; // Start/End available
                m_Loaded = true;
                return true;
            }
        }
#endif

        // Fallback to legacy exported entrypoints
    #if defined(_WIN32)
        m_Start   = (PFN_Start)GetProcAddress((HMODULE)m_Mod, "RENDERDOC_StartFrameCapture");
        m_End     = (PFN_End)GetProcAddress((HMODULE)m_Mod, "RENDERDOC_EndFrameCapture");
        m_Trigger = (PFN_Trigger)GetProcAddress((HMODULE)m_Mod, "RENDERDOC_TriggerCapture");
    #else
        m_Start   = (PFN_Start)dlsym(m_Mod, "RENDERDOC_StartFrameCapture");
        m_End     = (PFN_End)dlsym(m_Mod, "RENDERDOC_EndFrameCapture");
        m_Trigger = (PFN_Trigger)dlsym(m_Mod, "RENDERDOC_TriggerCapture");
    #endif
        m_Loaded = (m_Start && m_End) || m_Trigger;
        if (!m_Loaded) std::printf("[RDOC] renderdoc.dll found but no API available (GetAPI and legacy both missing)\n");
        return m_Loaded;
    }

    bool IsAvailable() const { return m_Loaded; }

    void StartCapture(void* device = nullptr) {
#if defined(HAS_RENDERDOC_APP_HEADER)
        if (m_Api) { m_Api->StartFrameCapture(device, nullptr); std::printf("[RDOC] StartFrameCapture via GetAPI\n"); return; }
#endif
        if (!m_Loaded) { std::printf("[RDOC] StartCapture skipped; API not available\n"); return; }
        if (m_Start) {
            int ok = m_Start(device, nullptr);
            std::printf("[RDOC] StartFrameCapture(%s,NULL) -> %d\n", device?"device":"NULL", ok);
        } else if (m_Trigger) {
            std::printf("[RDOC] TriggerCapture()\n");
            m_Trigger();
        }
    }

    void EndCapture(void* device = nullptr) {
#if defined(HAS_RENDERDOC_APP_HEADER)
        if (m_Api) { m_Api->EndFrameCapture(device, nullptr); std::printf("[RDOC] EndFrameCapture via GetAPI\n"); return; }
#endif
        if (!m_Loaded) return;
        if (m_End) {
            int ok = m_End(device, nullptr);
            std::printf("[RDOC] EndFrameCapture(%s,NULL) -> %d\n", device?"device":"NULL", ok);
            if (!ok && m_Trigger) {
                std::printf("[RDOC] End failed; TriggerCapture() fallback\n");
                m_Trigger();
            }
        }
    }

private:
#if defined(_WIN32)
    using PFN_Start   = int  (__cdecl *)(void*, void*);
    using PFN_End     = int  (__cdecl *)(void*, void*);
    using PFN_Trigger = void (__cdecl *)();
#else
    using PFN_Start   = int  (*)(void*, void*);
    using PFN_End     = int  (*)(void*, void*);
    using PFN_Trigger = void (*)();
#endif

    bool m_Loaded;
    void* m_Mod;
    PFN_Start   m_Start;
    PFN_End     m_End;
    PFN_Trigger m_Trigger;
#if defined(HAS_RENDERDOC_APP_HEADER)
    void* m_GetAPI; // function pointer
    RENDERDOC_API_1_6_0* m_Api; // we request 1.6.0; older headers typedef it to same layout
#endif
};

inline bool IsEnvEnabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    return (v[0] != '\0' && v[0] != '0' && v[0] != 'f' && v[0] != 'F');
}

} // namespace TestUtils

