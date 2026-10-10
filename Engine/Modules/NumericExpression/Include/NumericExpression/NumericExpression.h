#pragma once

#include <string>

namespace GameEngine {

// Filters keystrokes for numeric expression fields (FloatField, IntField).
// Keeps digits, operators, parentheses, commas, whitespace, and letters for
// built-in functions/constants (sin, sqrt, pi, etc.).
std::string FilterNumericExpressionInput(const std::string& raw);

// Evaluates a math expression string (e.g. "2+2", "sin(pi/2)", "sqrt(16)").
// Returns false on parse error or non-finite result.
bool TryEvaluateNumericExpression(const std::string& text, double& outValue);

} // namespace GameEngine
