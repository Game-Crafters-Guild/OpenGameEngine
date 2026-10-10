// Flexbox parity vs Chrome — main-axis distribution and cross-axis alignment
// of a single flex line: flex-direction row and column, all six
// justify-content values, all four align-items values, unequal item sizes, and
// a four-item fixture with a different margin on every edge.
//
// Every number below is a real Chrome measurement, not a derivation. The
// fixture is authored once and rendered into two byte-comparable arms (same
// selectors, same declarations, same order); the engine arm is the XML+CSS in
// this file, the Chrome arm the identical CSS inside a plain HTML document.
//
// Chrome arm, per fixture and per device scale factor:
//   "C:/Program Files/Google/Chrome/Application/chrome.exe"
//       --headless=new --disable-gpu --force-device-scale-factor=<1|2>
//       --run-all-compositor-stages-before-draw --virtual-time-budget=3000
//       --user-data-dir=<fresh> --no-first-run --dump-dom file:///<fixture>.html
// The page's own script writes getBoundingClientRect for every element into a
// #RESULT node; a screenshot is not a layout measurement. Chrome 141.
//
// Coordinates are child-relative-to-its-own-container, so where the container
// happens to sit is not part of the comparison. Sizes are chosen so free space
// does not divide evenly: 100px of row free space across 3 items puts
// space-around on 100/6 and space-evenly on 25, which no rounding can confuse.
// Every flex-relevant longhand is stated explicitly in the CSS, because Yoga's
// native defaults differ from CSS (column vs row, shrink 0 vs 1) and a default
// that only one arm papers over would contaminate the comparison.
//
// The engine has no box-sizing property — Yoga's width is always the border
// box — so the Chrome arm sets box-sizing: border-box on every element.
//
//
// WHAT THE ENGINE DOES DIFFERENTLY, AND HOW FAR
//
// Both arms quantise on the same quantum. Yoga rounds every solved edge onto
// the grid of the config its nodes were built with (YogaLayout.cpp,
// YogaAdapter::CreateNode -> SharedConfig), and YogaAdapter::SetContentScale
// puts that grid at 1/64 of a DEVICE pixel — Chrome's LayoutUnit. So the two
// can only disagree where they round the same exact length to different sides
// of one step, and that is what they do: 34 of the 1776 compared
// (field, scale) pairs sit exactly one step apart, always with the engine on
// the high side. Two mechanisms, both on show in the row space-around fixture:
//
//   - Yoga rounds half-up, Chrome truncates toward zero. 100/6 CSS px is
//     1066.67 steps at dsf 1; Yoga emits 1067 (16.671875), Chrome 1066
//     (16.65625).
//   - Chrome quantises the leading offset and the between-gap BEFORE
//     accumulating them, so its running offset can fall below the exact value.
//     Item 1 of that fixture sits at exactly 80 CSS px; Chrome reports
//     79.984375, having added a floored 16.65625 to a floored 33.328125. The
//     engine's 80 is the arithmetically correct answer — this divergence is
//     Chrome's, not the engine's.
//
// The other 1742 pairs are byte-identical to Chrome, including every fractional
// value that lands on the grid exactly: 14.125, 20.125, 27.5, 38.5, 179.625.
// So the expectation below is Chrome's own number, with the 34 off-by-one-step
// fields enumerated in kEngineOneStepAbove, and every field additionally held
// under a hard one-step bound that no distribution-algorithm error fits inside
// (a free space split n instead of n+1 ways moves items by whole pixels — 64
// steps and up).
//
// The grid is defined in DEVICE px, so content scale buys real layout
// precision: at scale 1 the engine addresses 1/64 CSS px, at scale 2 it
// addresses 1/128. That also means the solve is NOT content-scale invariant any
// more — the same length can round to different sides on the two grids. See
// ContentScaleRefinesTheGridRatherThanRescalingTheSolve, which bounds the two
// arms to one step of each other and enumerates where they part.
//
// TWO DEVICE SCALE FACTORS ARE NOT ONE MEASUREMENT. Because the grid is 1/64 of
// a DEVICE pixel, at dsf 2 Chrome resolves 1/128 CSS px and the non-terminating
// distributions land one notch differently. kChromeDsf2Exceptions carries
// exactly those fields; every other field is identical at both factors. Each
// arm is compared against its own column — there is no longer a single
// quantisation base for both.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

enum class Field
{
    X,
    Y,
    W,
    H
};

// One element's rect in CSS px, relative to its own flex container.
// Child == kContainerRow marks the container's own row, whose X/Y are 0 by
// construction and whose W/H are the specimen guard: if the container is not
// the size the fixture asked for, no child number below means anything.
struct ChromeRect
{
    const char* Container;
    int Child;
    float X;
    float Y;
    float W;
    float H;
};

struct ChromeDsf2Exception
{
    const char* Container;
    int Child;
    Field Which;
    float Value;
};

constexpr int kContainerRow = -1;

constexpr char kRowXml[] = R"(<uielement id="root">
  <uielement id="row_fs_fs" class="box jc-fs ai-fs">
    <uielement id="row_fs_fs__0" class="rowi0"/>
    <uielement id="row_fs_fs__1" class="rowi1"/>
    <uielement id="row_fs_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fs_ce" class="box jc-fs ai-ce">
    <uielement id="row_fs_ce__0" class="rowi0"/>
    <uielement id="row_fs_ce__1" class="rowi1"/>
    <uielement id="row_fs_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fs_fe" class="box jc-fs ai-fe">
    <uielement id="row_fs_fe__0" class="rowi0"/>
    <uielement id="row_fs_fe__1" class="rowi1"/>
    <uielement id="row_fs_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fs_st" class="box jc-fs ai-st">
    <uielement id="row_fs_st__0" class="rows0"/>
    <uielement id="row_fs_st__1" class="rows1"/>
    <uielement id="row_fs_st__2" class="rows2"/>
  </uielement>
  <uielement id="row_ce_fs" class="box jc-ce ai-fs">
    <uielement id="row_ce_fs__0" class="rowi0"/>
    <uielement id="row_ce_fs__1" class="rowi1"/>
    <uielement id="row_ce_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_ce_ce" class="box jc-ce ai-ce">
    <uielement id="row_ce_ce__0" class="rowi0"/>
    <uielement id="row_ce_ce__1" class="rowi1"/>
    <uielement id="row_ce_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_ce_fe" class="box jc-ce ai-fe">
    <uielement id="row_ce_fe__0" class="rowi0"/>
    <uielement id="row_ce_fe__1" class="rowi1"/>
    <uielement id="row_ce_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_ce_st" class="box jc-ce ai-st">
    <uielement id="row_ce_st__0" class="rows0"/>
    <uielement id="row_ce_st__1" class="rows1"/>
    <uielement id="row_ce_st__2" class="rows2"/>
  </uielement>
  <uielement id="row_fe_fs" class="box jc-fe ai-fs">
    <uielement id="row_fe_fs__0" class="rowi0"/>
    <uielement id="row_fe_fs__1" class="rowi1"/>
    <uielement id="row_fe_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fe_ce" class="box jc-fe ai-ce">
    <uielement id="row_fe_ce__0" class="rowi0"/>
    <uielement id="row_fe_ce__1" class="rowi1"/>
    <uielement id="row_fe_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fe_fe" class="box jc-fe ai-fe">
    <uielement id="row_fe_fe__0" class="rowi0"/>
    <uielement id="row_fe_fe__1" class="rowi1"/>
    <uielement id="row_fe_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_fe_st" class="box jc-fe ai-st">
    <uielement id="row_fe_st__0" class="rows0"/>
    <uielement id="row_fe_st__1" class="rows1"/>
    <uielement id="row_fe_st__2" class="rows2"/>
  </uielement>
  <uielement id="row_sb_fs" class="box jc-sb ai-fs">
    <uielement id="row_sb_fs__0" class="rowi0"/>
    <uielement id="row_sb_fs__1" class="rowi1"/>
    <uielement id="row_sb_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sb_ce" class="box jc-sb ai-ce">
    <uielement id="row_sb_ce__0" class="rowi0"/>
    <uielement id="row_sb_ce__1" class="rowi1"/>
    <uielement id="row_sb_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sb_fe" class="box jc-sb ai-fe">
    <uielement id="row_sb_fe__0" class="rowi0"/>
    <uielement id="row_sb_fe__1" class="rowi1"/>
    <uielement id="row_sb_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sb_st" class="box jc-sb ai-st">
    <uielement id="row_sb_st__0" class="rows0"/>
    <uielement id="row_sb_st__1" class="rows1"/>
    <uielement id="row_sb_st__2" class="rows2"/>
  </uielement>
  <uielement id="row_sa_fs" class="box jc-sa ai-fs">
    <uielement id="row_sa_fs__0" class="rowi0"/>
    <uielement id="row_sa_fs__1" class="rowi1"/>
    <uielement id="row_sa_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sa_ce" class="box jc-sa ai-ce">
    <uielement id="row_sa_ce__0" class="rowi0"/>
    <uielement id="row_sa_ce__1" class="rowi1"/>
    <uielement id="row_sa_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sa_fe" class="box jc-sa ai-fe">
    <uielement id="row_sa_fe__0" class="rowi0"/>
    <uielement id="row_sa_fe__1" class="rowi1"/>
    <uielement id="row_sa_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_sa_st" class="box jc-sa ai-st">
    <uielement id="row_sa_st__0" class="rows0"/>
    <uielement id="row_sa_st__1" class="rows1"/>
    <uielement id="row_sa_st__2" class="rows2"/>
  </uielement>
  <uielement id="row_se_fs" class="box jc-se ai-fs">
    <uielement id="row_se_fs__0" class="rowi0"/>
    <uielement id="row_se_fs__1" class="rowi1"/>
    <uielement id="row_se_fs__2" class="rowi2"/>
  </uielement>
  <uielement id="row_se_ce" class="box jc-se ai-ce">
    <uielement id="row_se_ce__0" class="rowi0"/>
    <uielement id="row_se_ce__1" class="rowi1"/>
    <uielement id="row_se_ce__2" class="rowi2"/>
  </uielement>
  <uielement id="row_se_fe" class="box jc-se ai-fe">
    <uielement id="row_se_fe__0" class="rowi0"/>
    <uielement id="row_se_fe__1" class="rowi1"/>
    <uielement id="row_se_fe__2" class="rowi2"/>
  </uielement>
  <uielement id="row_se_st" class="box jc-se ai-st">
    <uielement id="row_se_st__0" class="rows0"/>
    <uielement id="row_se_st__1" class="rows1"/>
    <uielement id="row_se_st__2" class="rows2"/>
  </uielement>
</uielement>)";

constexpr char kRowCss[] = R"(#root { display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; width: 800px; height: 600px; padding: 0px; margin: 0px; }
.box { display: flex; flex-direction: row; flex-wrap: nowrap; align-content: flex-start; width: 200px; height: 100px; padding: 0px; margin: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.jc-fs { justify-content: flex-start; }
.jc-ce { justify-content: center; }
.jc-fe { justify-content: flex-end; }
.jc-sb { justify-content: space-between; }
.jc-sa { justify-content: space-around; }
.jc-se { justify-content: space-evenly; }
.ai-fs { align-items: flex-start; }
.ai-ce { align-items: center; }
.ai-fe { align-items: flex-end; }
.ai-st { align-items: stretch; }
.rowi0 { width: 30px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.rowi1 { width: 50px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.rowi2 { width: 20px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.rows0 { width: 30px; height: auto; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.rows1 { width: 50px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.rows2 { width: 20px; height: auto; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; })";

constexpr char kColXml[] = R"(<uielement id="root">
  <uielement id="col_fs_fs" class="box jc-fs ai-fs">
    <uielement id="col_fs_fs__0" class="coli0"/>
    <uielement id="col_fs_fs__1" class="coli1"/>
    <uielement id="col_fs_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_fs_ce" class="box jc-fs ai-ce">
    <uielement id="col_fs_ce__0" class="coli0"/>
    <uielement id="col_fs_ce__1" class="coli1"/>
    <uielement id="col_fs_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_fs_fe" class="box jc-fs ai-fe">
    <uielement id="col_fs_fe__0" class="coli0"/>
    <uielement id="col_fs_fe__1" class="coli1"/>
    <uielement id="col_fs_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_fs_st" class="box jc-fs ai-st">
    <uielement id="col_fs_st__0" class="cols0"/>
    <uielement id="col_fs_st__1" class="cols1"/>
    <uielement id="col_fs_st__2" class="cols2"/>
  </uielement>
  <uielement id="col_ce_fs" class="box jc-ce ai-fs">
    <uielement id="col_ce_fs__0" class="coli0"/>
    <uielement id="col_ce_fs__1" class="coli1"/>
    <uielement id="col_ce_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_ce_ce" class="box jc-ce ai-ce">
    <uielement id="col_ce_ce__0" class="coli0"/>
    <uielement id="col_ce_ce__1" class="coli1"/>
    <uielement id="col_ce_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_ce_fe" class="box jc-ce ai-fe">
    <uielement id="col_ce_fe__0" class="coli0"/>
    <uielement id="col_ce_fe__1" class="coli1"/>
    <uielement id="col_ce_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_ce_st" class="box jc-ce ai-st">
    <uielement id="col_ce_st__0" class="cols0"/>
    <uielement id="col_ce_st__1" class="cols1"/>
    <uielement id="col_ce_st__2" class="cols2"/>
  </uielement>
  <uielement id="col_fe_fs" class="box jc-fe ai-fs">
    <uielement id="col_fe_fs__0" class="coli0"/>
    <uielement id="col_fe_fs__1" class="coli1"/>
    <uielement id="col_fe_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_fe_ce" class="box jc-fe ai-ce">
    <uielement id="col_fe_ce__0" class="coli0"/>
    <uielement id="col_fe_ce__1" class="coli1"/>
    <uielement id="col_fe_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_fe_fe" class="box jc-fe ai-fe">
    <uielement id="col_fe_fe__0" class="coli0"/>
    <uielement id="col_fe_fe__1" class="coli1"/>
    <uielement id="col_fe_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_fe_st" class="box jc-fe ai-st">
    <uielement id="col_fe_st__0" class="cols0"/>
    <uielement id="col_fe_st__1" class="cols1"/>
    <uielement id="col_fe_st__2" class="cols2"/>
  </uielement>
  <uielement id="col_sb_fs" class="box jc-sb ai-fs">
    <uielement id="col_sb_fs__0" class="coli0"/>
    <uielement id="col_sb_fs__1" class="coli1"/>
    <uielement id="col_sb_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_sb_ce" class="box jc-sb ai-ce">
    <uielement id="col_sb_ce__0" class="coli0"/>
    <uielement id="col_sb_ce__1" class="coli1"/>
    <uielement id="col_sb_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_sb_fe" class="box jc-sb ai-fe">
    <uielement id="col_sb_fe__0" class="coli0"/>
    <uielement id="col_sb_fe__1" class="coli1"/>
    <uielement id="col_sb_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_sb_st" class="box jc-sb ai-st">
    <uielement id="col_sb_st__0" class="cols0"/>
    <uielement id="col_sb_st__1" class="cols1"/>
    <uielement id="col_sb_st__2" class="cols2"/>
  </uielement>
  <uielement id="col_sa_fs" class="box jc-sa ai-fs">
    <uielement id="col_sa_fs__0" class="coli0"/>
    <uielement id="col_sa_fs__1" class="coli1"/>
    <uielement id="col_sa_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_sa_ce" class="box jc-sa ai-ce">
    <uielement id="col_sa_ce__0" class="coli0"/>
    <uielement id="col_sa_ce__1" class="coli1"/>
    <uielement id="col_sa_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_sa_fe" class="box jc-sa ai-fe">
    <uielement id="col_sa_fe__0" class="coli0"/>
    <uielement id="col_sa_fe__1" class="coli1"/>
    <uielement id="col_sa_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_sa_st" class="box jc-sa ai-st">
    <uielement id="col_sa_st__0" class="cols0"/>
    <uielement id="col_sa_st__1" class="cols1"/>
    <uielement id="col_sa_st__2" class="cols2"/>
  </uielement>
  <uielement id="col_se_fs" class="box jc-se ai-fs">
    <uielement id="col_se_fs__0" class="coli0"/>
    <uielement id="col_se_fs__1" class="coli1"/>
    <uielement id="col_se_fs__2" class="coli2"/>
  </uielement>
  <uielement id="col_se_ce" class="box jc-se ai-ce">
    <uielement id="col_se_ce__0" class="coli0"/>
    <uielement id="col_se_ce__1" class="coli1"/>
    <uielement id="col_se_ce__2" class="coli2"/>
  </uielement>
  <uielement id="col_se_fe" class="box jc-se ai-fe">
    <uielement id="col_se_fe__0" class="coli0"/>
    <uielement id="col_se_fe__1" class="coli1"/>
    <uielement id="col_se_fe__2" class="coli2"/>
  </uielement>
  <uielement id="col_se_st" class="box jc-se ai-st">
    <uielement id="col_se_st__0" class="cols0"/>
    <uielement id="col_se_st__1" class="cols1"/>
    <uielement id="col_se_st__2" class="cols2"/>
  </uielement>
</uielement>)";

constexpr char kColCss[] = R"(#root { display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; width: 800px; height: 600px; padding: 0px; margin: 0px; }
.box { display: flex; flex-direction: column; flex-wrap: nowrap; align-content: flex-start; width: 100px; height: 200px; padding: 0px; margin: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.jc-fs { justify-content: flex-start; }
.jc-ce { justify-content: center; }
.jc-fe { justify-content: flex-end; }
.jc-sb { justify-content: space-between; }
.jc-sa { justify-content: space-around; }
.jc-se { justify-content: space-evenly; }
.ai-fs { align-items: flex-start; }
.ai-ce { align-items: center; }
.ai-fe { align-items: flex-end; }
.ai-st { align-items: stretch; }
.coli0 { width: 20px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.coli1 { width: 40px; height: 50px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.coli2 { width: 30px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.cols0 { width: auto; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.cols1 { width: 40px; height: 50px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; }
.cols2 { width: auto; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; margin: 0px; padding: 0px; })";

constexpr char kMarginXml[] = R"(<uielement id="root">
  <uielement id="mrow_sb_fe" class="mrow">
    <uielement id="mrow_sb_fe__0" class="mi0"/>
    <uielement id="mrow_sb_fe__1" class="mi1"/>
    <uielement id="mrow_sb_fe__2" class="mi2"/>
    <uielement id="mrow_sb_fe__3" class="mi3"/>
  </uielement>
  <uielement id="mrow_sa_ce" class="mrow">
    <uielement id="mrow_sa_ce__0" class="mi0"/>
    <uielement id="mrow_sa_ce__1" class="mi1"/>
    <uielement id="mrow_sa_ce__2" class="mi2"/>
    <uielement id="mrow_sa_ce__3" class="mi3"/>
  </uielement>
  <uielement id="mrow_se_fs" class="mrow">
    <uielement id="mrow_se_fs__0" class="mi0"/>
    <uielement id="mrow_se_fs__1" class="mi1"/>
    <uielement id="mrow_se_fs__2" class="mi2"/>
    <uielement id="mrow_se_fs__3" class="mi3"/>
  </uielement>
  <uielement id="mcol_sb_fe" class="mcol">
    <uielement id="mcol_sb_fe__0" class="mi0"/>
    <uielement id="mcol_sb_fe__1" class="mi1"/>
    <uielement id="mcol_sb_fe__2" class="mi2"/>
    <uielement id="mcol_sb_fe__3" class="mi3"/>
  </uielement>
  <uielement id="mcol_sa_ce" class="mcol">
    <uielement id="mcol_sa_ce__0" class="mi0"/>
    <uielement id="mcol_sa_ce__1" class="mi1"/>
    <uielement id="mcol_sa_ce__2" class="mi2"/>
    <uielement id="mcol_sa_ce__3" class="mi3"/>
  </uielement>
  <uielement id="mcol_se_fs" class="mcol">
    <uielement id="mcol_se_fs__0" class="mi0"/>
    <uielement id="mcol_se_fs__1" class="mi1"/>
    <uielement id="mcol_se_fs__2" class="mi2"/>
    <uielement id="mcol_se_fs__3" class="mi3"/>
  </uielement>
</uielement>)";

constexpr char kMarginCss[] = R"(#root { display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; width: 800px; height: 600px; padding: 0px; margin: 0px; }
.mrow { display: flex; flex-direction: row; flex-wrap: nowrap; align-content: flex-start; width: 300px; height: 100px; padding: 0px; margin: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.mcol { display: flex; flex-direction: column; flex-wrap: nowrap; align-content: flex-start; width: 100px; height: 300px; padding: 0px; margin: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#mrow_sb_fe { justify-content: space-between; align-items: flex-end; }
#mrow_sa_ce { justify-content: space-around; align-items: center; }
#mrow_se_fs { justify-content: space-evenly; align-items: flex-start; }
#mcol_sb_fe { justify-content: space-between; align-items: flex-end; }
#mcol_sa_ce { justify-content: space-around; align-items: center; }
#mcol_se_fs { justify-content: space-evenly; align-items: flex-start; }
.mi0 { width: 40px; height: 20px; margin: 0px 10px 0px 5px; padding: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.mi1 { width: 60px; height: 30px; margin: 8px 4px 0px 12px; padding: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.mi2 { width: 30px; height: 40px; margin: 0px 6px 14px 3px; padding: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
.mi3 { width: 50px; height: 25px; margin: 2px 0px 0px 7px; padding: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; })";

constexpr ChromeRect kChromeRow[] = {
    {"row_fs_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fs_fs", 0, 0.0f, 0.0f, 30.0f, 20.0f},
    {"row_fs_fs", 1, 30.0f, 0.0f, 50.0f, 40.0f},
    {"row_fs_fs", 2, 80.0f, 0.0f, 20.0f, 30.0f},
    {"row_fs_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fs_ce", 0, 0.0f, 40.0f, 30.0f, 20.0f},
    {"row_fs_ce", 1, 30.0f, 30.0f, 50.0f, 40.0f},
    {"row_fs_ce", 2, 80.0f, 35.0f, 20.0f, 30.0f},
    {"row_fs_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fs_fe", 0, 0.0f, 80.0f, 30.0f, 20.0f},
    {"row_fs_fe", 1, 30.0f, 60.0f, 50.0f, 40.0f},
    {"row_fs_fe", 2, 80.0f, 70.0f, 20.0f, 30.0f},
    {"row_fs_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fs_st", 0, 0.0f, 0.0f, 30.0f, 100.0f},
    {"row_fs_st", 1, 30.0f, 0.0f, 50.0f, 40.0f},
    {"row_fs_st", 2, 80.0f, 0.0f, 20.0f, 100.0f},
    {"row_ce_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_ce_fs", 0, 50.0f, 0.0f, 30.0f, 20.0f},
    {"row_ce_fs", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_ce_fs", 2, 130.0f, 0.0f, 20.0f, 30.0f},
    {"row_ce_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_ce_ce", 0, 50.0f, 40.0f, 30.0f, 20.0f},
    {"row_ce_ce", 1, 80.0f, 30.0f, 50.0f, 40.0f},
    {"row_ce_ce", 2, 130.0f, 35.0f, 20.0f, 30.0f},
    {"row_ce_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_ce_fe", 0, 50.0f, 80.0f, 30.0f, 20.0f},
    {"row_ce_fe", 1, 80.0f, 60.0f, 50.0f, 40.0f},
    {"row_ce_fe", 2, 130.0f, 70.0f, 20.0f, 30.0f},
    {"row_ce_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_ce_st", 0, 50.0f, 0.0f, 30.0f, 100.0f},
    {"row_ce_st", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_ce_st", 2, 130.0f, 0.0f, 20.0f, 100.0f},
    {"row_fe_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fe_fs", 0, 100.0f, 0.0f, 30.0f, 20.0f},
    {"row_fe_fs", 1, 130.0f, 0.0f, 50.0f, 40.0f},
    {"row_fe_fs", 2, 180.0f, 0.0f, 20.0f, 30.0f},
    {"row_fe_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fe_ce", 0, 100.0f, 40.0f, 30.0f, 20.0f},
    {"row_fe_ce", 1, 130.0f, 30.0f, 50.0f, 40.0f},
    {"row_fe_ce", 2, 180.0f, 35.0f, 20.0f, 30.0f},
    {"row_fe_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fe_fe", 0, 100.0f, 80.0f, 30.0f, 20.0f},
    {"row_fe_fe", 1, 130.0f, 60.0f, 50.0f, 40.0f},
    {"row_fe_fe", 2, 180.0f, 70.0f, 20.0f, 30.0f},
    {"row_fe_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_fe_st", 0, 100.0f, 0.0f, 30.0f, 100.0f},
    {"row_fe_st", 1, 130.0f, 0.0f, 50.0f, 40.0f},
    {"row_fe_st", 2, 180.0f, 0.0f, 20.0f, 100.0f},
    {"row_sb_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sb_fs", 0, 0.0f, 0.0f, 30.0f, 20.0f},
    {"row_sb_fs", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_sb_fs", 2, 180.0f, 0.0f, 20.0f, 30.0f},
    {"row_sb_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sb_ce", 0, 0.0f, 40.0f, 30.0f, 20.0f},
    {"row_sb_ce", 1, 80.0f, 30.0f, 50.0f, 40.0f},
    {"row_sb_ce", 2, 180.0f, 35.0f, 20.0f, 30.0f},
    {"row_sb_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sb_fe", 0, 0.0f, 80.0f, 30.0f, 20.0f},
    {"row_sb_fe", 1, 80.0f, 60.0f, 50.0f, 40.0f},
    {"row_sb_fe", 2, 180.0f, 70.0f, 20.0f, 30.0f},
    {"row_sb_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sb_st", 0, 0.0f, 0.0f, 30.0f, 100.0f},
    {"row_sb_st", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_sb_st", 2, 180.0f, 0.0f, 20.0f, 100.0f},
    {"row_sa_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sa_fs", 0, 16.65625f, 0.0f, 30.0f, 20.0f},
    {"row_sa_fs", 1, 79.984375f, 0.0f, 50.0f, 40.0f},
    {"row_sa_fs", 2, 163.328125f, 0.0f, 20.0f, 30.0f},
    {"row_sa_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sa_ce", 0, 16.65625f, 40.0f, 30.0f, 20.0f},
    {"row_sa_ce", 1, 79.984375f, 30.0f, 50.0f, 40.0f},
    {"row_sa_ce", 2, 163.328125f, 35.0f, 20.0f, 30.0f},
    {"row_sa_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sa_fe", 0, 16.65625f, 80.0f, 30.0f, 20.0f},
    {"row_sa_fe", 1, 79.984375f, 60.0f, 50.0f, 40.0f},
    {"row_sa_fe", 2, 163.328125f, 70.0f, 20.0f, 30.0f},
    {"row_sa_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_sa_st", 0, 16.65625f, 0.0f, 30.0f, 100.0f},
    {"row_sa_st", 1, 79.984375f, 0.0f, 50.0f, 40.0f},
    {"row_sa_st", 2, 163.328125f, 0.0f, 20.0f, 100.0f},
    {"row_se_fs", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_se_fs", 0, 25.0f, 0.0f, 30.0f, 20.0f},
    {"row_se_fs", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_se_fs", 2, 155.0f, 0.0f, 20.0f, 30.0f},
    {"row_se_ce", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_se_ce", 0, 25.0f, 40.0f, 30.0f, 20.0f},
    {"row_se_ce", 1, 80.0f, 30.0f, 50.0f, 40.0f},
    {"row_se_ce", 2, 155.0f, 35.0f, 20.0f, 30.0f},
    {"row_se_fe", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_se_fe", 0, 25.0f, 80.0f, 30.0f, 20.0f},
    {"row_se_fe", 1, 80.0f, 60.0f, 50.0f, 40.0f},
    {"row_se_fe", 2, 155.0f, 70.0f, 20.0f, 30.0f},
    {"row_se_st", -1, 0.0f, 0.0f, 200.0f, 100.0f},
    {"row_se_st", 0, 25.0f, 0.0f, 30.0f, 100.0f},
    {"row_se_st", 1, 80.0f, 0.0f, 50.0f, 40.0f},
    {"row_se_st", 2, 155.0f, 0.0f, 20.0f, 100.0f},
};

constexpr ChromeRect kChromeCol[] = {
    {"col_fs_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fs_fs", 0, 0.0f, 0.0f, 20.0f, 30.0f},
    {"col_fs_fs", 1, 0.0f, 30.0f, 40.0f, 50.0f},
    {"col_fs_fs", 2, 0.0f, 80.0f, 30.0f, 20.0f},
    {"col_fs_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fs_ce", 0, 40.0f, 0.0f, 20.0f, 30.0f},
    {"col_fs_ce", 1, 30.0f, 30.0f, 40.0f, 50.0f},
    {"col_fs_ce", 2, 35.0f, 80.0f, 30.0f, 20.0f},
    {"col_fs_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fs_fe", 0, 80.0f, 0.0f, 20.0f, 30.0f},
    {"col_fs_fe", 1, 60.0f, 30.0f, 40.0f, 50.0f},
    {"col_fs_fe", 2, 70.0f, 80.0f, 30.0f, 20.0f},
    {"col_fs_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fs_st", 0, 0.0f, 0.0f, 100.0f, 30.0f},
    {"col_fs_st", 1, 0.0f, 30.0f, 40.0f, 50.0f},
    {"col_fs_st", 2, 0.0f, 80.0f, 100.0f, 20.0f},
    {"col_ce_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_ce_fs", 0, 0.0f, 50.0f, 20.0f, 30.0f},
    {"col_ce_fs", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_ce_fs", 2, 0.0f, 130.0f, 30.0f, 20.0f},
    {"col_ce_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_ce_ce", 0, 40.0f, 50.0f, 20.0f, 30.0f},
    {"col_ce_ce", 1, 30.0f, 80.0f, 40.0f, 50.0f},
    {"col_ce_ce", 2, 35.0f, 130.0f, 30.0f, 20.0f},
    {"col_ce_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_ce_fe", 0, 80.0f, 50.0f, 20.0f, 30.0f},
    {"col_ce_fe", 1, 60.0f, 80.0f, 40.0f, 50.0f},
    {"col_ce_fe", 2, 70.0f, 130.0f, 30.0f, 20.0f},
    {"col_ce_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_ce_st", 0, 0.0f, 50.0f, 100.0f, 30.0f},
    {"col_ce_st", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_ce_st", 2, 0.0f, 130.0f, 100.0f, 20.0f},
    {"col_fe_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fe_fs", 0, 0.0f, 100.0f, 20.0f, 30.0f},
    {"col_fe_fs", 1, 0.0f, 130.0f, 40.0f, 50.0f},
    {"col_fe_fs", 2, 0.0f, 180.0f, 30.0f, 20.0f},
    {"col_fe_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fe_ce", 0, 40.0f, 100.0f, 20.0f, 30.0f},
    {"col_fe_ce", 1, 30.0f, 130.0f, 40.0f, 50.0f},
    {"col_fe_ce", 2, 35.0f, 180.0f, 30.0f, 20.0f},
    {"col_fe_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fe_fe", 0, 80.0f, 100.0f, 20.0f, 30.0f},
    {"col_fe_fe", 1, 60.0f, 130.0f, 40.0f, 50.0f},
    {"col_fe_fe", 2, 70.0f, 180.0f, 30.0f, 20.0f},
    {"col_fe_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_fe_st", 0, 0.0f, 100.0f, 100.0f, 30.0f},
    {"col_fe_st", 1, 0.0f, 130.0f, 40.0f, 50.0f},
    {"col_fe_st", 2, 0.0f, 180.0f, 100.0f, 20.0f},
    {"col_sb_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sb_fs", 0, 0.0f, 0.0f, 20.0f, 30.0f},
    {"col_sb_fs", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_sb_fs", 2, 0.0f, 180.0f, 30.0f, 20.0f},
    {"col_sb_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sb_ce", 0, 40.0f, 0.0f, 20.0f, 30.0f},
    {"col_sb_ce", 1, 30.0f, 80.0f, 40.0f, 50.0f},
    {"col_sb_ce", 2, 35.0f, 180.0f, 30.0f, 20.0f},
    {"col_sb_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sb_fe", 0, 80.0f, 0.0f, 20.0f, 30.0f},
    {"col_sb_fe", 1, 60.0f, 80.0f, 40.0f, 50.0f},
    {"col_sb_fe", 2, 70.0f, 180.0f, 30.0f, 20.0f},
    {"col_sb_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sb_st", 0, 0.0f, 0.0f, 100.0f, 30.0f},
    {"col_sb_st", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_sb_st", 2, 0.0f, 180.0f, 100.0f, 20.0f},
    {"col_sa_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sa_fs", 0, 0.0f, 16.65625f, 20.0f, 30.0f},
    {"col_sa_fs", 1, 0.0f, 79.984375f, 40.0f, 50.0f},
    {"col_sa_fs", 2, 0.0f, 163.328125f, 30.0f, 20.0f},
    {"col_sa_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sa_ce", 0, 40.0f, 16.65625f, 20.0f, 30.0f},
    {"col_sa_ce", 1, 30.0f, 79.984375f, 40.0f, 50.0f},
    {"col_sa_ce", 2, 35.0f, 163.328125f, 30.0f, 20.0f},
    {"col_sa_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sa_fe", 0, 80.0f, 16.65625f, 20.0f, 30.0f},
    {"col_sa_fe", 1, 60.0f, 79.984375f, 40.0f, 50.0f},
    {"col_sa_fe", 2, 70.0f, 163.328125f, 30.0f, 20.0f},
    {"col_sa_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_sa_st", 0, 0.0f, 16.65625f, 100.0f, 30.0f},
    {"col_sa_st", 1, 0.0f, 79.984375f, 40.0f, 50.0f},
    {"col_sa_st", 2, 0.0f, 163.328125f, 100.0f, 20.0f},
    {"col_se_fs", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_se_fs", 0, 0.0f, 25.0f, 20.0f, 30.0f},
    {"col_se_fs", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_se_fs", 2, 0.0f, 155.0f, 30.0f, 20.0f},
    {"col_se_ce", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_se_ce", 0, 40.0f, 25.0f, 20.0f, 30.0f},
    {"col_se_ce", 1, 30.0f, 80.0f, 40.0f, 50.0f},
    {"col_se_ce", 2, 35.0f, 155.0f, 30.0f, 20.0f},
    {"col_se_fe", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_se_fe", 0, 80.0f, 25.0f, 20.0f, 30.0f},
    {"col_se_fe", 1, 60.0f, 80.0f, 40.0f, 50.0f},
    {"col_se_fe", 2, 70.0f, 155.0f, 30.0f, 20.0f},
    {"col_se_st", -1, 0.0f, 0.0f, 100.0f, 200.0f},
    {"col_se_st", 0, 0.0f, 25.0f, 100.0f, 30.0f},
    {"col_se_st", 1, 0.0f, 80.0f, 40.0f, 50.0f},
    {"col_se_st", 2, 0.0f, 155.0f, 100.0f, 20.0f},
};

constexpr ChromeRect kChromeMargin[] = {
    {"mrow_sb_fe", -1, 0.0f, 0.0f, 300.0f, 100.0f},
    {"mrow_sb_fe", 0, 5.0f, 80.0f, 40.0f, 20.0f},
    {"mrow_sb_fe", 1, 91.328125f, 70.0f, 60.0f, 30.0f},
    {"mrow_sb_fe", 2, 182.671875f, 46.0f, 30.0f, 40.0f},
    {"mrow_sb_fe", 3, 250.0f, 75.0f, 50.0f, 25.0f},
    {"mrow_sa_ce", -1, 0.0f, 0.0f, 300.0f, 100.0f},
    {"mrow_sa_ce", 0, 14.125f, 40.0f, 40.0f, 20.0f},
    {"mrow_sa_ce", 1, 94.375f, 39.0f, 60.0f, 30.0f},
    {"mrow_sa_ce", 2, 179.625f, 23.0f, 30.0f, 40.0f},
    {"mrow_sa_ce", 3, 240.875f, 38.5f, 50.0f, 25.0f},
    {"mrow_se_fs", -1, 0.0f, 0.0f, 300.0f, 100.0f},
    {"mrow_se_fs", 0, 19.59375f, 0.0f, 40.0f, 20.0f},
    {"mrow_se_fs", 1, 96.1875f, 8.0f, 60.0f, 30.0f},
    {"mrow_se_fs", 2, 177.796875f, 0.0f, 30.0f, 40.0f},
    {"mrow_se_fs", 3, 235.390625f, 2.0f, 50.0f, 25.0f},
    {"mcol_sb_fe", -1, 0.0f, 0.0f, 100.0f, 300.0f},
    {"mcol_sb_fe", 0, 50.0f, 0.0f, 40.0f, 20.0f},
    {"mcol_sb_fe", 1, 36.0f, 81.671875f, 60.0f, 30.0f},
    {"mcol_sb_fe", 2, 64.0f, 165.328125f, 30.0f, 40.0f},
    {"mcol_sb_fe", 3, 50.0f, 275.0f, 50.0f, 25.0f},
    {"mcol_sa_ce", -1, 0.0f, 0.0f, 100.0f, 300.0f},
    {"mcol_sa_ce", 0, 27.5f, 20.125f, 40.0f, 20.0f},
    {"mcol_sa_ce", 1, 24.0f, 88.375f, 60.0f, 30.0f},
    {"mcol_sa_ce", 2, 33.5f, 158.625f, 30.0f, 40.0f},
    {"mcol_sa_ce", 3, 28.5f, 254.875f, 50.0f, 25.0f},
    {"mcol_se_fs", -1, 0.0f, 0.0f, 100.0f, 300.0f},
    {"mcol_se_fs", 0, 5.0f, 32.1875f, 40.0f, 20.0f},
    {"mcol_se_fs", 1, 12.0f, 92.390625f, 60.0f, 30.0f},
    {"mcol_se_fs", 2, 3.0f, 154.59375f, 30.0f, 40.0f},
    {"mcol_se_fs", 3, 7.0f, 242.78125f, 50.0f, 25.0f},
};

constexpr ChromeDsf2Exception kChromeDsf2Exceptions[] = {
    {"row_sa_fs", 0, Field::X, 16.6640625f},
    {"row_sa_fs", 1, Field::X, 80.0f},
    {"row_sa_ce", 0, Field::X, 16.6640625f},
    {"row_sa_ce", 1, Field::X, 80.0f},
    {"row_sa_fe", 0, Field::X, 16.6640625f},
    {"row_sa_fe", 1, Field::X, 80.0f},
    {"row_sa_st", 0, Field::X, 16.6640625f},
    {"row_sa_st", 1, Field::X, 80.0f},
    {"col_sa_fs", 0, Field::Y, 16.6640625f},
    {"col_sa_fs", 1, Field::Y, 80.0f},
    {"col_sa_ce", 0, Field::Y, 16.6640625f},
    {"col_sa_ce", 1, Field::Y, 80.0f},
    {"col_sa_fe", 0, Field::Y, 16.6640625f},
    {"col_sa_fe", 1, Field::Y, 80.0f},
    {"col_sa_st", 0, Field::Y, 16.6640625f},
    {"col_sa_st", 1, Field::Y, 80.0f},
    {"mrow_sb_fe", 1, Field::X, 91.3359375f},
    {"mrow_sb_fe", 2, Field::X, 182.6640625f},
    {"mrow_se_fs", 1, Field::X, 96.1953125f},
    {"mcol_sb_fe", 1, Field::Y, 81.6640625f},
    {"mcol_sb_fe", 2, Field::Y, 165.3359375f},
    {"mcol_se_fs", 0, Field::Y, 32.1953125f},
    {"mcol_se_fs", 1, Field::Y, 92.3984375f},
    {"mcol_se_fs", 3, Field::Y, 242.796875f},
};

// One step of the layout grid, in DEVICE px. YogaAdapter::SetContentScale gives
// the shared config a pointScaleFactor of 64 * contentScale, so the step is this
// same number at every content scale — which is why every comparison below is
// made in device px rather than CSS px.
constexpr float kGridStepDevicePx = 1.0f / 64.0f;

// Slack for a comparison that is otherwise exact. Every value in play is a
// dyadic rational well inside float's mantissa; this only absorbs the multiply
// by contentScale.
constexpr float kSlackDevicePx = 1.0f / 4096.0f;

// The (field, scale) pairs where the engine lands one grid step ABOVE Chrome.
// Nothing lands below, and nothing is further out than one step; the two
// mechanisms are in the header. An entry claims one scale only — the same field
// usually agrees with Chrome at the other scale, because the other grid puts a
// different fraction in front of the rounding rule.
struct OneStepAbove
{
    const char* Container;
    int Child;
    Field Which;
    float ContentScale;
};

constexpr OneStepAbove kEngineOneStepAbove[] = {
    // space-around, main axis. Child 0 is the leading 100/6 (1066.67 steps at
    // dsf 1, 2133.33 at dsf 2 — half-up disagrees with truncation only on the
    // first); child 2 is 490/3, which flips the other way between the grids;
    // child 1 is the exact 80 that Chrome's pre-accumulation flooring misses.
    {"row_sa_fs", 0, Field::X, 1.0f},
    {"row_sa_fs", 1, Field::X, 1.0f},
    {"row_sa_fs", 2, Field::X, 2.0f},
    {"row_sa_ce", 0, Field::X, 1.0f},
    {"row_sa_ce", 1, Field::X, 1.0f},
    {"row_sa_ce", 2, Field::X, 2.0f},
    {"row_sa_fe", 0, Field::X, 1.0f},
    {"row_sa_fe", 1, Field::X, 1.0f},
    {"row_sa_fe", 2, Field::X, 2.0f},
    {"row_sa_st", 0, Field::X, 1.0f},
    {"row_sa_st", 1, Field::X, 1.0f},
    {"row_sa_st", 2, Field::X, 2.0f},
    {"col_sa_fs", 0, Field::Y, 1.0f},
    {"col_sa_fs", 1, Field::Y, 1.0f},
    {"col_sa_fs", 2, Field::Y, 2.0f},
    {"col_sa_ce", 0, Field::Y, 1.0f},
    {"col_sa_ce", 1, Field::Y, 1.0f},
    {"col_sa_ce", 2, Field::Y, 2.0f},
    {"col_sa_fe", 0, Field::Y, 1.0f},
    {"col_sa_fe", 1, Field::Y, 1.0f},
    {"col_sa_fe", 2, Field::Y, 2.0f},
    {"col_sa_st", 0, Field::Y, 1.0f},
    {"col_sa_st", 1, Field::Y, 1.0f},
    {"col_sa_st", 2, Field::Y, 2.0f},
    // space-evenly over the margin fixture: 73/5 and 161/5 put a .4 or .8
    // fraction on the grid, so which items diverge changes with the scale.
    // space-between and space-around over the same fixture divide exactly and
    // are Chrome-identical at both scales — hence no mrow_sb_fe / mrow_sa_ce /
    // mcol_sb_fe / mcol_sa_ce entries here.
    {"mrow_se_fs", 1, Field::X, 1.0f},
    {"mrow_se_fs", 3, Field::X, 1.0f},
    {"mrow_se_fs", 0, Field::X, 2.0f},
    {"mrow_se_fs", 1, Field::X, 2.0f},
    {"mrow_se_fs", 3, Field::X, 2.0f},
    {"mcol_se_fs", 0, Field::Y, 1.0f},
    {"mcol_se_fs", 1, Field::Y, 1.0f},
    {"mcol_se_fs", 3, Field::Y, 1.0f},
    {"mcol_se_fs", 0, Field::Y, 2.0f},
    {"mcol_se_fs", 2, Field::Y, 2.0f},
};

float FieldOf(const ChromeRect& r, Field which)
{
    switch (which)
    {
    case Field::X:
        return r.X;
    case Field::Y:
        return r.Y;
    case Field::W:
        return r.W;
    case Field::H:
        return r.H;
    }
    return 0.0f;
}

// Chrome's value at this device scale factor: the dsf-1 column unless this
// exact field is one where Chrome's device-pixel snapping moved.
float ChromeAtScale(const ChromeRect& r, Field which, float contentScale)
{
    if (contentScale != 2.0f)
        return FieldOf(r, which);
    for (const ChromeDsf2Exception& e : kChromeDsf2Exceptions)
    {
        if (std::string(e.Container) == r.Container && e.Child == r.Child && e.Which == which)
            return e.Value;
    }
    return FieldOf(r, which);
}

bool EngineIsOneStepAbove(const ChromeRect& r, Field which, float contentScale)
{
    for (const OneStepAbove& e : kEngineOneStepAbove)
    {
        if (e.Child == r.Child && e.Which == which && e.ContentScale == contentScale &&
            std::string(e.Container) == r.Container)
            return true;
    }
    return false;
}

// What the engine emits for this field, in DEVICE px: Chrome's own number at
// this scale, plus one grid step on the enumerated fields.
float ExpectedDevicePx(const ChromeRect& r, Field which, float contentScale)
{
    const float chromeDevice = ChromeAtScale(r, which, contentScale) * contentScale;
    return EngineIsOneStepAbove(r, which, contentScale) ? chromeDevice + kGridStepDevicePx
                                                        : chromeDevice;
}

// True when the value sits on a whole grid step, which every solved edge must.
bool IsOnGrid(float devicePx)
{
    const float steps = devicePx / kGridStepDevicePx;
    return std::fabs(steps - std::round(steps)) < 1e-3f;
}

const char* FieldName(Field which)
{
    switch (which)
    {
    case Field::X:
        return "x";
    case Field::Y:
        return "y";
    case Field::W:
        return "w";
    case Field::H:
        return "h";
    }
    return "?";
}

constexpr Field kFields[] = {Field::X, Field::Y, Field::W, Field::H};

// The element's rect relative to its own flex container, in PHYSICAL px —
// BorderBox's space, so a 30px item must occupy 60 device px at contentScale 2.
void RelativeRect(const GameEngine::UIElement& container, const GameEngine::UIElement& target,
                  float contentScale, float (&out)[4])
{
    out[0] = (target.GetLayoutX() - container.GetLayoutX()) * contentScale;
    out[1] = (target.GetLayoutY() - container.GetLayoutY()) * contentScale;
    out[2] = target.GetLayoutWidth() * contentScale;
    out[3] = target.GetLayoutHeight() * contentScale;
}

std::string TargetId(const ChromeRect& row)
{
    const std::string containerId = row.Container;
    return row.Child == kContainerRow ? containerId
                                      : containerId + "__" + std::to_string(row.Child);
}

// Reads one fixture's rects out of a settled manager, in table order.
// Returns false when the tree does not contain what the table names.
bool ReadFixture(const IsolatedUIFixture& fx, const ChromeRect* table, std::size_t count,
                 float contentScale, std::vector<std::array<float, 4>>& out)
{
    out.clear();
    for (std::size_t i = 0; i < count; ++i)
    {
        const GameEngine::UIElement* container = fx.Element(table[i].Container);
        const GameEngine::UIElement* target = fx.Element(TargetId(table[i]));
        if (!container || !target)
            return false;
        float rel[4];
        RelativeRect(*container, *target, contentScale, rel);
        out.push_back({rel[0], rel[1], rel[2], rel[3]});
    }
    return true;
}

void CompareFixture(float contentScale, const char* xml, const char* css, const ChromeRect* table,
                    std::size_t count)
{
    IsolatedUIFixture fx;
    if (!fx.Build(contentScale, xml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const ChromeRect& row = table[i];
        const GameEngine::UIElement* container = fx.Element(row.Container);
        ASSERT_NE(container, nullptr) << row.Container;
        const std::string targetId = TargetId(row);
        const GameEngine::UIElement* target = fx.Element(targetId);
        ASSERT_NE(target, nullptr) << targetId;

        float rel[4];
        RelativeRect(*container, *target, contentScale, rel);

        for (int f = 0; f < 4; ++f)
        {
            SCOPED_TRACE(targetId + "." + FieldName(kFields[f]));

            // Chrome's own number, except on the enumerated one-step fields.
            EXPECT_FLOAT_EQ(rel[f], ExpectedDevicePx(row, kFields[f], contentScale));

            // The same statement without the per-field table, and the part that
            // discriminates: whatever the engine does, it is never more than one
            // grid step from Chrome. A distribution computed on inner instead of
            // outer sizes, or free space split n instead of n+1 ways, moves
            // items by whole pixels — 64 steps and up.
            const float chromeDevice = ChromeAtScale(row, kFields[f], contentScale) * contentScale;
            EXPECT_LE(std::fabs(rel[f] - chromeDevice), kGridStepDevicePx + kSlackDevicePx);

            // And it is a solved edge on the grid, not an arbitrary float.
            EXPECT_TRUE(IsOnGrid(rel[f])) << rel[f];
        }
    }
}

} // namespace

TEST(FlexJustifyAlignParity, RowSingleLineMatchesChromeAtScale1)
{
    CompareFixture(1.0f, kRowXml, kRowCss, kChromeRow, std::size(kChromeRow));
}

TEST(FlexJustifyAlignParity, RowSingleLineMatchesChromeAtScale2)
{
    CompareFixture(2.0f, kRowXml, kRowCss, kChromeRow, std::size(kChromeRow));
}

TEST(FlexJustifyAlignParity, ColumnSingleLineMatchesChromeAtScale1)
{
    CompareFixture(1.0f, kColXml, kColCss, kChromeCol, std::size(kChromeCol));
}

TEST(FlexJustifyAlignParity, ColumnSingleLineMatchesChromeAtScale2)
{
    CompareFixture(2.0f, kColXml, kColCss, kChromeCol, std::size(kChromeCol));
}

// Distribution has to operate on OUTER main sizes and alignment on the MARGIN
// box, which only unequal margins on every edge can distinguish: four items
// whose outer widths sum to 227 in a 300px container leave 73px of free space,
// so the three modes land on 73/3, 73/4 and 73/5 and cannot alias each other.
// The column arm puts the same margins on the cross axis, where align-items
// must offset by margin-left/right rather than top/bottom.
TEST(FlexJustifyAlignParity, AsymmetricMarginsMatchChromeAtScale1)
{
    CompareFixture(1.0f, kMarginXml, kMarginCss, kChromeMargin, std::size(kChromeMargin));
}

TEST(FlexJustifyAlignParity, AsymmetricMarginsMatchChromeAtScale2)
{
    CompareFixture(2.0f, kMarginXml, kMarginCss, kChromeMargin, std::size(kChromeMargin));
}

// The grid in numbers rather than left implicit in the tables. The first three
// are cases a designer would notice — an item centred in an odd-sized box, and
// space-around's eighths — and all three are now Chrome's exact value; on a
// whole-logical-pixel grid they were 14, 39 and 28.
//
// The fourth is what the grid cannot fix. 73/5 + 5 = 19.6 CSS px lies between
// steps at both scales, so it is decided by the rounding rule, and Yoga's
// half-up meets Chrome's truncate-toward-zero. At dsf 1 that is 1254.4 steps
// and both take 1254; at dsf 2 it is 2508.8 and they part by one.
TEST(FlexJustifyAlignParity, QuantisesToOneSixtyFourthOfADevicePixel)
{
    struct Case
    {
        const char* Id;
        Field Which;
        float ChromeCssPxAtOne;
        float EngineCssPxAtOne;
        float ChromeCssPxAtTwo;
        float EngineCssPxAtTwo;
    };
    // Chrome dsf 1 and dsf 2 columns, straight from the tables above.
    constexpr Case kCases[] = {
        {"mrow_sa_ce__0", Field::X, 14.125f, 14.125f, 14.125f, 14.125f},
        {"mrow_sa_ce__3", Field::Y, 38.5f, 38.5f, 38.5f, 38.5f},
        {"mcol_sa_ce__0", Field::X, 27.5f, 27.5f, 27.5f, 27.5f},
        {"mrow_se_fs__0", Field::X, 19.59375f, 19.59375f, 19.59375f, 19.6015625f},
    };

    for (const float scale : {1.0f, 2.0f})
    {
        IsolatedUIFixture fx;
        if (!fx.Build(scale, kMarginXml, kMarginCss))
        {
            if (!fx.DeviceAvailable())
                GTEST_SKIP() << fx.Diagnostic();
            FAIL() << fx.Diagnostic();
        }

        for (const Case& c : kCases)
        {
            SCOPED_TRACE(std::string(c.Id) + " at contentScale " + std::to_string(scale));
            const std::string childId = c.Id;
            const std::string containerId = childId.substr(0, childId.find("__"));
            const GameEngine::UIElement* container = fx.Element(containerId);
            const GameEngine::UIElement* child = fx.Element(childId);
            ASSERT_NE(container, nullptr);
            ASSERT_NE(child, nullptr);

            float rel[4];
            RelativeRect(*container, *child, scale, rel);
            const float engine = rel[c.Which == Field::X ? 0 : 1];

            const float engineCss = scale == 1.0f ? c.EngineCssPxAtOne : c.EngineCssPxAtTwo;
            const float chromeCss = scale == 1.0f ? c.ChromeCssPxAtOne : c.ChromeCssPxAtTwo;
            EXPECT_FLOAT_EQ(engine, engineCss * scale);
            EXPECT_LE(std::fabs(engine - chromeCss * scale), kGridStepDevicePx + kSlackDevicePx);
            EXPECT_TRUE(IsOnGrid(engine)) << engine;

            // The negative that a reverted grid would trip: none of these four
            // is the whole-logical-pixel answer.
            EXPECT_NE(engine, std::round(chromeCss) * scale);
        }
    }
}

// Content scale is not a pure output multiplier any more. The grid is defined in
// DEVICE px, so the scale-2 solve rounds on a grid twice as fine in logical
// space and a length between steps can land on the other side. The solve itself
// is unchanged, which is what the one-step bound says; kScaleArmsDiffer is
// exactly where the finer grid picks differently, and being closer to Chrome is
// NOT what decides it — at row_sa child 2 the scale-1 arm is the Chrome-exact
// one and the finer grid is the step out.
TEST(FlexJustifyAlignParity, ContentScaleRefinesTheGridRatherThanRescalingTheSolve)
{
    struct ArmsDiffer
    {
        const char* Container;
        int Child;
        Field Which;
    };
    constexpr ArmsDiffer kScaleArmsDiffer[] = {
        {"row_sa_fs", 0, Field::X}, {"row_sa_fs", 2, Field::X},
        {"row_sa_ce", 0, Field::X}, {"row_sa_ce", 2, Field::X},
        {"row_sa_fe", 0, Field::X}, {"row_sa_fe", 2, Field::X},
        {"row_sa_st", 0, Field::X}, {"row_sa_st", 2, Field::X},
        {"col_sa_fs", 0, Field::Y}, {"col_sa_fs", 2, Field::Y},
        {"col_sa_ce", 0, Field::Y}, {"col_sa_ce", 2, Field::Y},
        {"col_sa_fe", 0, Field::Y}, {"col_sa_fe", 2, Field::Y},
        {"col_sa_st", 0, Field::Y}, {"col_sa_st", 2, Field::Y},
        {"mrow_sb_fe", 1, Field::X}, {"mrow_sb_fe", 2, Field::X},
        {"mrow_se_fs", 0, Field::X}, {"mrow_se_fs", 3, Field::X},
        {"mcol_sb_fe", 1, Field::Y}, {"mcol_sb_fe", 2, Field::Y},
        {"mcol_se_fs", 1, Field::Y}, {"mcol_se_fs", 2, Field::Y},
    };
    const auto armsDiffer = [&](const ChromeRect& r, Field which) {
        for (const ArmsDiffer& e : kScaleArmsDiffer)
        {
            if (e.Child == r.Child && e.Which == which && std::string(e.Container) == r.Container)
                return true;
        }
        return false;
    };

    struct Fixture
    {
        const char* Xml;
        const char* Css;
        const ChromeRect* Table;
        std::size_t Count;
    };
    const Fixture fixtures[] = {
        {kRowXml, kRowCss, kChromeRow, std::size(kChromeRow)},
        {kColXml, kColCss, kChromeCol, std::size(kChromeCol)},
        {kMarginXml, kMarginCss, kChromeMargin, std::size(kChromeMargin)},
    };

    for (const Fixture& f : fixtures)
    {
        IsolatedUIFixture one;
        IsolatedUIFixture two;
        if (!one.Build(1.0f, f.Xml, f.Css) || !two.Build(2.0f, f.Xml, f.Css))
        {
            if (!one.DeviceAvailable() || !two.DeviceAvailable())
                GTEST_SKIP() << one.Diagnostic();
            FAIL() << one.Diagnostic();
        }

        std::vector<std::array<float, 4>> atOne;
        std::vector<std::array<float, 4>> atTwo;
        ASSERT_TRUE(ReadFixture(one, f.Table, f.Count, 1.0f, atOne));
        ASSERT_TRUE(ReadFixture(two, f.Table, f.Count, 2.0f, atTwo));
        ASSERT_EQ(atOne.size(), atTwo.size());

        for (std::size_t i = 0; i < atOne.size(); ++i)
        {
            for (int k = 0; k < 4; ++k)
            {
                SCOPED_TRACE(TargetId(f.Table[i]) + "." + FieldName(kFields[k]));
                const float scaledOne = atOne[i][k] * 2.0f;
                EXPECT_LE(std::fabs(atTwo[i][k] - scaledOne),
                          kGridStepDevicePx + kSlackDevicePx);
                EXPECT_TRUE(IsOnGrid(atOne[i][k])) << atOne[i][k];
                EXPECT_TRUE(IsOnGrid(atTwo[i][k])) << atTwo[i][k];

                const bool differs = std::fabs(atTwo[i][k] - scaledOne) > kSlackDevicePx;
                EXPECT_EQ(differs, armsDiffer(f.Table[i], kFields[k]))
                    << atOne[i][k] << " -> " << atTwo[i][k];
            }
        }
    }
}
