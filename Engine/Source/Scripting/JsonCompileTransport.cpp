#include "Scripting/ICompileTransport.h"
#include "Scripting/PathResolver.h"
#include <cstring>
#include <string>
#include <vector>

namespace GameEngine
{

// Placeholder JSON (not real JSON). Just marshals strings as UTF-8 blocks for now.
static void AppendStr(const std::string& s, std::vector<uint8_t>& out)
{
    uint32_t n = static_cast<uint32_t>(s.size());
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + sizeof(uint32_t));
    out.insert(out.end(), s.begin(), s.end());
}
static bool ReadStr(const std::vector<uint8_t>& in, size_t& off, std::string& outStr)
{
    if (off + sizeof(uint32_t) > in.size())
        return false;
    uint32_t n = 0;
    memcpy(&n, in.data() + off, sizeof(uint32_t));
    off += sizeof(uint32_t);
    if (off + n > in.size())
        return false;
    outStr.assign(reinterpret_cast<const char*>(in.data() + off), n);
    off += n;
    return true;
}

bool Serialize(const CompileRequest& req, std::vector<uint8_t>& outBytes)
{
    outBytes.clear();
    AppendStr(req.ProjectPath, outBytes);
    AppendStr(req.Config, outBytes);
    AppendStr(req.Tfm, outBytes);
    AppendStr(req.OutDir, outBytes);
    return true;
}

bool Deserialize(const std::vector<uint8_t>& bytes, CompileResponse& outResp)
{
    size_t off = 0;
    outResp = CompileResponse{};
    // Minimal: read success flag and outputPath only
    if (off + 1 > bytes.size())
        return false;
    uint8_t success = bytes[off++];
    outResp.Success = (success != 0);
    std::string outp;
    if (!ReadStr(bytes, off, outp))
        outp.clear();
    outResp.OutputPath = std::move(outp);
    return true;
}

// Dummy loopback transport for initial integration.
class DummyLoopbackTransport : public ICompileTransport
{
  public:
    bool Connect(const std::string& endpoint) override
    {
        (void)endpoint;
        return true;
    }
    void Close() override {}
    bool Request(const std::vector<uint8_t>& request, std::vector<uint8_t>& outResponse) override
    {
        // Pretend success and echo a plausible output path
        (void)request;
        outResponse.clear();
        outResponse.push_back(1); // success
        const std::string outp = "bin/Debug/" + std::string(ScriptingPaths::kScriptsAssemblyFileName);
        uint32_t n = static_cast<uint32_t>(outp.size());
        outResponse.insert(outResponse.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + sizeof(uint32_t));
        outResponse.insert(outResponse.end(), outp.begin(), outp.end());
        return true;
    }
};

// Factory to obtain a transport
static DummyLoopbackTransport g_dummy;
ICompileTransport* GetDefaultCompileTransport()
{
    return &g_dummy;
}

} // namespace GameEngine
