#include "Assets/Packages/PackageVersion.h"

#include <cctype>
#include <charconv>

namespace GameEngine
{

namespace
{

bool TryParseNumericPart(std::string_view text, int& out)
{
    if (text.empty() || text.size() > 9)
        return false;
    for (char c : text)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    // Leading zeros are tolerated (npm forbids them; not worth a loud error here).
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{};
}

void SetError(std::string* outError, std::string message)
{
    if (outError)
        *outError = std::move(message);
}

} // namespace

bool PackageVersion::TryParse(std::string_view text, PackageVersion& out, std::string* outError)
{
    out = PackageVersion{};
    if (text.empty())
    {
        SetError(outError, "version is empty");
        return false;
    }

    // Split off "-prerelease" (build metadata "+..." is dropped).
    std::string_view numerals = text;
    const size_t plus = numerals.find('+');
    if (plus != std::string_view::npos)
        numerals = numerals.substr(0, plus);
    const size_t dash = numerals.find('-');
    if (dash != std::string_view::npos)
    {
        out.PreRelease = std::string(numerals.substr(dash + 1));
        if (out.PreRelease.empty())
        {
            SetError(outError, "empty prerelease tag in '" + std::string(text) + "'");
            return false;
        }
        numerals = numerals.substr(0, dash);
    }

    int parts[3] = {0, 0, 0};
    int partCount = 0;
    size_t begin = 0;
    while (begin <= numerals.size())
    {
        const size_t dot = numerals.find('.', begin);
        const std::string_view part =
            numerals.substr(begin, dot == std::string_view::npos ? std::string_view::npos : dot - begin);
        if (partCount >= 3 || !TryParseNumericPart(part, parts[partCount]))
        {
            SetError(outError, "invalid semver '" + std::string(text) + "' (expected major[.minor[.patch]][-pre])");
            return false;
        }
        ++partCount;
        if (dot == std::string_view::npos)
            break;
        begin = dot + 1;
    }

    out.Major = parts[0];
    out.Minor = parts[1];
    out.Patch = parts[2];
    out.SpecifiedParts = partCount;
    return true;
}

int PackageVersion::Compare(const PackageVersion& other) const
{
    if (Major != other.Major)
        return Major < other.Major ? -1 : 1;
    if (Minor != other.Minor)
        return Minor < other.Minor ? -1 : 1;
    if (Patch != other.Patch)
        return Patch < other.Patch ? -1 : 1;
    // Release > prerelease; two prereleases compare lexicographically.
    if (PreRelease.empty() != other.PreRelease.empty())
        return PreRelease.empty() ? 1 : -1;
    const int cmp = PreRelease.compare(other.PreRelease);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

std::string PackageVersion::ToString() const
{
    std::string s = std::to_string(Major) + "." + std::to_string(Minor) + "." + std::to_string(Patch);
    if (!PreRelease.empty())
        s += "-" + PreRelease;
    return s;
}

bool PackageVersionRange::TryParse(std::string_view text, PackageVersionRange& out, std::string* outError)
{
    out = PackageVersionRange{};

    // Trim whitespace.
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);

    if (text.empty() || text == "*")
    {
        out.RangeKind = Kind::Any;
        return true;
    }

    if (text.front() == '^')
    {
        out.RangeKind = Kind::Caret;
        text.remove_prefix(1);
    }
    else if (text.front() == '~')
    {
        out.RangeKind = Kind::Tilde;
        text.remove_prefix(1);
    }
    else if (text.rfind(">=", 0) == 0)
    {
        out.RangeKind = Kind::GreaterOrEqual;
        text.remove_prefix(2);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
    }
    else
    {
        out.RangeKind = Kind::Exact;
    }

    return PackageVersion::TryParse(text, out.Base, outError);
}

bool PackageVersionRange::Matches(const PackageVersion& version) const
{
    // Lower bound shared by every operator except Any.
    if (RangeKind != Kind::Any && version.Compare(Base) < 0)
        return false;

    switch (RangeKind)
    {
    case Kind::Any:
        return true;
    case Kind::GreaterOrEqual:
        return true;
    case Kind::Exact:
    {
        // Full "1.2.3" is equality; partial "2.1" behaves like npm's x-range
        // ">=2.1.0 <2.2.0", "2" like ">=2.0.0 <3.0.0".
        if (Base.SpecifiedParts >= 3)
            return version.Compare(Base) == 0;
        if (version.Major != Base.Major)
            return false;
        if (Base.SpecifiedParts == 2 && version.Minor != Base.Minor)
            return false;
        return true;
    }
    case Kind::Caret:
    {
        // First non-zero component is the breaking boundary (npm caret).
        if (Base.Major > 0)
            return version.Major == Base.Major;
        if (Base.Minor > 0)
            return version.Major == 0 && version.Minor == Base.Minor;
        return version.Major == 0 && version.Minor == 0 && version.Patch == Base.Patch;
    }
    case Kind::Tilde:
    {
        // "~1.2.3" / "~1.2" pin the minor; "~1" pins only the major.
        if (version.Major != Base.Major)
            return false;
        if (Base.SpecifiedParts >= 2 && version.Minor != Base.Minor)
            return false;
        return true;
    }
    }
    return false;
}

std::string PackageVersionRange::ToString() const
{
    switch (RangeKind)
    {
    case Kind::Any:
        return "*";
    case Kind::Exact:
        return Base.ToString();
    case Kind::Caret:
        return "^" + Base.ToString();
    case Kind::Tilde:
        return "~" + Base.ToString();
    case Kind::GreaterOrEqual:
        return ">=" + Base.ToString();
    }
    return Base.ToString();
}

} // namespace GameEngine
