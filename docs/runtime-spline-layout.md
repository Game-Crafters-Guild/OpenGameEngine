# Runtime spline layout

The public `SplineLayout/` headers expose tile and fence layout through
`GameEngine::SplineLayout`. Implementations live in Engine and are available in
the staged SDK. They do not create entities, load meshes, pick the scene or own
gameplay state. Editor recipes and runtime construction use the same functions.

For a fence or wall, provide:

- A world-space `CenterSample` polyline, including measured surface normals.
- `FenceLayoutParams.RunBoundaries`, identifying authored corners by fractional
  sample index. Two boundaries describe one run; adjacent runs share a station.
- Active span-pool bounds and optional post bounds. The solver measures the
  along-path axis from those bounds and uses their natural lengths to fill runs.
- Planting, grade and stretch settings, and optionally a `StationSurfaceProbe`
  that measures the exact surface beneath each final station.

`BuildFenceLayout` returns station and span poses, length scales, pool choices
and validation messages. `BuildTilePoses` provides the corresponding tile layout,
including deterministic variation and caller-supplied station distances.
`PieceBasis`, `PieceEntity` and `SeamShear` provide shared pose-composition helpers.
The `PieceEntity` helpers write transforms and names; they do not create entities.

Surface-query policy belongs to the caller. A runtime game can probe composed
terrain directly; the editor can use its picking service. A probe callback must
respect the threading requirements of the surface implementation it calls.
`HoldSurfaceAcrossGaps` preserves the existing nearest-measured-height behavior
for missing surface samples. A game that forbids construction over holes should
reject those routes before committing, rather than interpreting that fallback
as proof of buildable ground.

Inspect layout validation before applying the result. Piece-budget limits and
unsatisfied stretch or grade constraints can produce partial output. Layout
ordinals describe one evaluation; they are not persistent gameplay IDs. Callers
must preserve health, selection, ownership and navigation identities across
re-layout, and stage a complete valid result before replacing existing entities.

The editor's `Placement/` headers forward to the runtime API for source
compatibility. Controllers still own editor surface picking, rebuild scheduling
and generated-entity lifetime. Recipe fields, serialization and layout math are
unchanged by this extraction. Junction topology is a separate feature; this API
does not implement it.

## Verification

`EngineSplineLayoutTests` calls the public API through the Engine library without
editor include directories or implementation sources. The existing editor layout,
recipe and parenting suites exercise the forwarding headers against that same
implementation.

`Tests/Projects/SplineLayoutSDK` is a standalone consumer. Configure it with
`-DGameEngine_DIR=<staged SDK>/cmake` and a build configuration matching the SDK.
It builds against `GameEngine::Engine` without engine source paths or Editor.
Run it with the staged engine and dependency DLLs on the loader path on Windows.

## Module boundary

`SplineLayout` is an OBJECT module under `Engine/Modules/SplineLayout`, alongside
`Spline`, `SplineGeometry` and `SplineECS`. The low-level `Spline` module retains
its Types/Mathematics-only dependencies. Layout accepts sampled centerlines,
reads placement component recipes and writes transform/name values; its public
dependencies are Types, Mathematics, AssetCore and ECSComponents. It does not
link Engine, Editor or a renderer. It does not create entities or query a world.

Engine includes the module's objects and exposes its public include directory
through its existing umbrella. SDK staging discovers module Include directories,
so `<SplineLayout/...>` remains the public include path. Editor forwarding
headers and existing SDK consumers continue to use that same API.

`SplineLayoutTests` runs the layout suites against the module without linking
Engine or Editor. `EngineSplineLayoutTests` and the standalone SDK consumer
separately verify the Engine DLL and SDK boundary. Editor integration also builds
these shared suites alongside its controller tests.

## Independent runtime compression limits

`FenceLayoutParams::SpanMinScale` optionally supplies a compression floor
separate from `SpanMaxStretch`. For a 5 m kit, `SpanMinScale = 0.15` and
`SpanMaxStretch = 1.25` admit a 0.75 m connector while a 7.41 m run uses two
panels at approximately 0.741 scale rather than one at 1.482 scale. The usual
nominal/adjacent-count search and validation behavior remain unchanged.

The default zero keeps the reciprocal floor used by existing editor recipes.
Negative or non-finite values also use that default; values above one clamp to
one. Validation identifies an explicit floor separately from a reciprocal floor.
This setting belongs to the runtime layout parameters; no serialized editor
recipe field or new inspector control is introduced here.

## Crest rows and caps

A crest row is a line of pieces along the tops of the spans: battlements,
coping, lamps. It follows one of two rules per run.

When the crest pitch is the longest crest piece's own length (Crest Pitch 0, or
typed equal to it) and every wall in the run has that same length, the row is
registered to the walls: one crest piece stands on each wall, centered on it,
posed in its frame and stretched exactly as far as the fill stretched that wall.
Its ends are the wall's ends, so the row closes wherever the walls close,
around a curve too, and the posts reserve nothing against it: the post at a
wall's end covers the crest's join exactly as it covers the wall's.

Otherwise the run keeps the pitch grid, and its pieces are never stretched.
Cells are sized to the longest piece in the pool and each piece is centered in
its cell, so the row reads as one repeated profile however long the run is.
Cells are measured from each run's opening authored point, so editing the
spline elsewhere leaves the row where it stands. A piece that would stand
inside a post's footprint, inside a stretch a runtime caller reserved, or past
the end of its run, is dropped rather than squeezed; a post at that edge covers
the join, and without one the gap shows. A run never mixes the two rules.

Caps are the end pieces some kits ship for those gaps. Each gap a crest row
leaves takes at most one cap: the longest cap that fits it, flush against the
last whole piece and facing the edge the gap ends at. What the cap does not
cover stays bare, and the post, if there is one, covers it. A cap longer than
every gap is never placed.

To author one, add a `SplineFence` to a spline entity, put the wall mesh into
Span and the battlement into Crest. Crest Pitch stays 0 unless the kit wants air
between pieces — 0 uses the longest crest piece's own length, so the pieces meet
flush, and a smaller value is raised to it and reported. The kit's end piece
goes into Cap. A fence with no span pool lays no crest row, because the row
stands on the tops of the spans, and the log says so.

On the pitch grid a mesh in Post reserves its footprint at every station, and
crest cells stand only in the clear stretch between two footprints. Because
cells are laid from the run's opening point and not from each post, a piece is
certain to fit a clear stretch only when it is at most half that stretch long
and, at a typed Crest Pitch, no longer than the stretch less the pitch. A wide
tower in Post with a battlement that is not as long as the wall panels can
therefore lay no crest pieces at all; the caps may still stand. When that
happens the log names the longest clear stretch and the piece length that
always fits it. Use a shorter crest piece, or clear the Post pool. A battlement
exactly as long as the wall panels registers to them instead and stands
between the towers.

A runtime caller passes the active crest and cap pool bounds in
`FenceLayoutParams::CrestPieces` and `CapPieces`, the pitch in `CrestPitch`, and
the stretches of run its own planted pieces occupy in `Reservations`. A
reservation is a signed interval about an authored point: negative reaches back
into the run closing on that point, positive forward into the run opening at it,
so one entry covers a node two runs meet at. Post footprints need no entry — the
layout derives them from the post bounds it was given.
`FenceLayoutResult::Crests` returns the row with its caps, distinguished by
`FenceCrest::IsCap`. `FenceCrest::LengthScale` is 1 on the pitch grid and for
every cap; a crest registered to its wall carries that wall's
`FenceSpan::LengthScale`, and the caller scales the piece along its length by
it as it does the wall. Crests are emitted after stations and spans under the same
piece budget, so a wall that exhausts the budget keeps its structure and reports
the cut. An empty `CrestPieces` produces no row and leaves the rest of the result
unchanged.

## Gates and explicit pieces

A span can be told what to draw instead of the fence's own pick. A gate draws a
piece from the Gate pool; an explicit piece keeps one chosen piece from the Span
pool whatever the seed picks. Both are span overrides on the recipe, addressed
by the authored point that opens the span's run and the span's position in that
run, counted from 0.

To make a gate, put the gate mesh into the fence's Gate pool and click the span
in the scene view. Its section leads the inspector: it names the span and
offers Use (Span Pool, Gate or Explicit Piece) and the pool piece to draw, by
mesh name. What Use chooses is saved on the fence, so it survives every rebuild;
the rest of the piece is rebuilt from the fence. The span stays selected when it
becomes a gate, so the gate piece can be chosen straight away. The fence's own
inspector lists every override under Span Overrides, where each can be changed
or removed. A fence holds at most sixteen, and both inspectors say so when the
table is full. Overrides are saved with the scene and undo like any other edit.

A gate is chosen, not stretched to the length of the wall it replaces: the fill
counts it at its own length, and the run's one shared stretch then closes the
run around it the way it closes around walls of mixed length, so the gate takes
that stretch like every other span of its run. The seeded picks of the other
spans stay what they were when the gate leaves its run's span count unchanged;
a gate that changes the count moves the picks of every later run, whose
ordinals follow the spans before them. A gate reserves its span of the crest
row, so battlements stop at a gate on the pitch grid and on a row registered to
the walls alike; only the walls decide registration, so a gate longer than the
walls leaves them registered. Where no post covers a gate's end at a bend, the
gate is mitred like a wall. With the Gate pool empty a gate is an opening: the
room of the wall it replaced with nothing in it, which is how to leave a gap in
a hedge. An opening places no piece, so it cannot be selected in the scene view
again; change it back in the fence's Span Overrides.

An override follows its point through edits of the spline. Inserting a point
moves it with the point it names. Deleting a point folds that point's run into
the run before it, and the override counts on past the spans that stood before
its run, so it names the same place along the wall; the merged run is filled
anew, so the span there may be a little longer or shorter. Resampling moves an
override to the new point nearest the old one. Deleting the first or last point
of an open spline removes that end's run and the overrides on it. An override
the layout cannot apply is never dropped in silence: a span past its run's
fill, a point that opens no run, a slot its pool does not hold, a run whose two
points coincide and a second override on one span are each reported once per
rebuild, and the inspectors show the ones they can tell beside the override.

A runtime caller passes the active gate-pool bounds in
`FenceLayoutParams::GatePieces` and the override table in `SpanOverrides`;
entries whose `Kind` is `None` are empty. `FenceSpan::IsGate` marks a gate, whose
`PoolSlot` then names a gate-pool slot. An opening emits no span but keeps its
ordinal in `FenceSpan::Index`, so the spans after it keep theirs. With no
override set the result is identical to one built without these fields.

Rebuild native consumers with the matching Engine DLL and SDK: the recipe
component and the public parameter struct have gained members and must not be
mixed across binary versions.
