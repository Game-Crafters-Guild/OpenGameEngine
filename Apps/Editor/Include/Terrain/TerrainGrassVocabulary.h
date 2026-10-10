#pragma once

#include <string>

namespace GameEngine::Editor::TerrainGrassVocabulary
{

// What the grass Render Mode row says about itself, and what it has to admit when the
// draw the row describes is not the draw that runs.
//
// Free of UI types on purpose, for the same reason TerrainRuleVocabulary is: every string
// here is authoring copy a test pins by phrase, and a widget that had to be laid out to
// read its own labels would put that copy out of reach — EditorTests runs no layout pass.
//
// `needsAlpha` is TerrainECS::TerrainGrassNeedsAlpha for this grass: whether anything it
// draws carries live alpha. Both functions below take that one answer and nothing else, so
// the note and the tooltip can never disagree about which state the row is in.

// Tooltip for the Render Mode dropdown: what each mode does, plus — when nothing carries
// alpha — the sentence saying the choice currently resolves nothing.
std::string RenderModeTooltip(bool needsAlpha);

// Why the Render Mode row writes nothing, or empty when it writes.
//
// State, then the fix, then the caveat: a reader who wants the row live needs the action
// ahead of the scoping rule that explains why the control was not simply disabled.
//
// The closing sentence is deliberately scoped to EVERY ACTIVE TERRAIN, not to this one: the
// alpha reduction runs once per params upload over every active terrain rather than per
// view, so this row's choice still counts while any other terrain needs the alpha path —
// including one no view can currently see. Said in a note rather than by disabling the
// control, because a disabled control would claim the opposite.
std::string RenderModeInactiveNote(bool needsAlpha);

// Tooltip for the Range row. Range is a CEILING, not a promise: the per-view instance budget
// shortens it when the authored density cannot be carried that far, so the word "hard" has to go.
std::string RangeTooltip();

// What the Range row has to admit when the budget fit shortened it, or empty when it did not.
//
// `authoredRange` and `deliveredRange` in metres. Empty when they agree, so the row stays quiet in
// the case where the authored number is the whole truth.
//
// `textureCards` selects which density the note names, and it must be the SAME choice the placement
// compute makes (`textureGrass ? GrassTextureCardsPerSquareMeter : GrassDensity`): a card terrain is
// fitted on cards/m2, so a note telling its author to lower a blade density names a dial that had
// nothing to do with the shortening.
//
// Scoped to THIS terrain's authored values. The runtime fits the maximum density and range across
// every active grass terrain at once, so a scene with several disagreeing terrains can deliver a
// shorter range than this row predicts; it can never deliver a longer one.
std::string RangeFitNote(float authoredRange, float deliveredRange, bool textureCards);

} // namespace GameEngine::Editor::TerrainGrassVocabulary
