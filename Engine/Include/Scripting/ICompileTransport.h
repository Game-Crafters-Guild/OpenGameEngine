#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine {

// Abstract transport for talking to the resident compile server.
// Platform implementations may use named pipes (Windows), Unix domain sockets, or TCP.
class ICompileTransport {
public:
    virtual ~ICompileTransport() = default;

    // Connect to the server at the given endpoint (opaque string, transport-specific).
    // Returns true on success.
    virtual bool Connect(const std::string& endpoint) = 0;

    // Close the connection.
    virtual void Close() = 0;

    // Send a request payload and synchronously receive a response payload.
    // Returns true on success; false on transport error (response may be empty).
    virtual bool Request(const std::vector<uint8_t>& request,
                         std::vector<uint8_t>& outResponse) = 0;
};

// Compile request/response minimal envelopes (to be aligned with managed server)
struct CompileRequest {
    std::string ProjectPath;   // absolute or engine-relative path
    std::string Config;        // "Debug" / "Release"
    std::string Tfm;           // e.g., net8.0
    std::string OutDir;        // where artifacts should be written (optional)
};

struct CompileDiagnostic {
    std::string File;
    int32_t Line = 0;
    int32_t Column = 0;
    std::string Message;
    bool IsError = false;
};

struct CompileResponse {
    bool Success = false;
    std::string OutputPath; // path to produced DLL (if server writes to disk)
    std::vector<uint8_t> Assembly; // optional in-memory assembly bytes
    std::vector<uint8_t> Pdb;      // optional in-memory PDB bytes
    std::vector<CompileDiagnostic> Diagnostics;
};

// Serialize/deserialize helpers (JSON or binary TBD)
// For now, placeholder signatures.
bool Serialize(const CompileRequest& req, std::vector<uint8_t>& outBytes);
bool Deserialize(const std::vector<uint8_t>& bytes, CompileResponse& outResp);


    // Default transport factory (provided by platform or a dummy in JsonCompileTransport.cpp)
    ICompileTransport* GetDefaultCompileTransport();
} // namespace GameEngine

