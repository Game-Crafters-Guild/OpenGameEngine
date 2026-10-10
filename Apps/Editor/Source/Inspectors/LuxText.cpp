#include "Inspectors/LuxText.h"

#include <cmath>
#include <cstdio>

namespace GameEngine
{
namespace
{
// U+00A0: text wraps only at ordinary spaces, so a figure stays on one line.
constexpr const char* kNoBreakSpace = "\xC2\xA0";
} // namespace

std::string FormatLux(float lux)
{
    char text[32];
    if (lux >= 100.0f)
    {
        // Three significant figures: the inspectors read these as prose, not as a meter, and more
        // digits than the model is good for read as precision it does not have.
        constexpr int kSignificantFigures = 3;
        const int magnitude = static_cast<int>(std::floor(std::log10(lux)));
        const double step = std::pow(10.0, magnitude + 1 - kSignificantFigures);
        const long long rounded = std::llround(std::round(static_cast<double>(lux) / step) * step);
        std::string digits = std::to_string(rounded);
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
            digits.insert(static_cast<size_t>(i), kNoBreakSpace);
        return digits + kNoBreakSpace + "lx";
    }
    if (lux >= 1.0f)
        std::snprintf(text, sizeof(text), "%.1f", lux);
    else
        std::snprintf(text, sizeof(text), "%.2f", lux);
    return std::string(text) + kNoBreakSpace + "lx";
}

} // namespace GameEngine
