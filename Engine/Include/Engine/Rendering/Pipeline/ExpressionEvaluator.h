#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Expr
{
// Evaluate a small arithmetic expression:
// - operators: + - * /
// - parentheses
// - identifiers (looked up in vars)
// - functions: ceil(x), floor(x), min(a,b,...), max(a,b,...)
//
// Returns false and sets outError on parse/eval failure.
bool EvalExpression(const std::string& expression,
                    const std::unordered_map<std::string, double>& vars,
                    double& outValue,
                    std::string* outError = nullptr);

// Populate/resolve variables from a JSON object:
// - numeric values become variables directly
// - string values are treated as expressions and resolved against existing vars
//
// Returns false if variables cannot be resolved.
bool BuildVariables(const nlohmann::json& jsonVariables,
                    std::unordered_map<std::string, double>& inOutVars,
                    std::string* outError = nullptr);
} // namespace GameEngine::Engine::Renderer::Pipeline::Expr

