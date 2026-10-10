#include "Rendering/ShaderGraph/SgGlslSignatureReader.h"

#include <cctype>
#include <sstream>

namespace GameEngine::ShaderGraph
{
namespace
{

bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string ReadIdentifier(const std::string& s, size_t& i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    const size_t start = i;
    while (i < s.size() && IsIdentifierChar(s[i]))
        ++i;
    return s.substr(start, i - start);
}

std::string ReadType(const std::string& s, size_t& i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    const size_t start = i;
    while (i < s.size() && (IsIdentifierChar(s[i]) || s[i] == '.'))
        ++i;
    return s.substr(start, i - start);
}

bool ParseParameterList(const std::string& params, std::vector<SgPortMeta>& inputs, std::vector<SgPortMeta>& outputs)
{
    size_t i = 0;
    while (i < params.size())
    {
        while (i < params.size() && (std::isspace(static_cast<unsigned char>(params[i])) || params[i] == ','))
            ++i;
        if (i >= params.size())
            break;

        bool isOut = false;
        if (params.compare(i, 3, "out") == 0 && !IsIdentifierChar(params[i + 3]))
        {
            isOut = true;
            i += 3;
        }
        else if (params.compare(i, 2, "in") == 0 && !IsIdentifierChar(params[i + 2]))
        {
            i += 2;
        }

        const std::string typeName = ReadType(params, i);
        if (typeName.empty())
            break;
        const std::string name = ReadIdentifier(params, i);
        if (name.empty())
            break;

        SgPortMeta port;
        port.Name = name;
        port.GlslType = typeName;
        port.GraphType = GlslTypeToGraphType(typeName);
        port.DisplayName = name;
        if (isOut)
            outputs.push_back(std::move(port));
        else
            inputs.push_back(std::move(port));
    }
    return true;
}

// Line comments stripped so prose mentioning a function name can never read as
// a signature. The node library uses // comments only (no /* */ blocks).
std::string StripLineComments(const std::string& source)
{
    std::string text;
    text.reserve(source.size());
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);
        text += line;
        text.push_back('\n');
    }
    return text;
}

} // namespace

std::vector<SgNodeOverload> ReadTaggedFunctionOverloads(const std::string& source,
                                                        const std::string& expectedBaseName)
{
    std::vector<SgNodeOverload> overloads;
    if (expectedBaseName.empty())
        return overloads;

    // Whole-text scan (not per line): parameter lists may wrap across lines —
    // SG_SampleTexture2D's do, and the per-line reader silently dropped them.
    const std::string text = StripLineComments(source);

    for (size_t searchFrom = 0;;)
    {
        const size_t fnPos = text.find(expectedBaseName, searchFrom);
        if (fnPos == std::string::npos)
            break;
        searchFrom = fnPos + 1;

        // Exact-name matches only: an identifier char on either side means a
        // DIFFERENT function (SG_SampleTexture2DLOD is not an overload of
        // SG_SampleTexture2D).
        if (fnPos > 0 && IsIdentifierChar(text[fnPos - 1]))
            continue;
        size_t cursor = fnPos + expectedBaseName.size();
        if (cursor < text.size() && IsIdentifierChar(text[cursor]))
            continue;

        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
            ++cursor;
        if (cursor >= text.size() || text[cursor] != '(')
            continue;
        const size_t paramsStart = cursor + 1;
        const size_t paramsEnd = text.find(')', paramsStart);
        if (paramsEnd == std::string::npos)
            continue;

        // A definition carries a return type immediately before the name; an
        // expression fragment (call site, `return SG_Foo(...)`) does not.
        size_t typeEnd = fnPos;
        while (typeEnd > 0 && std::isspace(static_cast<unsigned char>(text[typeEnd - 1])))
            --typeEnd;
        size_t typeBegin = typeEnd;
        while (typeBegin > 0 && IsIdentifierChar(text[typeBegin - 1]))
            --typeBegin;
        const std::string retType = text.substr(typeBegin, typeEnd - typeBegin);
        if (retType.empty() || retType == "return")
            continue;

        SgNodeOverload overload;
        overload.FunctionName = expectedBaseName;
        ParseParameterList(text.substr(paramsStart, paramsEnd - paramsStart), overload.Inputs,
                           overload.Outputs);

        if (retType != "void")
        {
            SgPortMeta result;
            result.Name = "result";
            result.GlslType = retType;
            result.GraphType = GlslTypeToGraphType(retType);
            result.DisplayName = "Result";
            overload.Outputs.push_back(std::move(result));
        }

        overloads.push_back(std::move(overload));
        searchFrom = paramsEnd;
    }
    return overloads;
}

} // namespace GameEngine::ShaderGraph
