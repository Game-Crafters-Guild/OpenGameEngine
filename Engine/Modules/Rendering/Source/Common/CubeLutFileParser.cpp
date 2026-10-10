#include "Rendering/Utils/CubeLutFileParser.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace GameEngine::Rendering
{
namespace
{
bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view TrimLeft(std::string_view s)
{
    while (!s.empty() && IsSpace(s.front()))
        s.remove_prefix(1);
    return s;
}

bool StartsWith(std::string_view line, std::string_view prefix)
{
    return line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0;
}

bool NearlyEqual(float32 a, float32 b)
{
    return std::fabs(a - b) <= 1e-6f;
}

bool ParseScalarDomainLine(std::string_view line,
                           std::string_view expectedKey,
                           float32& outValue,
                           std::string& outError)
{
    if (!StartsWith(line, expectedKey))
        return false;

    const std::string lineCopy(line);
    std::istringstream iss(lineCopy);
    std::string key;
    float32 r = 0.0f;
    float32 g = 0.0f;
    float32 b = 0.0f;
    if (!(iss >> key >> r >> g >> b))
    {
        outError = "Invalid DOMAIN_MIN/DOMAIN_MAX line.";
        return true;
    }

    if (!NearlyEqual(r, g) || !NearlyEqual(r, b))
    {
        outError = "Per-channel DOMAIN_MIN/DOMAIN_MAX is not supported by the current LUT shader.";
        return true;
    }

    outValue = r;
    return true;
}

bool ParseHeaderLine(std::string_view line,
                     bool& saw1dSize,
                     bool& saw3dSize,
                     uint32_t& size1d,
                     uint32_t& size3d,
                     float32& in1dMin,
                     float32& in1dMax,
                     float32& in3dMin,
                     float32& in3dMax,
                     std::string& outError)
{
    float32 domainValue = 0.0f;
    if (ParseScalarDomainLine(line, "DOMAIN_MIN", domainValue, outError))
    {
        if (!outError.empty())
            return false;
        in1dMin = domainValue;
        in3dMin = domainValue;
        return true;
    }
    if (ParseScalarDomainLine(line, "DOMAIN_MAX", domainValue, outError))
    {
        if (!outError.empty())
            return false;
        in1dMax = domainValue;
        in3dMax = domainValue;
        return true;
    }
    if (StartsWith(line, "LUT_1D_SIZE"))
    {
        const std::string lineCopy(line);
        std::istringstream iss(lineCopy);
        std::string key;
        uint32_t n = 0;
        if (!(iss >> key >> n) || n == 0)
            return false;
        size1d = n;
        saw1dSize = true;
        return true;
    }
    if (StartsWith(line, "LUT_3D_SIZE"))
    {
        const std::string lineCopy(line);
        std::istringstream iss(lineCopy);
        std::string key;
        uint32_t n = 0;
        if (!(iss >> key >> n) || n == 0)
            return false;
        size3d = n;
        saw3dSize = true;
        return true;
    }
    if (StartsWith(line, "LUT_1D_INPUT_RANGE"))
    {
        const std::string lineCopy(line);
        std::istringstream iss(lineCopy);
        std::string key;
        float a = 0.0f;
        float b = 0.0f;
        if (!(iss >> key >> a >> b))
            return false;
        in1dMin = static_cast<float32>(a);
        in1dMax = static_cast<float32>(b);
        return true;
    }
    if (StartsWith(line, "LUT_3D_INPUT_RANGE"))
    {
        const std::string lineCopy(line);
        std::istringstream iss(lineCopy);
        std::string key;
        float a = 0.0f;
        float b = 0.0f;
        if (!(iss >> key >> a >> b))
            return false;
        in3dMin = static_cast<float32>(a);
        in3dMax = static_cast<float32>(b);
        return true;
    }
    return false;
}

bool ParseRgbLine(std::string_view line, float32& r, float32& g, float32& b)
{
    std::string s(line);
    std::istringstream iss(s);
    return static_cast<bool>(iss >> r >> g >> b);
}

bool IsSkippableMetadata(std::string_view line)
{
    return StartsWith(line, "TITLE");
}
} // namespace

bool ParseCubeLutFromText(std::string_view text, CubeLutParseResult& out)
{
    out = CubeLutParseResult{};
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.remove_prefix(3);
    }

    out.In1DMin = 0.0f;
    out.In1DMax = 1.0f;
    out.In3DMin = 0.0f;
    out.In3DMax = 1.0f;

    bool saw1dSize = false;
    bool saw3dSize = false;
    uint32_t size1d = 0;
    uint32_t size3d = 0;

    enum class Phase
    {
        Preamble,
        Headers,
        Data1D,
        Data3D,
        Done
    } phase = Phase::Preamble;

    size_t pos = 0;
    const size_t n = text.size();
    auto nextLine = [&]() -> std::string_view {
        size_t start = pos;
        while (pos < n && text[pos] != '\n')
            ++pos;
        size_t end = pos;
        if (pos < n && text[pos] == '\n')
            ++pos;
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return line;
    };

    uint64_t idx1d = 0;
    uint64_t idx3d = 0;

    while (pos < n)
    {
        const std::string_view raw = nextLine();
        const std::string_view line = TrimLeft(raw);
        if (line.empty())
            continue;

        if (phase == Phase::Preamble)
        {
            if (line[0] == '#')
                continue;
            phase = Phase::Headers;
        }

        if (phase == Phase::Headers)
        {
            if (line[0] == '#')
                continue;
            if (ParseHeaderLine(line,
                                saw1dSize,
                                saw3dSize,
                                size1d,
                                size3d,
                                out.In1DMin,
                                out.In1DMax,
                                out.In3DMin,
                                out.In3DMax,
                                out.Error))
                continue;
            if (!out.Error.empty())
                return false;
            if (IsSkippableMetadata(line))
                continue;

            if (!saw3dSize && !saw1dSize)
            {
                out.Error = "Missing LUT_1D_SIZE and/or LUT_3D_SIZE.";
                return false;
            }

            out.Has1D = saw1dSize;
            out.Has3D = saw3dSize;
            out.Size1D = size1d;
            out.Size3D = size3d;

            constexpr uint32_t kMaxLut3D = 256u;
            constexpr uint32_t kMaxLut1D = 65536u;
            if (out.Has3D && (out.Size3D < 2u || out.Size3D > kMaxLut3D))
            {
                out.Error = "LUT_3D_SIZE out of supported range (2-256).";
                return false;
            }
            if (out.Has1D && (out.Size1D < 2u || out.Size1D > kMaxLut1D))
            {
                out.Error = "LUT_1D_SIZE out of supported range.";
                return false;
            }

            if (out.Has1D)
            {
                out.Lut1DRgb.resize(static_cast<size_t>(out.Size1D) * 3u);
                phase = Phase::Data1D;
            }
            else
            {
                const uint64_t cube = static_cast<uint64_t>(out.Size3D) * out.Size3D * out.Size3D;
                out.Lut3DRgb.resize(static_cast<size_t>(cube * 3u));
                phase = Phase::Data3D;
            }
        }

        if (phase == Phase::Data1D)
        {
            if (line[0] == '#')
                continue;
            float32 r = 0.0f;
            float32 g = 0.0f;
            float32 b = 0.0f;
            if (!ParseRgbLine(line, r, g, b))
            {
                out.Error = "Invalid RGB line in 1D LUT section.";
                return false;
            }
            const size_t base = static_cast<size_t>(idx1d * 3u);
            out.Lut1DRgb[base + 0] = r;
            out.Lut1DRgb[base + 1] = g;
            out.Lut1DRgb[base + 2] = b;
            ++idx1d;
            if (idx1d >= out.Size1D)
            {
                if (out.Has3D)
                {
                    const uint64_t cube = static_cast<uint64_t>(out.Size3D) * out.Size3D * out.Size3D;
                    out.Lut3DRgb.resize(static_cast<size_t>(cube * 3u));
                    phase = Phase::Data3D;
                }
                else
                {
                    phase = Phase::Done;
                }
            }
            continue;
        }

        if (phase == Phase::Data3D)
        {
            if (line[0] == '#')
                continue;
            float32 r = 0.0f;
            float32 g = 0.0f;
            float32 b = 0.0f;
            if (!ParseRgbLine(line, r, g, b))
            {
                out.Error = "Invalid RGB line in 3D LUT section.";
                return false;
            }
            const size_t base = static_cast<size_t>(idx3d * 3u);
            out.Lut3DRgb[base + 0] = r;
            out.Lut3DRgb[base + 1] = g;
            out.Lut3DRgb[base + 2] = b;
            ++idx3d;
            const uint64_t cube = static_cast<uint64_t>(out.Size3D) * out.Size3D * out.Size3D;
            if (idx3d >= cube)
                phase = Phase::Done;
            continue;
        }

        if (phase == Phase::Done)
        {
            if (line[0] == '#')
                continue;
            bool onlySpace = true;
            for (char c : line)
            {
                if (!IsSpace(c))
                {
                    onlySpace = false;
                    break;
                }
            }
            if (onlySpace)
                continue;
            out.Error = "Unexpected content after LUT data.";
            return false;
        }
    }

    if (out.Has1D && idx1d != out.Size1D)
    {
        out.Error = "Incomplete 1D LUT data.";
        return false;
    }
    if (out.Has3D)
    {
        const uint64_t cube = static_cast<uint64_t>(out.Size3D) * out.Size3D * out.Size3D;
        if (idx3d != cube)
        {
            out.Error = "Incomplete 3D LUT data.";
            return false;
        }
    }

    if (!out.Has1D && !out.Has3D)
    {
        out.Error = "No LUT data.";
        return false;
    }

    out.Ok = true;
    return true;
}

} // namespace GameEngine::Rendering
