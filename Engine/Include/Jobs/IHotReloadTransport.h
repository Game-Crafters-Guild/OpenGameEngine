#pragma once
#include <string>

namespace GameEngine {

class IHotReloadTransport {
public:
    virtual ~IHotReloadTransport() = default;
    virtual bool Connect() = 0;
    virtual bool SendRequest(const std::string& requestJson, std::string& outResponse) = 0;
    virtual void Close() = 0;
};

} // namespace GameEngine

