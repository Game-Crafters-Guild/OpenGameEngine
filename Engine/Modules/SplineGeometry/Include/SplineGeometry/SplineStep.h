#pragma once

#include "Mathematics/Vector3.h"
#include "SplineGeometry/SplineStation.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// Step a wall's top at its authored points: each run between two consecutive
// points keeps a level top, the profile's height above its highest station, so
// no stretch of it stands lower than that, and it grows taller toward the
// downhill end.
// Masonry is built this way on a slope: the courses stay level within a run and
// step where a builder would put a tower anyway.
//
// `pointDistances` are the Distances of the stations standing on the run's
// interior authored points, ascending; on a welded loop the seam point is the
// stream's two ends and is not listed. A corner's rings all stand at its point's
// Distance: a Mitre's first ring takes the run before it and its Crease ring the
// run after. Any other station on a point — a round corner's fan, or the single
// station of a smooth point — takes the higher of the two levels, and gains a
// Crease copy on the side of the lower one, so every step is a Crease ring whose
// top differs from the ring before it; the builder closes it with a riser
// (SplineStripStation::TopOffset). A welded loop whose first and last runs stand
// at different levels gains a Crease copy of its last station at the first
// run's level.
//
// Every ring is stood plumb first — its Up the frame's vertical and its Forward
// level — so each top rises straight above its base: a ring leaning with the
// grade would carry a raised top along the run too, by a different amount at
// every ring, and fan the side faces between them. Heights are measured along
// `up`, the frame's vertical in the stations' space. Run it after the corners
// and the face turn.
void StepTopsAtPoints(std::vector<SplineStripStation>& stations,
                      std::span<const float32> pointDistances, const Mathematics::Vector3& up,
                      bool closedLoop);

} // namespace GameEngine::SplineGeometry
