#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"

#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
namespace
{
// std430-friendly packed light record written into the LightBuffer.
// Keep this decoupled from shader structs for now; this is a bootstrap path.
struct GPULightPacked
{
    uint32_t type = 0;
    uint32_t castsShadows = 0;
    uint32_t areaShape = 0;
    uint32_t castsLight = 1;
    float positionWS[3] = {};
    float range = 0.0f;
    float directionWS[3] = {};
    float intensity = 0.0f;
    float color[3] = {};
    float areaWidth = 1.0f;
    float areaHeight = 1.0f;
    float areaRadius = 0.5f;
    float decay = 2.0f;
    float spotCosInner = 1.0f;
    float spotCosOuter = 1.0f;
    float fogContribution = 1.0f;
    float fogDensityBoost = 0.0f;
    float fogOriginFade = 0.2f;
    float areaRightWS[3] = {1.0f, 0.0f, 0.0f};
    float fogAnisotropy = 0.25f;
    float areaUpWS[3] = {0.0f, 1.0f, 0.0f};
    float falloffMode = 0.0f;
    // M1: point-shadow atlas slot (-1 = unshadowed) the receiver indexes into
    // ge_pointShadowSlots; the three reserved ints keep the std430 stride 16-aligned
    // and hold future spot/area slots. Mirrors the ivec4 shadowSlots lane in
    // light_packed_fields.glsl — any change here changes that file in the same commit.
    int32_t shadowSlot = -1;
    int32_t shadowSlotReserved[3] = {-1, -1, -1};
};
static_assert(sizeof(GPULightPacked) == 144, "GPULightPacked must be 144 bytes");
} // namespace

bool LightUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object())
        {
            if (j.contains("buffer") && j["buffer"].is_string())
            {
                m_BufferRef = j["buffer"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void LightUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring, EXACT-size: header +
    // count*sizeof(GPULightPacked) instead of the blueprint's full kMaxLights
    // worst case — every byte written at
    // declaration, so the old zero-fill of unused records has nothing to
    // cover. Directional/ambient stay out-of-band via LightUBO (same filter
    // as the old exec).
    const auto* viewDesc = d.Services.Views().FindViewDesc(d.View.id);
    const uint64_t worldId = viewDesc ? viewDesc->worldId : 0u;
    // Pre-sorted strongest-first by RenderServices::FinalizeWorldLights, so the
    // kMaxLights truncation below (a first-N walk) keeps the top-contribution
    // lights deterministically instead of dropping by arbitrary iteration order.
    const auto lights = d.Services.GetWorldLights(worldId);

    constexpr uint32_t kMaxLights = 1024;
    constexpr size_t kHeaderBytes = sizeof(uint32_t) * 4;

    uint32_t clusterableCount = 0;
    for (const auto& in : lights)
    {
        if (in.type == GameEngine::Components::LightType::Directional ||
            in.type == GameEngine::Components::LightType::Ambient)
            continue;
        if (clusterableCount >= kMaxLights)
            break;
        ++clusterableCount;
    }

    // At least one (zeroed) record even with no clusterable lights: WebGPU
    // validates the binding against the WGSL struct's minimum size, which
    // counts one element of the runtime light array; the header's count=0
    // keeps the shader from reading it.
    const size_t bytes =
        kHeaderBytes + size_t(std::max(clusterableCount, 1u)) * sizeof(GPULightPacked);
    const auto alloc = d.Frame.AllocUpload(bytes, 256);
    if (!alloc.Ptr)
        return;

    uint8_t* dst = static_cast<uint8_t*>(alloc.Ptr);
    std::memset(dst, 0, bytes);
    std::memcpy(dst, &clusterableCount, sizeof(uint32_t));

    // M1: per-light point-shadow atlas slot, indexed by the same packed clusterable
    // index this loop assigns (writeIdx). Computed once per (view, frame) by the
    // shadow assignment; -1 for lights the atlas budget did not admit.
    const auto camData = d.Services.Views().ResolveCameraData(d.View.id);
    const std::span<const int32_t> shadowSlotOfCluster =
        d.Services.PointShadowSlotOfCluster(d.View.id, worldId, camData);

    auto* outLights = reinterpret_cast<GPULightPacked*>(dst + kHeaderBytes);
    uint32_t writeIdx = 0;
    for (const auto& in : lights)
    {
        if (in.type == GameEngine::Components::LightType::Directional ||
            in.type == GameEngine::Components::LightType::Ambient)
            continue;
        if (writeIdx >= kMaxLights)
            break;
        GPULightPacked p{};
        p.type = static_cast<uint32_t>(in.type);
        p.castsShadows = in.castsShadows;
        p.areaShape = static_cast<uint32_t>(in.areaShape);
        p.castsLight = in.castsLight;
        const float inner = std::clamp(in.innerAngle, 0.0f, 3.1415926f);
        const float outer = std::clamp(in.outerAngle, 0.0f, 3.1415926f);
        p.spotCosInner = std::cos(std::min(inner, outer));
        p.spotCosOuter = std::cos(std::max(inner, outer));
        p.positionWS[0] = in.positionWS[0];
        p.positionWS[1] = in.positionWS[1];
        p.positionWS[2] = in.positionWS[2];
        p.range = in.range;
        p.directionWS[0] = in.directionWS[0];
        p.directionWS[1] = in.directionWS[1];
        p.directionWS[2] = in.directionWS[2];
        p.areaRightWS[0] = in.rightWS[0];
        p.areaRightWS[1] = in.rightWS[1];
        p.areaRightWS[2] = in.rightWS[2];
        p.areaUpWS[0] = in.upWS[0];
        p.areaUpWS[1] = in.upWS[1];
        p.areaUpWS[2] = in.upWS[2];
        p.intensity = in.intensity;
        p.color[0] = in.color[0];
        p.color[1] = in.color[1];
        p.color[2] = in.color[2];
        p.areaWidth = std::max(in.areaWidth, 0.001f);
        p.areaHeight = std::max(in.areaHeight, 0.001f);
        p.areaRadius = std::max(in.areaRadius, 0.001f);
        p.falloffMode = static_cast<float>(static_cast<uint32_t>(in.falloff));
        p.decay = std::max(in.decay, 0.0f);
        p.fogContribution = std::max(in.fogContribution, 0.0f);
        p.fogDensityBoost = std::max(in.fogDensityBoost, 0.0f);
        p.fogOriginFade = std::clamp(in.fogOriginFade, 0.0f, 1.0f);
        p.fogAnisotropy = std::clamp(in.fogAnisotropy, -0.95f, 0.95f);
        p.shadowSlot = writeIdx < shadowSlotOfCluster.size()
                           ? shadowSlotOfCluster[writeIdx]
                           : -1;
        outLights[writeIdx++] = p;
    }

    d.PublishBuffer(m_BufferRef, {alloc.Buffer, alloc.Offset, bytes, /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
