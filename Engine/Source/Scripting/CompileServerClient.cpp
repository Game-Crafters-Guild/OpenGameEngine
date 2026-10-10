#include "Scripting/ICompileTransport.h"
#include "Logger/Logger.h"

namespace GameEngine {

// Very thin client wrapper over ICompileTransport.
class CompileServerClientImpl {
public:
    explicit CompileServerClientImpl(ICompileTransport* transport) : m_Transport(transport) {}

    bool Build(const CompileRequest& req, CompileResponse& outResp) {
        if (!m_Transport) return false;
        std::vector<uint8_t> bytes;
        if (!Serialize(req, bytes)) return false;
        std::vector<uint8_t> resp;
        if (!m_Transport->Request(bytes, resp)) return false;
        if (!Deserialize(resp, outResp)) return false;
        return true;
    }

private:
    ICompileTransport* m_Transport;
};

// TODO: Provide factory and platform-specific transports in follow-up changes.

} // namespace GameEngine

