#include "Graph/GraphBlendSpace1DStore.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {

namespace {

bool IsFinitePosition(float value)
{
    return std::isfinite(value);
}

} // namespace

BlendSpace1DAxis BlendSpace1DAxis::FromSamples(const std::vector<BlendSpace1DSampleDesc>& samples)
{
    BlendSpace1DAxis axis;
    if (samples.empty())
    {
        axis.Min = 0.f;
        axis.Max = 1.f;
        return axis;
    }

    if (samples.size() == 1)
    {
        axis.Min = samples[0].Position - 1.f;
        axis.Max = samples[0].Position + 1.f;
        if (!(axis.Min < axis.Max))
        {
            axis.Min = 0.f;
            axis.Max = 1.f;
        }
        return axis;
    }

    float minP = samples[0].Position;
    float maxP = samples[0].Position;
    for (const BlendSpace1DSampleDesc& sample : samples)
    {
        minP = std::min(minP, sample.Position);
        maxP = std::max(maxP, sample.Position);
    }
    if (minP == maxP)
    {
        axis.Min = minP - 1.f;
        axis.Max = maxP + 1.f;
        if (!(axis.Min < axis.Max))
        {
            axis.Min = 0.f;
            axis.Max = 1.f;
        }
        return axis;
    }

    axis.Min = minP;
    axis.Max = maxP;
    return axis;
}

float BlendSpace1DAxis::PositionFromX(float x, float trackLeft, float trackWidth) const
{
    if (trackWidth <= 0.f)
        return Min;
    const float t = (x - trackLeft) / trackWidth;
    return Min + t * (Max - Min);
}

float BlendSpace1DAxis::XFromPosition(float position, float trackLeft, float trackWidth) const
{
    if (trackWidth <= 0.f)
        return trackLeft;
    const float span = Max - Min;
    if (span == 0.f)
        return trackLeft;
    const float t = (position - Min) / span;
    return trackLeft + t * trackWidth;
}

float BlendSpace1DAxis::ClampedT(float value) const
{
    const float span = Max - Min;
    if (span == 0.f || !std::isfinite(span) || !std::isfinite(value))
        return 0.5f;
    return std::clamp((value - Min) / span, 0.f, 1.f);
}

bool GraphBlendSpace1DStore::TryLoad(const Graph::Node& host, std::vector<BlendSpace1DSampleDesc>& out)
{
    const auto it = host.Extensions.find(kExtensionKey);
    if (it == host.Extensions.end())
        return false;
    const Graph::GraphObject* bag = it->second.TryObject();
    if (!bag)
        return false;
    const auto samplesIt = bag->find("samples");
    if (samplesIt == bag->end())
        return false;
    const std::vector<Graph::GraphValue>* list = samplesIt->second.TryList();
    if (!list)
        return false;

    std::vector<BlendSpace1DSampleDesc> loaded;
    loaded.reserve(list->size());
    for (const Graph::GraphValue& entry : *list)
    {
        const Graph::GraphObject* sample = entry.TryObject();
        if (!sample)
            continue;
        const auto posIt = sample->find("position");
        if (posIt == sample->end() || !posIt->second.IsNumber())
            continue;
        const float position = static_cast<float>(posIt->second.AsFloat());
        if (!IsFinitePosition(position))
            continue;
        BlendSpace1DSampleDesc desc;
        desc.Position = position;
        desc.Label = sample->GetString("label");
        desc.ClipGuid = sample->GetString("clipGuid");
        loaded.push_back(std::move(desc));
    }
    out = std::move(loaded);
    return true;
}

void GraphBlendSpace1DStore::Store(Graph::Node& host, const std::vector<BlendSpace1DSampleDesc>& samples)
{
    std::vector<BlendSpace1DSampleDesc> sorted;
    sorted.reserve(samples.size());
    for (const BlendSpace1DSampleDesc& sample : samples)
    {
        if (!IsFinitePosition(sample.Position))
            continue;
        sorted.push_back(sample);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const BlendSpace1DSampleDesc& a, const BlendSpace1DSampleDesc& b)
                     { return a.Position < b.Position; });

    std::vector<Graph::GraphValue> list;
    list.reserve(sorted.size());
    for (const BlendSpace1DSampleDesc& sample : sorted)
    {
        Graph::GraphObject obj;
        obj["position"] = sample.Position;
        obj["label"] = sample.Label;
        if (!sample.ClipGuid.empty())
            obj["clipGuid"] = sample.ClipGuid;
        list.emplace_back(Graph::GraphValue(std::move(obj)));
    }

    Graph::GraphObject bag;
    bag["samples"] = Graph::GraphValue(std::move(list));
    host.Extensions[kExtensionKey] = Graph::GraphValue(std::move(bag));
}

} // namespace GameEngine
