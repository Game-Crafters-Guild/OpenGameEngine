#include "Editor/Registries/BuildExporterRegistry.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

BuildExporterRegistry& BuildExporterRegistry::Get()
{
    static BuildExporterRegistry s_Instance;
    return s_Instance;
}

void BuildExporterRegistry::Register(BuildExporterDescriptor descriptor)
{
    if (descriptor.ExporterId.empty() || !descriptor.HandlesPlatform || !descriptor.Run)
    {
        Logger::Log::Error("BuildExporterRegistry: rejecting exporter registration '{}' — "
                           "ExporterId, HandlesPlatform and Run are required",
                           descriptor.ExporterId.empty() ? descriptor.DisplayName
                                                         : descriptor.ExporterId);
        return;
    }

    const auto sameId = [&](const BuildExporterDescriptor& existing)
    {
        return existing.ExporterId == descriptor.ExporterId;
    };
    const auto existing = std::find_if(m_Exporters.begin(), m_Exporters.end(), sameId);
    if (existing != m_Exporters.end())
    {
        Logger::Log::Info("BuildExporterRegistry: exporter '{}' re-registered; replacing forward",
                          descriptor.ExporterId);
        *existing = std::move(descriptor);
        return;
    }

    m_Exporters.push_back(std::move(descriptor));
}

bool BuildExporterRegistry::TryFindForPlatform(std::string_view platformName,
                                               BuildExporterDescriptor& outDescriptor) const
{
    for (const BuildExporterDescriptor& exporter : m_Exporters)
    {
        if (exporter.HandlesPlatform(platformName))
        {
            outDescriptor = exporter;
            return true;
        }
    }
    return false;
}

} // namespace GameEngine::Editor
