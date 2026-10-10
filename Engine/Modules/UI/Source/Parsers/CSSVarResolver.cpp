// CSS custom-property var() resolution. Split out of CSSParser.cpp (P4f).

#include "CSSParserDetail.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace UIParsing
{
using namespace CSSDetail;

namespace
{

static CustomPropertyScope::LookupStatus LookupCustomVarRaw(
    const std::unordered_map<StringId, std::string>& customVars,
    const CustomPropertyScope* parentScope,
    StringId name,
    const std::string*& outValue)
{
    outValue = nullptr;
    auto it = customVars.find(name);
    if (it != customVars.end())
    {
        outValue = &it->second;
        return CustomPropertyScope::LookupStatus::Found;
    }
    if (parentScope)
    {
        const CustomPropertyScope::Entry* val = nullptr;
        auto st = parentScope->Lookup(name, val);
        if (st == CustomPropertyScope::LookupStatus::Found && val && !val->Invalid)
        {
            outValue = &val->Value;
            return CustomPropertyScope::LookupStatus::Found;
        }
        return st;
    }
    return CustomPropertyScope::LookupStatus::Missing;
}

} // namespace

bool CSSDetail::ContainsVarCall(std::string_view s)
{
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    auto lc = [](char c) { return (char)std::tolower((unsigned char)c); };
    for (size_t i = 0; i + 3 < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '"' && !inSingleQuote)
        {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }
        if (c == '\'' && !inDoubleQuote)
        {
            inSingleQuote = !inSingleQuote;
            continue;
        }
        if (inSingleQuote || inDoubleQuote)
            continue;
        if (lc(s[i]) == 'v' && lc(s[i + 1]) == 'a' && lc(s[i + 2]) == 'r' && s[i + 3] == '(')
            return true;
    }
    return false;
}

bool CSSDetail::ResolveVarFunctions(const std::unordered_map<StringId, std::string>& customVars,
                                const CustomPropertyScope* parentScope,
                                std::string_view input,
                                std::string& out,
                                int depth,
                                std::vector<std::string>& stack)
{
    constexpr int kMaxDepth = 32;
    if (depth > kMaxDepth)
        return false;

    out.clear();
    out.reserve(input.size());

    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    auto startsWithVarParen = [&](std::string_view s) -> bool
    {
        if (s.size() < 4)
            return false;
        const char v0 = s[0], v1 = s[1], v2 = s[2], v3 = s[3];
        auto lc = [](char c) { return (char)std::tolower((unsigned char)c); };
        return lc(v0) == 'v' && lc(v1) == 'a' && lc(v2) == 'r' && v3 == '(';
    };

    size_t i = 0;
    while (i < input.size())
    {
        const char c = input[i];
        if (c == '"' && !inSingleQuote)
        {
            inDoubleQuote = !inDoubleQuote;
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '\'' && !inDoubleQuote)
        {
            inSingleQuote = !inSingleQuote;
            out.push_back(c);
            ++i;
            continue;
        }

        if (!inSingleQuote && !inDoubleQuote && startsWithVarParen(input.substr(i)))
        {
            size_t j = i + 4;
            int parenDepth = 1;
            bool q1 = false, q2 = false;
            while (j < input.size())
            {
                const char cj = input[j];
                if (cj == '"' && !q1)
                {
                    q2 = !q2;
                    ++j;
                    continue;
                }
                if (cj == '\'' && !q2)
                {
                    q1 = !q1;
                    ++j;
                    continue;
                }
                if (!q1 && !q2)
                {
                    if (cj == '(')
                        ++parenDepth;
                    else if (cj == ')')
                    {
                        --parenDepth;
                        if (parenDepth == 0)
                            break;
                    }
                }
                ++j;
            }
            if (j >= input.size() || input[j] != ')')
                return false;

            std::string_view inner = input.substr(i + 4, j - (i + 4));
            inner = TrimView(inner);

            std::string_view namePart = inner;
            std::string_view fallbackPart{};
            {
                int pd = 0;
                bool qs = false, qd = false;
                for (size_t k = 0; k < inner.size(); ++k)
                {
                    const char ck = inner[k];
                    if (ck == '"' && !qs)
                    {
                        qd = !qd;
                        continue;
                    }
                    if (ck == '\'' && !qd)
                    {
                        qs = !qs;
                        continue;
                    }
                    if (!qs && !qd)
                    {
                        if (ck == '(')
                            ++pd;
                        else if (ck == ')')
                            pd = std::max(0, pd - 1);
                        else if (ck == ',' && pd == 0)
                        {
                            namePart = inner.substr(0, k);
                            fallbackPart = inner.substr(k + 1);
                            break;
                        }
                    }
                }
            }
            namePart = TrimView(namePart);
            fallbackPart = TrimView(fallbackPart);

            if (namePart.size() < 3 || !(namePart[0] == '-' && namePart[1] == '-'))
                return false;

            std::string varName(namePart);

            auto inStack = [&](const std::string& n) -> bool
            {
                return std::find(stack.begin(), stack.end(), n) != stack.end();
            };

            std::string resolved;
            const std::string* raw = nullptr;
            const bool cycle = inStack(varName);
            const StringId varId = HashStringId(varName);
            CustomPropertyScope::LookupStatus st = cycle ? CustomPropertyScope::LookupStatus::Missing
                                                         : LookupCustomVarRaw(customVars, parentScope, varId, raw);
            bool ok = false;
            if (!cycle && st == CustomPropertyScope::LookupStatus::Found && raw)
            {
                stack.push_back(varName);
                ok = ResolveVarFunctions(customVars, parentScope, *raw, resolved, depth + 1, stack);
                stack.pop_back();
            }

            if (!ok)
            {
                if (!fallbackPart.empty())
                {
                    ok = ResolveVarFunctions(customVars, parentScope, fallbackPart, resolved, depth + 1, stack);
                }
            }

            if (!ok)
                return false;

            out.append(resolved);
            i = j + 1;
            continue;
        }

        out.push_back(c);
        ++i;
    }

    return true;
}

} // namespace UIParsing
} // namespace GameEngine
