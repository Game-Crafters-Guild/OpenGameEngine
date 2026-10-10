#include "Editor/Assets/UnityImportHostedConverter.h"

#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/ScriptManager.h"

namespace GameEngine {

namespace {

constexpr size_t kStderrTailBytes = 8192;
constexpr int32 kStreamStdout = 1;
constexpr int32 kStreamStderr = 2;

// Carried through the managed write callback as the opaque user pointer. The
// converter is single-threaded, so all callbacks arrive on the invoking
// thread — no synchronization needed.
struct HostedRunContext {
    const std::function<void(const std::string&)>* OnStdoutLine = nullptr;
    const std::atomic<bool>* CancelRequested = nullptr;
    std::string StdoutAcc;
    std::string StderrTail;
};

void AppendTail(std::string& tail, const char* data, size_t count) {
    tail.append(data, count);
    if (tail.size() > kStderrTailBytes)
        tail.erase(0, tail.size() - kStderrTailBytes);
}

// Emit complete '\n'-terminated lines out of the accumulating buffer, keeping
// the trailing partial line. Strips a single trailing '\r' for robustness.
void DrainLines(std::string& acc, const std::function<void(const std::string&)>& onLine) {
    size_t start = 0;
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i] != '\n')
            continue;
        size_t end = i;
        if (end > start && acc[end - 1] == '\r')
            --end;
        onLine(acc.substr(start, end - start));
        start = i + 1;
    }
    acc.erase(0, start);
}

// Managed CallbackWriter target: returns nonzero to request cancellation.
int32 CORECLR_DELEGATE_CALLTYPE ConverterWriteThunk(void* user, int32 stream,
                                                    const char* utf8, int32 len) {
    auto* ctx = static_cast<HostedRunContext*>(user);
    if (len > 0) {
        if (stream == kStreamStdout) {
            ctx->StdoutAcc.append(utf8, static_cast<size_t>(len));
            DrainLines(ctx->StdoutAcc, *ctx->OnStdoutLine);
        } else if (stream == kStreamStderr) {
            AppendTail(ctx->StderrTail, utf8, static_cast<size_t>(len));
        }
    }
    return ctx->CancelRequested->load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace

ConverterRunResult RunConverterHosted(
    const std::filesystem::path& converterDll,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine,
    const std::atomic<bool>& cancelRequested) {
    ConverterRunResult result;

    std::error_code ec;
    if (converterDll.empty() || !std::filesystem::exists(converterDll, ec)) {
        result.StderrTail = "UnityConverter.dll is not staged next to the editor: " +
                            converterDll.string();
        return result;
    }

    auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
    if (!clr.IsInitialized()) {
        result.StderrTail =
            "The scripting runtime is not initialized; the hosted Unity converter "
            "requires the .NET runtime the editor's scripting already uses.";
        return result;
    }

    using ConverterRunFn = int32 (CORECLR_DELEGATE_CALLTYPE*)(const char* argvUtf8,
                                                              int32 argvByteLen,
                                                              void* writeFn, void* user);
    void* fnRaw = nullptr;
    const int32 resolveRc = clr.ResolveManagedUco(
        converterDll, GE_HOST_STR("GameEngine.UnityConverter.EditorHost, UnityConverter"),
        GE_HOST_STR("Run"), &fnRaw);
    if (resolveRc != 0 || fnRaw == nullptr) {
        Logger::Log::Warning("UnityImport: EditorHost.Run resolve failed (hostfxr rc=0x{:X})",
                             static_cast<uint32>(resolveRc));
        result.StderrTail = "Could not resolve the hosted converter entry point "
                            "(hostfxr rc=" + std::to_string(resolveRc) + ").";
        return result;
    }

    // '\0'-separated UTF-8 argv block; the managed decoder drops empty
    // entries, which the modal never produces.
    std::string argvBlock;
    for (const auto& a : args) {
        argvBlock += a;
        argvBlock.push_back('\0');
    }

    HostedRunContext ctx;
    ctx.OnStdoutLine = &onStdoutLine;
    ctx.CancelRequested = &cancelRequested;

    result.Spawned = true;
    result.ExitCode = reinterpret_cast<ConverterRunFn>(fnRaw)(
        argvBlock.data(), static_cast<int32>(argvBlock.size()),
        reinterpret_cast<void*>(&ConverterWriteThunk), &ctx);
    if (!ctx.StdoutAcc.empty())
        onStdoutLine(ctx.StdoutAcc); // trailing line without a newline
    result.StderrTail = std::move(ctx.StderrTail);
    return result;
}

} // namespace GameEngine
