#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "UI/ResolvedStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

namespace GameEngine { namespace UIParsing {

// Logs one warning naming the unregistered declaration names a parsed sheet
// carried, so a property this engine has no row for stops being dropped in
// silence. Call once per load/hot-reload, after Stylesheet::SourceName is set —
// the name is the whole point of the message.
//
// CSS properties this engine has decided not to implement are filtered out, so
// the warning means "this declaration does nothing and you probably did not
// expect that" rather than restating a known gap on every boot. Returns how
// many names it actually warned about; a clean sheet returns 0 and logs
// nothing.
size_t WarnUnknownStylesheetProperties(const Stylesheet& sheet);

struct ElementState {
    bool Hover = false;
    bool Active = false;
    bool Focus = false;
    bool FocusVisible = false; // :focus-visible (set when focus came via keyboard)
    bool FocusWithin = false;
    bool Disabled = false;
    bool Enabled = true;
    bool Checked = false;
    // Stage 5 Block C: custom pseudo-state set, keyed by hashed StringId
    // of the lowercase state name. UIElement::AddCustomState / RemoveCustomState
    // populate the per-element source; ResolveCascadeForElement copies the
    // current set into ElementState before each cascade.
    std::unordered_set<StringId> CustomStates;
};

// Lexbor-backed CSS parser (required). No fallback implementation.
class CSSParser {
public:
    // Parse CSS text into a Stylesheet (simple rules: tag/id/class + :hover/:active/:focus; basic properties)
    static bool ParseStylesFromString(const std::string& cssText, Stylesheet& outSheet);

    // CSS property name → invalidation impact, answering for shorthands too by
    // folding in the longhands the name expands to: `outline` is paint-only
    // because outline-width/style/color are, and `overflow` is layout-affecting
    // because overflow-x/y are, even though the `overflow` slot on its own is
    // not. A zero impact means the name carries no invalidation information --
    // either the table has no row for it, or the row and its expansion are
    // special-cased by other consumers and classify as nothing here
    // (`transition`) -- and the caller owns the policy for that.
    static StylePropertyImpact PropertyImpactForName(const std::string& name);

    // True when the parser has a row for `name`, including the pure shorthands
    // (`border`, `border-top`, `flex`, `outline`) that exist only as expansions
    // and own no StylePropertyId of their own -- the predicate that separates
    // "unregistered" from "a shorthand with no id".
    static bool IsKnownPropertyName(const std::string& name);

    // Cascade resolution: produce a computed style for a UIElement given the
    // element state. The optional hoverTarget allows ancestor :hover selectors
    // (e.g. `.parent:hover .child`) to work by telling the matcher which
    // element and ancestor chain are currently hovered.
    static ResolvedStyle ComputeStyleFor(const UIElement& el,
                                         const Stylesheet& sheet,
                                         const ElementState& state,
                                         const UIElement* hoverTarget = nullptr);

    // Multi-stylesheet cascade entry point. The vector of sheets is ordered
    // from lowest to highest precedence; later stylesheets win over earlier
    // ones for equal-specificity rules.
    static ResolvedStyle ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const ElementState& state,
                                         const UIElement* hoverTarget = nullptr);

    // Multi-stylesheet cascade entry point with explicit parent style to support
    // CSS-wide keywords (inherit/initial/unset) and default inheritance.
    // The parentStyle should represent the logical parent for inheritance in the
    // current pass (UI tree parent for layout; visual/Yoga parent for draw).
    static ResolvedStyle ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget = nullptr);

    // Multi-stylesheet cascade entry point using a prebuilt per-stylesheet rule index
    // to prune selector matching candidates. sheetIndices must either be empty, or
    // have the same size/order as `sheets` (each entry may be null to fall back).
    static ResolvedStyle ComputeStyleFor(const UIElement& el,
                                         std::span<const Stylesheet* const> sheets,
                                         std::span<const StylesheetRuleIndex* const> sheetIndices,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget = nullptr);

    static ResolvedStyle ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const std::vector<const StylesheetRuleIndex*>& sheetIndices,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget = nullptr);

    // Output-parameter overload that writes directly into the caller's storage,
    // avoiding a temporary + copy. Preferred on hot paths (e.g. buildYoga).
    //
    // outMatchedPseudoStates (Stage 5 Block B): if non-null, receives a bitmask
    // of UIElement::MatchedPseudoFlag bits set for every dynamic pseudo-class
    // referenced by a rule that matched the element. Mark-dirty sites consult
    // this to skip elements whose style cannot change for a given state flip
    // (e.g. an element with no :hover-anchored matched rules can be skipped on
    // hover transitions). Pass nullptr when the predicate isn't needed.
    static void ComputeStyleInto(ResolvedStyle& out,
                                 const UIElement& el,
                                 std::span<const Stylesheet* const> sheets,
                                 std::span<const StylesheetRuleIndex* const> sheetIndices,
                                 const ElementState& state,
                                 const ResolvedStyle* parentStyle,
                                 const UIElement* hoverTarget = nullptr,
                                 uint16_t* outMatchedPseudoStates = nullptr);

    // Parse inline style declarations into a StyleOverrides bag.
    static void ParseInlineStyleToOverrides(std::string_view inlineStyle, StyleOverrides& out);

    // Cascade-matcher memoization Phase 3: build the per-element rule
    // cache. Walks each stylesheet's candidate rules (using the rule
    // index when available), runs a STRUCTURAL match (treats dynamic
    // pseudos as `true`), and stores hits in `el.m_CachedMatchedRules`.
    // Sets `el.m_RuleCacheValid = true` on completion, EXCEPT when `sheets`
    // is empty: an empty pool proves nothing about which rules match, so the
    // cache is left invalid for the next caller that has a real pool.
    //
    // Phase 3 callers populate the cache on demand from the cascade but
    // don't read from it — Phase 4 wires `ComputeStyleInto` to consume
    // the cache. This phase exists primarily so the population logic
    // can be exercised + validated against the live matcher before flip.
    static void BuildRuleCache(const UIElement& el,
                               std::span<const Stylesheet* const> sheets,
                               std::span<const StylesheetRuleIndex* const> sheetIndices);

    // Cascade-memoization Phase 4/5: returns true if the rule cache is
    // enabled for this process (env var GE_UI_RULE_CACHE evaluated at
    // first call). Consumed by the narrowing predicate at hover/focus
    // transition sites — narrowing is only sound when the cache is
    // primary, because m_MatchedPseudoStates is then accumulated from
    // structural matches (rules that COULD match) rather than filtered
    // matches (rules currently matching).
    static bool IsRuleCacheEnabled();

#if GE_DEBUG_INSTRUMENTATION
    // Cache-correctness validator. Compiled only where the engine carries
    // its debug instrumentation; release builds compile the validator out
    // entirely. Opt-in via env var
    // GE_UI_RULE_CACHE_VALIDATE=1 (read once at first query). When on,
    // ComputeStyleInto walks both the cached and live-filtered match
    // paths and asserts the cached match set is a superset of the
    // live-matched set, logging element + rule on divergence.
    //
    // Useful as insurance after the GE_UI_RULE_CACHE default-on flip:
    // any latent invalidation gap surfaces as a logged divergence rather
    // than as silent stale style. Adds substantial runtime cost (every
    // cascade does two selector walks) so it must be opt-in.
    static bool IsRuleCacheValidationEnabled();

    // Test hook: override the env-var gate so unit tests can drive the
    // validator without mutating process env. Pass `true` to enable,
    // `false` to disable, regardless of GE_UI_RULE_CACHE_VALIDATE.
    // Persists across calls — tests should pair the call with
    // ClearRuleCacheValidationOverrideForTest in their teardown so the
    // env-var fallback is restored for any subsequent fixture.
    static void SetRuleCacheValidationEnabledForTest(bool enabled);

    // Restore the env-var fallback after a SetRuleCacheValidationEnabledForTest
    // override. Calling this in test teardown prevents the override from
    // forcing the validator state for unrelated tests in the same binary.
    static void ClearRuleCacheValidationOverrideForTest();

    // Total number of cache-vs-live divergences observed since process
    // start (or since ResetRuleCacheValidationDivergenceCount was last
    // called). Exposed so tests can assert "no divergences fired during
    // a sequence of mutations". Each divergence is also logged as a
    // warning at the time it's observed.
    static uint64_t GetRuleCacheValidationDivergenceCount();
    static void ResetRuleCacheValidationDivergenceCount();
#endif
};

}} // namespace GameEngine::UIParsing

