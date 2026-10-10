#pragma once

#include <string>

namespace GameEngine
{

// An illuminance as the inspectors show it: from 100 lux up, three significant figures with thousands
// grouped by a space ("98 600 lx", "100 000 lx"); one decimal from 1 lx ("5.2 lx"); two below. The
// spaces are non-breaking, so a wrapped sentence never splits a figure or parts it from its unit.
std::string FormatLux(float lux);

} // namespace GameEngine
