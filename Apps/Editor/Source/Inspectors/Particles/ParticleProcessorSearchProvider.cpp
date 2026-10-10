#include "Inspectors/Particles/ParticleProcessorSearchProvider.h"

#include "Particles/ParticleProcessorRegistry.h"
#include "UI/PickerQueryFilter.h"

#include <utility>
#include <vector>

namespace GameEngine::ParticleInspectors
{

void ParticleProcessorSearchProvider::BeginSearch(const std::string& query, ResultSink sink)
{
    const auto filter = PickerQueryFilter::Parse(query);
    std::vector<SearchResultItem> results;
    Particles::ParticleProcessorRegistry::ForEach(
        [&filter, &results](const Particles::ParticleProcessorDescriptor& descriptor)
        {
            const std::string label(descriptor.DisplayName);
            const std::string keywords = std::string(descriptor.Id) + " " + std::string(descriptor.Category);
            if (!filter.Matches(label, keywords))
                return;
            SearchResultItem item;
            item.Id = results.size() + 1;
            item.Label = label;
            item.Detail = std::string(descriptor.Category);
            item.Icon = SearchIcon::FromClass(std::string(descriptor.IconClass));
            item.UserData = std::string(descriptor.Id);
            results.push_back(std::move(item));
        });
    sink(std::move(results), true);
}

} // namespace GameEngine::ParticleInspectors
