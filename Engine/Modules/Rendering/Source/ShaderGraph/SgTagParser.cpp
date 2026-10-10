#include "Rendering/ShaderGraph/SgTagParser.h"

#include "Rendering/ShaderGraph/SgGlslSignatureReader.h"

#include <fstream>
#include <sstream>

namespace GameEngine::ShaderGraph
{
namespace
{

std::string Trim(std::string_view v)
{
    size_t b = 0;
    size_t e = v.size();
    while (b < e && std::isspace(static_cast<unsigned char>(v[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1])))
        --e;
    return std::string(v.substr(b, e - b));
}

std::string ToLowerAscii(std::string_view value)
{
    std::string out(value);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string StripCommentBody(std::string_view line)
{
    std::string_view s = line;
    if (s.rfind("//", 0) == 0)
        s.remove_prefix(2);
    if (!s.empty() && s[0] == ' ')
        s.remove_prefix(1);
    return Trim(s);
}

bool StartsWithTag(const std::string& body, const char* prefix)
{
    return body.rfind(prefix, 0) == 0;
}

std::string Unquote(std::string_view v)
{
    std::string out = Trim(v);
    if (out.size() >= 2 &&
        ((out.front() == '"' && out.back() == '"') || (out.front() == '\'' && out.back() == '\'')))
        return out.substr(1, out.size() - 2);
    return out;
}

bool IsAttributeNameChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool TryParseInt(const std::string& text, int& out)
{
    if (text.empty())
        return false;
    try
    {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed == 0)
            return false;
        out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool TryParseFloat(const std::string& text, float& out)
{
    if (text.empty())
        return false;
    try
    {
        size_t consumed = 0;
        const float value = std::stof(text, &consumed);
        if (consumed == 0)
            return false;
        out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

size_t FindNextAttributeKeyStart(const std::string& s, size_t searchFrom)
{
    for (size_t i = searchFrom; i < s.size(); ++i)
    {
        if (s[i] != ' ' && s[i] != '\t')
            continue;

        size_t j = i + 1;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
            ++j;
        if (j >= s.size())
            return s.size();

        size_t k = j;
        while (k < s.size() && IsAttributeNameChar(s[k]))
            ++k;
        // A key needs at least one identifier character: a bare '=' inside a
        // value ("1.0 == 203") must not start a phantom key.
        if (k > j && k < s.size() && s[k] == '=')
            return i;
    }
    return s.size();
}

void ParseKeyValuePairs(const std::string& rest, std::vector<std::pair<std::string, std::string>>& out,
                        std::vector<std::string>& positional)
{
    size_t i = 0;
    while (i < rest.size())
    {
        while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i])))
            ++i;
        if (i >= rest.size())
            break;

        const size_t start = i;
        if (rest[start] == '"' || rest[start] == '\'')
        {
            // A quoted positional argument ("Pulse Speed") is one token, spaces
            // included; the quotes stay on so a consumer can tell it from a bare word.
            const size_t close = rest.find(rest[start], start + 1);
            const size_t end = close == std::string::npos ? rest.size() : close + 1;
            positional.push_back(rest.substr(start, end - start));
            i = end;
            continue;
        }
        const size_t eq = rest.find('=', start);
        size_t tokenEnd = start;
        while (tokenEnd < rest.size() && !std::isspace(static_cast<unsigned char>(rest[tokenEnd])))
            ++tokenEnd;

        if (eq != std::string::npos && eq < tokenEnd)
        {
            const std::string key = rest.substr(start, eq - start);
            // A quoted value is exactly the quoted span, whatever it contains
            // (tooltip="1.0 == 203 nits" must not scan for the next key inside).
            // The quotes stay on, as with a quoted positional, so a consumer can
            // tell verbatim text from a bare value it may post-process.
            size_t valueEnd;
            if (eq + 1 < rest.size() && (rest[eq + 1] == '"' || rest[eq + 1] == '\''))
            {
                const size_t close = rest.find(rest[eq + 1], eq + 2);
                valueEnd = close == std::string::npos ? rest.size() : close + 1;
            }
            else
            {
                valueEnd = FindNextAttributeKeyStart(rest, eq + 1);
            }
            const std::string value = Trim(rest.substr(eq + 1, valueEnd - (eq + 1)));
            out.emplace_back(key, value);
            i = valueEnd;
            continue;
        }

        positional.push_back(rest.substr(start, tokenEnd - start));
        i = tokenEnd;
    }
}

std::pair<std::string, std::string> SplitEdgeEndpoint(const std::string& endpoint)
{
    const auto dot = endpoint.rfind('.');
    if (dot == std::string::npos)
        return {Trim(endpoint), ""};
    return {Trim(endpoint.substr(0, dot)), Trim(endpoint.substr(dot + 1))};
}

} // namespace

std::vector<std::pair<std::string, std::string>> ParseTagAttributes(const std::string& line)
{
    std::vector<std::pair<std::string, std::string>> attrs;
    const std::string body = StripCommentBody(line);
    if (!StartsWithTag(body, "@"))
        return attrs;

    if (StartsWithTag(body, "@sgnode"))
    {
        attrs.emplace_back("kind", "sgnode");
        attrs.emplace_back("arg0", Trim(body.substr(7)));
        return attrs;
    }

    std::string rest = body;
    if (StartsWithTag(rest, "@sg-"))
        rest = rest.substr(4);
    else if (StartsWithTag(rest, "@"))
        rest = rest.substr(1);

    const auto space = rest.find(' ');
    if (space == std::string::npos)
    {
        attrs.emplace_back("kind", rest);
        return attrs;
    }

    attrs.emplace_back("kind", rest.substr(0, space));
    std::vector<std::string> positional;
    ParseKeyValuePairs(rest.substr(space + 1), attrs, positional);
    for (size_t i = 0; i < positional.size(); ++i)
        attrs.emplace_back("arg" + std::to_string(i), positional[i]);
    return attrs;
}

std::optional<std::string> FindTagAttribute(const std::vector<std::pair<std::string, std::string>>& attrs,
                                            const std::string& key)
{
    for (const auto& [k, v] : attrs)
    {
        if (k == key)
            return v;
    }
    return std::nullopt;
}

namespace
{

// Single-value tags are written positionally (`@sg-lighting Unlit`), so the value
// arrives as arg0 rather than under the tag's own name. Accept the `key=value`
// spelling too, since the grammar allows it everywhere else.
std::optional<std::string> FindTagValue(const std::vector<std::pair<std::string, std::string>>& attrs,
                                        const std::string& key)
{
    if (auto named = FindTagAttribute(attrs, key))
        return named;
    return FindTagAttribute(attrs, "arg0");
}

} // namespace

bool IsShaderGraphSource(const std::string& source)
{
    std::istringstream iss(source);
    std::string line;
    int lineCount = 0;
    while (std::getline(iss, line) && lineCount++ < 64)
    {
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
            continue;

        const bool isComment = trimmed.rfind("//", 0) == 0;
        if (!isComment)
            break;

        const std::string body = StripCommentBody(line);
        if (body.rfind("@sg-graph", 0) == 0)
            return true;
    }
    return false;
}

SgParsedFile ParseShaderGraphSource(const std::string& source)
{
    SgParsedFile parsed;
    parsed.IsGraphFile = IsShaderGraphSource(source);

    std::istringstream iss(source);
    std::string line;
    std::ostringstream tagBlock;
    std::ostringstream body;
    bool inTagBlock = true;

    while (std::getline(iss, line))
    {
        const std::string trimmed = Trim(line);
        const bool isComment = trimmed.rfind("//", 0) == 0;

        if (inTagBlock)
        {
            if (trimmed.empty() || isComment)
            {
                tagBlock << line << '\n';
                continue;
            }

            inTagBlock = false;
        }

        body << line << '\n';
    }

    parsed.TagBlock = tagBlock.str();
    parsed.Body = body.str();
    return parsed;
}

SgParsedFile ParseShaderGraphFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return ParseShaderGraphSource(oss.str());
}

SgGraphDocument ParseGraphDocumentFromTags(const std::string& tagBlock)
{
    SgGraphDocument doc;
    std::istringstream iss(tagBlock);
    std::string line;
    while (std::getline(iss, line))
    {
        auto attrs = ParseTagAttributes(line);
        if (attrs.empty())
            continue;
        // Graph tags treat a quoted value and a bare one alike; strip quotes once
        // so number parsing and pin defaults see the content.
        for (auto& [k, v] : attrs)
        {
            if (k.rfind("arg", 0) != 0)
                v = Unquote(v);
        }
        const std::string& kind = attrs[0].first == "kind" ? attrs[0].second : attrs[0].first;

        if (kind == "graph")
        {
            if (auto arg0 = FindTagAttribute(attrs, "arg0"))
                doc.GraphName = *arg0;
            for (const auto& [k, v] : attrs)
            {
                if (k == "version")
                {
                    int parsed = doc.Version;
                    if (TryParseInt(v, parsed))
                        doc.Version = parsed;
                }
            }
        }
        else if (kind == "version")
        {
            if (auto arg0 = FindTagAttribute(attrs, "arg0"))
            {
                int parsed = doc.Version;
                if (TryParseInt(*arg0, parsed))
                    doc.Version = parsed;
            }
        }
        else if (kind == "stage")
        {
            const auto stage = FindTagValue(attrs, "stage").value_or("surface");
            if (stage == "vertex")
                doc.Stage = SgGraphStage::Vertex;
            else if (stage == "both")
                doc.Stage = SgGraphStage::Both;
            else
                doc.Stage = SgGraphStage::Surface;
        }
        else if (kind == "lighting")
        {
            doc.LightingModel = FindTagValue(attrs, "lighting").value_or("StandardPBR");
        }
        else if (kind == "variant")
        {
            if (auto v = FindTagValue(attrs, "variant"))
                doc.VariantDefines.push_back(*v);
        }
        else if (kind == "materialized")
        {
            doc.Materialized = true;
        }
        else if (kind == "property")
        {
            SgGraphProperty prop;
            if (auto name = FindTagAttribute(attrs, "arg0"))
                prop.Name = *name;
            if (auto ty = FindTagAttribute(attrs, "arg1"))
                prop.Type = *ty;
            if (auto d = FindTagAttribute(attrs, "default"))
                prop.DefaultValue = *d;
            if (auto r = FindTagAttribute(attrs, "range"))
            {
                const auto comma = r->find(',');
                if (comma != std::string::npos)
                {
                    prop.RangeMin = r->substr(0, comma);
                    prop.RangeMax = r->substr(comma + 1);
                }
            }
            if (auto h = FindTagAttribute(attrs, "hint"))
                prop.Hint = *h;
            if (auto pub = FindTagAttribute(attrs, "public"))
            {
                const std::string value = ToLowerAscii(*pub);
                prop.IsPublic = (value == "true" || value == "1" || value == "yes");
            }
            if (!prop.Name.empty())
                doc.Properties.push_back(std::move(prop));
        }
        else if (kind == "texture")
        {
            SgGraphTexture tex;
            if (auto name = FindTagAttribute(attrs, "arg0"))
                tex.Name = *name;
            if (auto guid = FindTagAttribute(attrs, "arg1"))
                tex.Guid = Unquote(*guid);
            if (auto h = FindTagAttribute(attrs, "hint"))
                tex.Hint = *h;
            if (!tex.Name.empty())
                doc.Textures.push_back(std::move(tex));
        }
        else if (kind == "node")
        {
            SgGraphNode node;
            if (auto id = FindTagAttribute(attrs, "arg0"))
                node.Id = *id;
            if (auto ty = FindTagAttribute(attrs, "type"))
                node.TypeId = *ty;
            if (auto pos = FindTagAttribute(attrs, "pos"))
            {
                const auto open = pos->find('(');
                const auto comma = pos->find(',', open == std::string::npos ? 0 : open);
                const auto close = pos->find(')', comma == std::string::npos ? 0 : comma);
                if (open != std::string::npos && comma != std::string::npos)
                {
                    float px = node.PositionX;
                    float py = node.PositionY;
                    if (TryParseFloat(pos->substr(open + 1, comma - open - 1), px))
                        node.PositionX = px;
                    if (TryParseFloat(pos->substr(comma + 1, close - comma - 1), py))
                        node.PositionY = py;
                }
            }
            for (const auto& [k, v] : attrs)
            {
                if (k == "kind" || k == "arg0" || k == "type" || k == "pos" || k.rfind("arg", 0) == 0)
                    continue;
                node.PortDefaults[k] = v;
            }
            if (!node.Id.empty() && !node.TypeId.empty())
                doc.Nodes.push_back(std::move(node));
        }
        else if (kind == "edge")
        {
            std::string edgeText;
            const std::string body = StripCommentBody(line);
            const auto edgePos = body.find("edge");
            if (edgePos != std::string::npos)
                edgeText = Trim(body.substr(edgePos + 4));
            if (edgeText.empty())
                edgeText = FindTagAttribute(attrs, "arg0").value_or("");
            const auto arrow = edgeText.find("->");
            if (arrow == std::string::npos)
                continue;
            SgGraphEdge edge;
            const auto [srcNode, srcPort] = SplitEdgeEndpoint(Trim(edgeText.substr(0, arrow)));
            const auto [dstNode, dstPort] = SplitEdgeEndpoint(Trim(edgeText.substr(arrow + 2)));
            edge.SourceNodeId = srcNode;
            edge.SourcePortId = srcPort;
            edge.TargetNodeId = dstNode;
            edge.TargetPortId = dstPort;
            doc.Edges.push_back(std::move(edge));
        }
        else if (kind == "viewport")
        {
            if (auto v = FindTagAttribute(attrs, "panX"))
            {
                float parsed = doc.ViewportPanX;
                if (TryParseFloat(*v, parsed))
                    doc.ViewportPanX = parsed;
            }
            if (auto v = FindTagAttribute(attrs, "panY"))
            {
                float parsed = doc.ViewportPanY;
                if (TryParseFloat(*v, parsed))
                    doc.ViewportPanY = parsed;
            }
            if (auto v = FindTagAttribute(attrs, "zoom"))
            {
                float parsed = doc.ViewportZoom;
                if (TryParseFloat(*v, parsed))
                    doc.ViewportZoom = parsed;
            }
        }
        else
        {
            doc.UnknownTags.push_back(Trim(line));
        }
    }
    return doc;
}

std::string SerializeGraphDocumentTags(const SgGraphDocument& doc)
{
    std::ostringstream oss;
    oss << "// Auto-generated. The @sg-* tag block is authoritative.\n";
    oss << "// The body below is regenerated by the graph editor on save.\n//\n";
    oss << "// @sg-graph     " << doc.GraphName << "\n";
    oss << "// @sg-version   " << doc.Version << "\n";
    const char* stageStr = doc.Stage == SgGraphStage::Both   ? "both"
                           : doc.Stage == SgGraphStage::Vertex ? "vertex"
                                                               : "surface";
    oss << "// @sg-stage     " << stageStr << "\n";
    if (!doc.LightingModel.empty())
        oss << "// @sg-lighting  " << doc.LightingModel << "\n";
    if (doc.Materialized)
        oss << "// @sg-materialized true\n";
    for (const auto& def : doc.VariantDefines)
        oss << "// @sg-variant   " << def << "\n";
    for (const auto& prop : doc.Properties)
    {
        oss << "// @sg-property  " << prop.Name << " " << prop.Type;
        if (!prop.DefaultValue.empty())
            oss << " default=" << prop.DefaultValue;
        if (!prop.RangeMin.empty() && !prop.RangeMax.empty())
            oss << " range=" << prop.RangeMin << "," << prop.RangeMax;
        if (!prop.Hint.empty())
            oss << " hint=" << prop.Hint;
        if (prop.IsPublic)
            oss << " public=true";
        oss << "\n";
    }
    for (const auto& tex : doc.Textures)
    {
        oss << "// @sg-texture   " << tex.Name << " \"" << tex.Guid << "\"";
        if (!tex.Hint.empty())
            oss << " hint=" << tex.Hint;
        oss << "\n";
    }
    if (doc.ViewportPanX != 0.f || doc.ViewportPanY != 0.f || doc.ViewportZoom != 1.f)
        oss << "// @sg-viewport  panX=" << doc.ViewportPanX << " panY=" << doc.ViewportPanY
            << " zoom=" << doc.ViewportZoom << "\n";
    for (const std::string& tag : doc.UnknownTags)
        oss << tag << "\n";
    oss << "//\n";
    for (const auto& node : doc.Nodes)
    {
        oss << "// @sg-node      " << node.Id << " type=" << node.TypeId << " pos=(" << node.PositionX
            << "," << node.PositionY << ")";
        for (const auto& [port, value] : node.PortDefaults)
            oss << " " << port << "=" << value;
        oss << "\n";
    }
    for (const auto& edge : doc.Edges)
    {
        oss << "// @sg-edge      " << edge.SourceNodeId << "." << edge.SourcePortId << " -> "
            << edge.TargetNodeId << "." << edge.TargetPortId << "\n";
    }
    return oss.str();
}

SgNodeDefinition ParseHelperNodeFromSource(const std::filesystem::path& path,
                                            const std::string& source,
                                            const std::string& typeId,
                                            const std::vector<std::pair<std::string, std::string>>& fileTags)
{
    SgNodeDefinition def;
    def.TypeId = typeId;
    def.SourceFile = path.string();

    std::istringstream iss(source);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line))
        lines.push_back(line);

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto attrs = ParseTagAttributes(lines[i]);
        if (attrs.empty())
            continue;
        const std::string kind = attrs[0].first == "kind" ? attrs[0].second : "";
        if (kind == "sgnode")
            continue;
        if (kind == "display")
            def.DisplayName = Unquote(FindTagAttribute(attrs, "display").value_or(""));
        else if (kind == "category")
        {
            // @category Math/Basic → kind=category, arg0=Math/Basic (no category= key).
            std::string category = FindTagAttribute(attrs, "category").value_or("");
            if (category.empty())
                category = FindTagAttribute(attrs, "arg0").value_or("");
            def.Category = Unquote(category);
        }
        else if (kind == "tooltip")
            def.Tooltip = Unquote(FindTagAttribute(attrs, "tooltip").value_or(""));
        else if (kind == "keywords")
            def.Keywords = Unquote(FindTagAttribute(attrs, "keywords").value_or(""));
        else if (kind == "deprecated")
            def.DeprecatedReason = Unquote(FindTagAttribute(attrs, "deprecated").value_or(""));
        else if (kind == "param" || kind == "out")
        {
            // port metadata collected per overload below
        }
    }

    if (def.Category.empty())
    {
        for (const auto& [k, v] : fileTags)
        {
            if (k == "category")
            {
                def.Category = Unquote(v);
                break;
            }
        }
    }

    if (def.DisplayName.empty() && !def.TypeId.empty())
        def.DisplayName = def.TypeId;

    const auto overloads = ReadTaggedFunctionOverloads(source, "SG_" + def.TypeId);
    for (auto& o : overloads)
    {
        if (o.FunctionName.empty())
            continue;
        def.Overloads.push_back(std::move(o));
    }
    return def;
}

} // namespace GameEngine::ShaderGraph
