#include "Logger/CallbackSink.h"

namespace Logger
{

CallbackSink::CallbackSink()
    : CallbackSink(Config{})
{
}

CallbackSink::CallbackSink(const Config& config)
    : m_Config(config)
{
}

void CallbackSink::Write(const LogMessage& message)
{
    if (!ShouldLog(message.Level))
    {
        return;
    }

    std::vector<LogCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(m_CallbackMutex);
        callbacksCopy.reserve(m_Callbacks.size());
        for (const auto& entry : m_Callbacks)
        {
            callbacksCopy.push_back(entry.callback);
        }
    }

    for (const auto& callback : callbacksCopy)
    {
        if (callback)
        {
            callback(message);
        }
    }
}

bool CallbackSink::ShouldLog(LogLevel level) const
{
    return level >= m_Config.MinLevel && m_Config.MinLevel != LogLevel::Off;
}

uint64 CallbackSink::RegisterCallback(LogCallback callback)
{
    std::lock_guard<std::mutex> lock(m_CallbackMutex);
    uint64 id = m_NextCallbackId++;
    m_Callbacks.push_back({id, std::move(callback)});
    return id;
}

void CallbackSink::UnregisterCallback(uint64 id)
{
    std::lock_guard<std::mutex> lock(m_CallbackMutex);
    for (auto it = m_Callbacks.begin(); it != m_Callbacks.end(); ++it)
    {
        if (it->id == id)
        {
            m_Callbacks.erase(it);
            return;
        }
    }
}

} // namespace Logger
