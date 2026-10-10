#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{
class IntField;

// A day of the year as the inspectors show and read it, in a year without 29 February (day 1 is
// 1 January, day 365 is 31 December).

// "21 March" for day 80.
std::string DateOfDay(int dayOfYear);

// The day a typed date names: "21 June", "June 21", "21 Jun", "21/6" (day first, so "6/7" is 6 July;
// "6/21" is read month first because it is no date day first) and "2026-06-21", whose year does not
// change the day. A month name may be written in full or by its first three letters, in any case.
// False for anything else, including a date that does not exist ("30 February", "31/4") and two
// numbers split by a slash that are no date either way.
bool TryParseDayOfYear(std::string_view text, int& outDay);

// Let a day-of-year field take a typed date as well as a number: a date is read first, anything
// else the way every IntField reads it. Typed text is read when it is committed, not per keystroke,
// so the first digits of "21 June" never pass for day 2 or 21, and text that is neither a date nor a
// number leaves the field's value unchanged.
void AcceptDateEntry(IntField& field);

} // namespace GameEngine
