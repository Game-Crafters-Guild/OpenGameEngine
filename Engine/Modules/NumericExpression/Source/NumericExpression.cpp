#include "NumericExpression/NumericExpression.h"

#include <cmath>
#include <cctype>

extern "C" {
#include "tinyexpr.h"
}

namespace GameEngine {
namespace
{
bool IsAllowedNumericExpressionChar(char c)
{
    if (std::isdigit(static_cast<unsigned char>(c)))
        return true;
    if (std::isalpha(static_cast<unsigned char>(c)))
        return true;
    switch (c)
    {
    case '.':
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '^':
    case '(':
    case ')':
    case ',':
    case ' ':
    case '\t':
        return true;
    default:
        return false;
    }
}
} // namespace

std::string FilterNumericExpressionInput(const std::string& raw)
{
    std::string filtered;
    filtered.reserve(raw.size());
    for (char c : raw)
    {
        if (IsAllowedNumericExpressionChar(c))
            filtered.push_back(c);
    }
    return filtered;
}

bool TryEvaluateNumericExpression(const std::string& text, double& outValue)
{
    if (text.empty())
        return false;

    int error = 0;
    const double result = te_interp(text.c_str(), &error);
    if (error != 0)
        return false;
    if (!std::isfinite(result))
        return false;

    outValue = result;
    return true;
}

} // namespace GameEngine
