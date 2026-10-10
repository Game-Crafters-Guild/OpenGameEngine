#include "Graph/GraphBlendSpace2DStore.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {

namespace {

bool IsFinitePosition(float value)
{
    return std::isfinite(value);
}

} // namespace

BlendSpace2DAxis BlendSpace2DAxis::FromSamples(const std::vector<BlendSpace2DSampleDesc>& samples, bool useX)
{
    BlendSpace2DAxis axis;
    if (samples.empty())
        return axis;
    float minV = useX ? samples[0].X : samples[0].Y;
    float maxV = minV;
    for (const BlendSpace2DSampleDesc& sample : samples)
    {
        const float v = useX ? sample.X : sample.Y;
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    }
    if (minV == maxV)
    {
        axis.Min = minV - 1.f;
        axis.Max = maxV + 1.f;
        if (!(axis.Min < axis.Max))
        {
            axis.Min = 0.f;
            axis.Max = 1.f;
        }
        return axis;
    }
    axis.Min = minV;
    axis.Max = maxV;
    return axis;
}

float BlendSpace2DAxis::ClampedT(float value) const
{
    const float span = Max - Min;
    if (span == 0.f || !std::isfinite(span) || !std::isfinite(value))
        return 0.5f;
    return std::clamp((value - Min) / span, 0.f, 1.f);
}

bool GraphBlendSpace2DStore::TryLoad(const Graph::Node& host, std::vector<BlendSpace2DSampleDesc>& out)
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

    std::vector<BlendSpace2DSampleDesc> loaded;
    loaded.reserve(list->size());
    for (const Graph::GraphValue& entry : *list)
    {
        const Graph::GraphObject* sample = entry.TryObject();
        if (!sample)
            continue;
        const auto xIt = sample->find("x");
        const auto yIt = sample->find("y");
        if (xIt == sample->end() || yIt == sample->end() || !xIt->second.IsNumber() || !yIt->second.IsNumber())
            continue;
        const float x = static_cast<float>(xIt->second.AsFloat());
        const float y = static_cast<float>(yIt->second.AsFloat());
        if (!IsFinitePosition(x) || !IsFinitePosition(y))
            continue;
        BlendSpace2DSampleDesc desc;
        desc.X = x;
        desc.Y = y;
        desc.Label = sample->GetString("label");
        desc.ClipGuid = sample->GetString("clipGuid");
        loaded.push_back(std::move(desc));
    }
    out = std::move(loaded);
    return true;
}

void GraphBlendSpace2DStore::Store(Graph::Node& host, const std::vector<BlendSpace2DSampleDesc>& samples)
{
    std::vector<Graph::GraphValue> list;
    list.reserve(samples.size());
    for (const BlendSpace2DSampleDesc& sample : samples)
    {
        if (!IsFinitePosition(sample.X) || !IsFinitePosition(sample.Y))
            continue;
        Graph::GraphObject obj;
        obj["x"] = sample.X;
        obj["y"] = sample.Y;
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
