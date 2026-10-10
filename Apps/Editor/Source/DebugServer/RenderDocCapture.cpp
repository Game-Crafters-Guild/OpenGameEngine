#include "DebugServer/RenderDocCapture.h"

#include "Core/Application.h"
#include "Logger/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <renderdoc_app.h>
#endif

namespace GameEngine
{

#if defined(_WIN32)
static RENDERDOC_API_1_6_0* Api(void* p) { return static_cast<RENDERDOC_API_1_6_0*>(p); }

// Directory override; when unset captures land in <exeDir>/Captures.
static constexpr const char* kCaptureDirEnv = "GE_RENDERDOC_CAPTURE_DIR";
static constexpr const char* kCaptureDirName = "Captures";
// RenderDoc appends "_frameN.rdc" to the template, so this is a stem, not a file.
static constexpr const char* kCaptureFileStem = "editor";
#endif

bool RenderDocCapture::Initialize()
{
#if defined(_WIN32)
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod)
    {
        Logger::Log::Info("RenderDoc: DLL not detected (editor not launched under RenderDoc)");
        return false;
    }

    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!getApi)
    {
        Logger::Log::Warning("RenderDoc: DLL found but RENDERDOC_GetAPI export missing");
        return false;
    }

    void* apiPtr = nullptr;
    int result = getApi(eRENDERDOC_API_Version_1_6_0, &apiPtr);
    if (result != 1 || !apiPtr)
    {
        Logger::Log::Warning("RenderDoc: RENDERDOC_GetAPI failed (result={})", result);
        return false;
    }

    m_Api = apiPtr;

    int major = 0, minor = 0, patch = 0;
    Api(m_Api)->GetAPIVersion(&major, &minor, &patch);
    Logger::Log::Info("RenderDoc: Connected to in-app API v{}.{}.{}", major, minor, patch);

    ApplyCaptureFilePathTemplate();

    return true;
#else
    return false;
#endif
}

#if defined(_WIN32)
// RenderDoc's untouched default is "<temp>/RenderDoc/<exe>_<date>", so a template
// whose parent is that directory is nobody's deliberate choice and is safe to
// replace. Anything else came from a launcher (renderdoccmd --capture-file, the
// qrenderdoc launch dialog) and is left alone.
static bool IsRenderDocDefaultTemplate(const char* templatePath)
{
    if (!templatePath || templatePath[0] == '\0')
        return true;

    std::error_code ec;
    const std::filesystem::path defaultDir = std::filesystem::temp_directory_path(ec) / "RenderDoc";
    if (ec)
        return false;

    return std::filesystem::path(templatePath).parent_path() == defaultDir;
}
#endif

void RenderDocCapture::ApplyCaptureFilePathTemplate()
{
#if defined(_WIN32)
    namespace fs = std::filesystem;

    const char* envDir = std::getenv(kCaptureDirEnv);
    const bool haveEnvDir = envDir && envDir[0] != '\0';

    const char* inherited = Api(m_Api)->GetCaptureFilePathTemplate();
    if (!haveEnvDir && !IsRenderDocDefaultTemplate(inherited))
    {
        Logger::Log::Info("RenderDoc: Keeping inherited capture template '{}' (set {} to override)",
                          inherited, kCaptureDirEnv);
        return;
    }

    // Anchor to the executable directory, never the working directory: the
    // editor is launched from varying cwds and the staged tree is what ships.
    const fs::path dir =
        haveEnvDir ? fs::path(envDir) : PathUtils::GetExecutableDirectory() / kCaptureDirName;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        Logger::Log::Warning("RenderDoc: Cannot create capture directory '{}' ({}) — keeping "
                             "RenderDoc's own template; set {} to a writable directory",
                             dir.string(), ec.message(), kCaptureDirEnv);
        return;
    }

    const std::string templatePath = (dir / kCaptureFileStem).string();

    Api(m_Api)->SetCaptureFilePathTemplate(templatePath.c_str());
    Logger::Log::Info("RenderDoc: Capture path template '{}_frameN.rdc' (replacing '{}')",
                      templatePath, inherited ? inherited : "");
#endif
}

void RenderDocCapture::TriggerCapture()
{
#if defined(_WIN32)
    if (!m_Api)
        return;

    Api(m_Api)->TriggerCapture();
    Logger::Log::Info("RenderDoc: Capture triggered (will capture next presented frame)");
#endif
}

std::string RenderDocCapture::GetLastCapturePath() const
{
#if defined(_WIN32)
    if (!m_Api)
        return {};

    uint32_t count = Api(m_Api)->GetNumCaptures();
    if (count == 0)
        return {};

    uint32_t pathLen = 0;
    Api(m_Api)->GetCapture(count - 1, nullptr, &pathLen, nullptr);
    if (pathLen == 0)
        return {};

    std::string path(pathLen, '\0');
    Api(m_Api)->GetCapture(count - 1, path.data(), &pathLen, nullptr);

    if (!path.empty() && path.back() == '\0')
        path.pop_back();

    return path;
#else
    return {};
#endif
}

uint32_t RenderDocCapture::GetCaptureCount() const
{
#if defined(_WIN32)
    if (!m_Api)
        return 0;

    return Api(m_Api)->GetNumCaptures();
#else
    return 0;
#endif
}

} // namespace GameEngine
