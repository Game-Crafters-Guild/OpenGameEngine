#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{

// Semantic version for package manifests (npm-compatible subset).
//
// Full form: "major.minor.patch[-prerelease]". Partial forms ("2", "2.1")
// are accepted for RANGES (missing parts read as 0 and widen the range),
// while a package's own version must be a full three-part semver.
//
// Ordering: numeric on major/minor/patch; a version WITH a prerelease tag
// sorts below the same numerals without one; prerelease tags compare
// lexicographically (P0 simplification of semver's dot-segment rules).
struct PackageVersion
{
    int Major = 0;
    int Minor = 0;
    int Patch = 0;
    std::string PreRelease;
    // How many numeric parts the source text specified (1-3). Ranges use
    // this to widen partial versions ("2.1" == ">=2.1.0 <2.2.0").
    int SpecifiedParts = 0;

    static bool TryParse(std::string_view text, PackageVersion& out, std::string* outError = nullptr);

    // <0 / 0 / >0 like strcmp. Ignores SpecifiedParts.
    int Compare(const PackageVersion& other) const;

    std::string ToString() const;
};

// Version range over one PackageVersion base. P0 supports the operators the
// package plan calls for: exact ("1.2.3", partial "2.1" behaves like npm's
// x-range), caret ("^2.1"), tilde ("~1.2.3"), ">=1.0.0", and "*" (any).
struct PackageVersionRange
{
    enum class Kind
    {
        Exact,
        Caret,
        Tilde,
        GreaterOrEqual,
        Any,
    };

    Kind RangeKind = Kind::Any;
    PackageVersion Base;

    static bool TryParse(std::string_view text, PackageVersionRange& out, std::string* outError = nullptr);

    bool Matches(const PackageVersion& version) const;

    std::string ToString() const;
};

} // namespace GameEngine
