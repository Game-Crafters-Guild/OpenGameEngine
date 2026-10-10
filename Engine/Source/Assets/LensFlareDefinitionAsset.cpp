#include "Assets/LensFlareDefinitionAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {

LensFlare::Color ParseColor(const nlohmann::json& j, LensFlare::Color fallback)
{
    if (!j.is_object())
        return fallback;
    LensFlare::Color c;
    c.R = j.value("r", fallback.R);
    c.G = j.value("g", fallback.G);
    c.B = j.value("b", fallback.B);
    c.A = j.value("a", fallback.A);
    return c;
}

nlohmann::json ColorToJson(const LensFlare::Color& c)
{
    return {{"r", c.R}, {"g", c.G}, {"b", c.B}, {"a", c.A}};
}

nlohmann::json CurveToJson(const std::vector<LensFlare::CurveKeyData>& keys)
{
    nlohmann::json out = nlohmann::json::array();
    for (const LensFlare::CurveKeyData& key : keys)
    {
        out.push_back({{"time", key.Time},
                       {"value", key.Value},
                       {"inTangent", key.InTangent},
                       {"outTangent", key.OutTangent}});
    }
    return out;
}

void ParseGlobals(const nlohmann::json& j, LensFlare::FlareGlobals& g)
{
    g.GlobalScale      = j.value("globalScale", g.GlobalScale);
    g.GlobalBrightness = j.value("globalBrightness", g.GlobalBrightness);
    if (j.contains("globalTint"))
        g.GlobalTint = ParseColor(j["globalTint"], g.GlobalTint);

    g.UseDistanceFade  = j.value("useDistanceFade", g.UseDistanceFade);
    g.UseDistanceScale = j.value("useDistanceScale", g.UseDistanceScale);
    g.UseMaxDistance   = j.value("useMaxDistance", g.UseMaxDistance);
    g.MaxDistance      = j.value("maxDistance", g.MaxDistance);

    g.UseAngleLimit      = j.value("useAngleLimit", g.UseAngleLimit);
    g.MaxAngle           = j.value("maxAngle", g.MaxAngle);
    g.UseAngleScale      = j.value("useAngleScale", g.UseAngleScale);
    g.UseAngleBrightness = j.value("useAngleBrightness", g.UseAngleBrightness);
    g.UseAngleCurve      = j.value("useAngleCurve", g.UseAngleCurve);

    const auto parseCurve = [](const nlohmann::json& parent, const char* name,
                               std::vector<LensFlare::CurveKeyData>& out)
    {
        auto it = parent.find(name);
        if (it == parent.end() || !it->is_array())
            return;
        out.reserve(it->size());
        for (const auto& k : *it)
        {
            LensFlare::CurveKeyData key;
            key.Time = k.value("time", 0.0f);
            key.Value = k.value("value", 0.0f);
            key.InTangent = k.value("inTangent", 0.0f);
            key.OutTangent = k.value("outTangent", 0.0f);
            out.push_back(key);
        }
    };
    parseCurve(j, "angleCurve", g.AngleCurveKeys);
    parseCurve(j, "dynamicEdgeCurve", g.DynamicEdgeCurveKeys);

    g.OffScreenFadeDist = j.value("offScreenFadeDist", g.OffScreenFadeDist);

    g.UseDynamicEdgeBoost   = j.value("useDynamicEdgeBoost", g.UseDynamicEdgeBoost);
    g.DynamicEdgeBrightness = j.value("dynamicEdgeBrightness", g.DynamicEdgeBrightness);
    g.DynamicEdgeRange      = j.value("dynamicEdgeRange", g.DynamicEdgeRange);
    g.DynamicEdgeBias       = j.value("dynamicEdgeBias", g.DynamicEdgeBias);

    g.UseDynamicEdgeScale   = j.value("useDynamicEdgeScale", g.UseDynamicEdgeScale);
    g.DynamicEdgeScale      = j.value("dynamicEdgeScale", g.DynamicEdgeScale);
    g.DynamicEdgeScaleRange = j.value("dynamicEdgeScaleRange", g.DynamicEdgeScaleRange);
    g.DynamicEdgeScaleBias  = j.value("dynamicEdgeScaleBias", g.DynamicEdgeScaleBias);

    g.UseDynamicCenterBoost   = j.value("useDynamicCenterBoost", g.UseDynamicCenterBoost);
    g.DynamicCenterBrightness = j.value("dynamicCenterBrightness", g.DynamicCenterBrightness);
    g.DynamicCenterScale      = j.value("dynamicCenterScale", g.DynamicCenterScale);
    g.DynamicCenterRange      = j.value("dynamicCenterRange", g.DynamicCenterRange);
    g.DynamicCenterBias       = j.value("dynamicCenterBias", g.DynamicCenterBias);

    g.MultiplyScaleByTransformScale =
        j.value("multiplyScaleByTransformScale", g.MultiplyScaleByTransformScale);

    g.NeverCull = j.value("neverCull", g.NeverCull);
}

LensFlare::FlareElement ParseElement(const nlohmann::json& j)
{
    LensFlare::FlareElement e;
    e.SpriteName = j.value("sprite", std::string{});
    e.Visible    = j.value("visible", e.Visible);
    e.Brightness = j.value("brightness", e.Brightness);
    e.Scale      = j.value("scale", e.Scale);
    e.SizeX      = j.value("sizeX", e.SizeX);
    e.SizeY      = j.value("sizeY", e.SizeY);
    e.Position   = j.value("position", e.Position);
    e.OffsetX    = j.value("offsetX", e.OffsetX);
    e.OffsetY    = j.value("offsetY", e.OffsetY);
    e.AnamorphicX = j.value("anamorphicX", e.AnamorphicX);
    e.AnamorphicY = j.value("anamorphicY", e.AnamorphicY);
    e.Angle           = j.value("angle", e.Angle);
    e.UseStarRotation = j.value("useStarRotation", e.UseStarRotation);
    e.RotateToFlare   = j.value("rotateToFlare", e.RotateToFlare);
    e.RotationSpeed   = j.value("rotationSpeed", e.RotationSpeed);
    if (j.contains("tint"))
        e.Tint = ParseColor(j["tint"], e.Tint);
    // Absent = negative default = inherit the flare's global boost.
    e.EdgeBrightnessBoost   = j.value("edgeBrightnessBoost", e.EdgeBrightnessBoost);
    e.CenterBrightnessBoost = j.value("centerBrightnessBoost", e.CenterBrightnessBoost);
    e.EdgeScaleBoost        = j.value("edgeScaleBoost", e.EdgeScaleBoost);
    e.CenterScaleBoost      = j.value("centerScaleBoost", e.CenterScaleBoost);
    return e;
}

} // namespace

bool LensFlareDefinitionAsset::Save() const
{
    const LensFlare::FlareGlobals& g = m_Globals;
    nlohmann::json globals{
        {"globalScale", g.GlobalScale},
        {"globalBrightness", g.GlobalBrightness},
        {"globalTint", ColorToJson(g.GlobalTint)},
        {"useDistanceFade", g.UseDistanceFade},
        {"useDistanceScale", g.UseDistanceScale},
        {"useMaxDistance", g.UseMaxDistance},
        {"maxDistance", g.MaxDistance},
        {"useAngleLimit", g.UseAngleLimit},
        {"maxAngle", g.MaxAngle},
        {"useAngleScale", g.UseAngleScale},
        {"useAngleBrightness", g.UseAngleBrightness},
        {"useAngleCurve", g.UseAngleCurve},
        {"angleCurve", CurveToJson(g.AngleCurveKeys)},
        {"offScreenFadeDist", g.OffScreenFadeDist},
        {"useDynamicEdgeBoost", g.UseDynamicEdgeBoost},
        {"dynamicEdgeBrightness", g.DynamicEdgeBrightness},
        {"dynamicEdgeRange", g.DynamicEdgeRange},
        {"dynamicEdgeBias", g.DynamicEdgeBias},
        {"dynamicEdgeCurve", CurveToJson(g.DynamicEdgeCurveKeys)},
        {"useDynamicEdgeScale", g.UseDynamicEdgeScale},
        {"dynamicEdgeScale", g.DynamicEdgeScale},
        {"dynamicEdgeScaleRange", g.DynamicEdgeScaleRange},
        {"dynamicEdgeScaleBias", g.DynamicEdgeScaleBias},
        {"useDynamicCenterBoost", g.UseDynamicCenterBoost},
        {"dynamicCenterBrightness", g.DynamicCenterBrightness},
        {"dynamicCenterScale", g.DynamicCenterScale},
        {"dynamicCenterRange", g.DynamicCenterRange},
        {"dynamicCenterBias", g.DynamicCenterBias},
        {"multiplyScaleByTransformScale", g.MultiplyScaleByTransformScale},
        {"neverCull", g.NeverCull},
    };

    nlohmann::json elements = nlohmann::json::array();
    for (const LensFlare::FlareElement& e : m_Elements)
    {
        nlohmann::json element{
            {"sprite", e.SpriteName},
            {"visible", e.Visible},
            {"brightness", e.Brightness},
            {"scale", e.Scale},
            {"sizeX", e.SizeX},
            {"sizeY", e.SizeY},
            {"position", e.Position},
            {"offsetX", e.OffsetX},
            {"offsetY", e.OffsetY},
            {"anamorphicX", e.AnamorphicX},
            {"anamorphicY", e.AnamorphicY},
            {"angle", e.Angle},
            {"useStarRotation", e.UseStarRotation},
            {"rotateToFlare", e.RotateToFlare},
            {"rotationSpeed", e.RotationSpeed},
            {"tint", ColorToJson(e.Tint)},
        };
        if (e.EdgeBrightnessBoost != LensFlare::kInheritGlobalBoost)
            element["edgeBrightnessBoost"] = e.EdgeBrightnessBoost;
        if (e.CenterBrightnessBoost != LensFlare::kInheritGlobalBoost)
            element["centerBrightnessBoost"] = e.CenterBrightnessBoost;
        if (e.EdgeScaleBoost != LensFlare::kInheritGlobalBoost)
            element["edgeScaleBoost"] = e.EdgeScaleBoost;
        if (e.CenterScaleBoost != LensFlare::kInheritGlobalBoost)
            element["centerScaleBoost"] = e.CenterScaleBoost;
        elements.push_back(std::move(element));
    }

    const nlohmann::json document{
        {"atlas", m_AtlasRef},
        {"globals", std::move(globals)},
        {"elements", std::move(elements)},
    };

    std::ofstream out(GetPath(), std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << document.dump(2) << '\n';
    return out.good();
}

bool LensFlareDefinitionAsset::Load()
{
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        SetState(AssetState::Failed);
        return false;
    }
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool LensFlareDefinitionAsset::LoadFromData(const Vector<uint8>& data)
{
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void LensFlareDefinitionAsset::Unload()
{
    m_AtlasRef.clear();
    m_Globals = {};
    m_Elements.clear();
    SetState(AssetState::Unloaded);
}

bool LensFlareDefinitionAsset::ParseFromText(const std::string& text)
{
    m_AtlasRef.clear();
    m_Globals = {};
    m_Elements.clear();

    nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return false;

    m_AtlasRef = doc.value("atlas", std::string{});

    if (doc.contains("globals") && doc["globals"].is_object())
        ParseGlobals(doc["globals"], m_Globals);

    if (doc.contains("elements") && doc["elements"].is_array())
    {
        for (const auto& e : doc["elements"])
        {
            if (e.is_object())
                m_Elements.push_back(ParseElement(e));
        }
    }

    return true;
}

} // namespace GameEngine
