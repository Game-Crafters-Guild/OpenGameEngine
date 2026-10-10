#pragma once

#include "UI/Controls/SearchDialog.h"

#include <string>

namespace GameEngine::ParticleInspectors
{

/// The Add Processor picker's results: every registered processor type whose name, id or category
/// matches the query, in registration order. A result's UserData is the processor type id.
class ParticleProcessorSearchProvider final : public ISearchProvider
{
  public:
    void BeginSearch(const std::string& query, ResultSink sink) override;
    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search processors..."; }
};

} // namespace GameEngine::ParticleInspectors
