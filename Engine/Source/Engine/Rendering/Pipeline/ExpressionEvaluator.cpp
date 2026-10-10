#include "Engine/Rendering/Pipeline/ExpressionEvaluator.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Expr
{
namespace
{
struct EvalResult
{
    bool ok = false;
    double value = 0.0;
    std::string error;
};

class Parser
{
  public:
    Parser(std::string_view s, const std::unordered_map<std::string, double>& vars)
        : m_S(s), m_Vars(vars)
    {
    }

    EvalResult Eval()
    {
        EvalResult r{};
        SkipWs();
        const double v = ParseExpression(r);
        SkipWs();
        if (!r.ok)
            return r;
        if (m_Pos != m_S.size())
        {
            r.ok = false;
            r.error = "unexpected trailing characters";
            return r;
        }
        r.ok = true;
        r.value = v;
        return r;
    }

  private:
    void SkipWs()
    {
        while (m_Pos < m_S.size())
        {
            const char c = m_S[m_Pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++m_Pos;
            else
                break;
        }
    }

    bool Match(char c)
    {
        SkipWs();
        if (m_Pos < m_S.size() && m_S[m_Pos] == c)
        {
            ++m_Pos;
            return true;
        }
        return false;
    }

    double ParseExpression(EvalResult& r)
    {
        r.ok = true;
        double lhs = ParseTerm(r);
        if (!r.ok)
            return 0.0;
        for (;;)
        {
            SkipWs();
            if (Match('+'))
            {
                const double rhs = ParseTerm(r);
                if (!r.ok)
                    return 0.0;
                lhs += rhs;
            }
            else if (Match('-'))
            {
                const double rhs = ParseTerm(r);
                if (!r.ok)
                    return 0.0;
                lhs -= rhs;
            }
            else
            {
                break;
            }
        }
        return lhs;
    }

    double ParseTerm(EvalResult& r)
    {
        double lhs = ParseFactor(r);
        if (!r.ok)
            return 0.0;
        for (;;)
        {
            SkipWs();
            if (Match('*'))
            {
                const double rhs = ParseFactor(r);
                if (!r.ok)
                    return 0.0;
                lhs *= rhs;
            }
            else if (Match('/'))
            {
                const double rhs = ParseFactor(r);
                if (!r.ok)
                    return 0.0;
                if (rhs == 0.0)
                {
                    r.ok = false;
                    r.error = "division by zero";
                    return 0.0;
                }
                lhs /= rhs;
            }
            else
            {
                break;
            }
        }
        return lhs;
    }

    static bool IsIdentStart(char c)
    {
        return (c == '_') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    static bool IsIdentChar(char c)
    {
        return IsIdentStart(c) || (c >= '0' && c <= '9');
    }

    std::string ParseIdent()
    {
        SkipWs();
        if (m_Pos >= m_S.size() || !IsIdentStart(m_S[m_Pos]))
            return {};
        const size_t start = m_Pos;
        ++m_Pos;
        while (m_Pos < m_S.size() && IsIdentChar(m_S[m_Pos]))
            ++m_Pos;
        return std::string(m_S.substr(start, m_Pos - start));
    }

    double ParseNumber(EvalResult& r)
    {
        SkipWs();
        const size_t start = m_Pos;
        bool sawDigit = false;
        while (m_Pos < m_S.size())
        {
            const char c = m_S[m_Pos];
            if (c >= '0' && c <= '9')
            {
                sawDigit = true;
                ++m_Pos;
            }
            else if (c == '.')
            {
                ++m_Pos;
            }
            else
            {
                break;
            }
        }
        if (!sawDigit)
            return 0.0;
        try
        {
            return std::stod(std::string(m_S.substr(start, m_Pos - start)));
        }
        catch (...)
        {
            r.ok = false;
            r.error = "invalid number";
            return 0.0;
        }
    }

    double ParseFunctionCall(const std::string& fn, EvalResult& r)
    {
        // Assumes '(' already matched.
        std::vector<double> args;
        SkipWs();
        if (Match(')'))
        {
            // no-arg call
        }
        else
        {
            for (;;)
            {
                const double a = ParseExpression(r);
                if (!r.ok)
                    return 0.0;
                args.push_back(a);
                SkipWs();
                if (Match(')'))
                    break;
                if (!Match(','))
                {
                    r.ok = false;
                    r.error = "expected ',' or ')'";
                    return 0.0;
                }
            }
        }

        const std::string f = ToLowerAscii(fn);
        if (f == "ceil")
        {
            if (args.size() != 1)
            {
                r.ok = false;
                r.error = "ceil() expects 1 arg";
                return 0.0;
            }
            return std::ceil(args[0]);
        }
        if (f == "floor")
        {
            if (args.size() != 1)
            {
                r.ok = false;
                r.error = "floor() expects 1 arg";
                return 0.0;
            }
            return std::floor(args[0]);
        }
        if (f == "min")
        {
            if (args.empty())
            {
                r.ok = false;
                r.error = "min() expects >= 1 arg";
                return 0.0;
            }
            double v = args[0];
            for (size_t i = 1; i < args.size(); ++i)
                v = std::min(v, args[i]);
            return v;
        }
        if (f == "max")
        {
            if (args.empty())
            {
                r.ok = false;
                r.error = "max() expects >= 1 arg";
                return 0.0;
            }
            double v = args[0];
            for (size_t i = 1; i < args.size(); ++i)
                v = std::max(v, args[i]);
            return v;
        }

        r.ok = false;
        r.error = "unknown function '" + fn + "'";
        return 0.0;
    }

    double ParseFactor(EvalResult& r)
    {
        SkipWs();
        if (Match('+'))
        {
            return ParseFactor(r);
        }
        if (Match('-'))
        {
            return -ParseFactor(r);
        }
        if (Match('('))
        {
            const double v = ParseExpression(r);
            if (!r.ok)
                return 0.0;
            if (!Match(')'))
            {
                r.ok = false;
                r.error = "expected ')'";
                return 0.0;
            }
            return v;
        }

        // number
        {
            const size_t save = m_Pos;
            const double n = ParseNumber(r);
            if (!r.ok)
                return 0.0;
            if (m_Pos != save)
                return n;
        }

        // identifier / function
        const std::string ident = ParseIdent();
        if (!ident.empty())
        {
            SkipWs();
            if (Match('('))
            {
                return ParseFunctionCall(ident, r);
            }

            const auto it = m_Vars.find(ident);
            if (it == m_Vars.end())
            {
                r.ok = false;
                r.error = "unknown identifier '" + ident + "'";
                return 0.0;
            }
            return it->second;
        }

        r.ok = false;
        r.error = "expected number or identifier";
        return 0.0;
    }

  private:
    std::string_view m_S;
    const std::unordered_map<std::string, double>& m_Vars;
    size_t m_Pos = 0;
};
} // namespace

bool EvalExpression(const std::string& expression,
                    const std::unordered_map<std::string, double>& vars,
                    double& outValue,
                    std::string* outError)
{
    Parser p(expression, vars);
    EvalResult r = p.Eval();
    if (!r.ok)
    {
        if (outError)
            *outError = r.error.empty() ? "expression eval failed" : r.error;
        return false;
    }
    outValue = r.value;
    return true;
}

bool BuildVariables(const nlohmann::json& jsonVariables,
                    std::unordered_map<std::string, double>& inOutVars,
                    std::string* outError)
{
    if (!jsonVariables.is_object())
        return true;

    std::unordered_map<std::string, std::string> pending;
    for (auto it = jsonVariables.begin(); it != jsonVariables.end(); ++it)
    {
        const std::string name = it.key();
        const auto& v = it.value();
        if (v.is_number())
        {
            inOutVars[name] = v.get<double>();
        }
        else if (v.is_string())
        {
            pending[name] = v.get<std::string>();
        }
    }

    // Resolve string expressions with a simple fixed-point iteration.
    for (size_t iter = 0; iter < pending.size() + 4; ++iter)
    {
        bool progressed = false;
        for (auto it = pending.begin(); it != pending.end();)
        {
            double val = 0.0;
            std::string err;
            if (EvalExpression(it->second, inOutVars, val, &err))
            {
                inOutVars[it->first] = val;
                it = pending.erase(it);
                progressed = true;
            }
            else
            {
                ++it;
            }
        }
        if (!progressed)
            break;
    }

    if (!pending.empty())
    {
        if (outError)
        {
            std::ostringstream oss;
            oss << "unresolved variables: ";
            bool first = true;
            for (const auto& kv : pending)
            {
                if (!first)
                    oss << ", ";
                first = false;
                oss << kv.first;
            }
            *outError = oss.str();
        }
        return false;
    }
    return true;
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Expr

