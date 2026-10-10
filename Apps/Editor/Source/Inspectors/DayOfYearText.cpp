#include "Inspectors/DayOfYearText.h"

#include "Rendering/Sky/SolarPath.h"
#include "UI/Controls/IntField.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr std::array<std::string_view, 12> kCalendarMonthNames = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
constexpr std::array<int, 12> kCalendarDaysInMonth = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr size_t kMonthAbbreviationLength = 3;
constexpr size_t kYearDigits = 4;

// The day of the year for a day of a month (both counted from 1), or nothing if that date does not exist.
std::optional<int> DayOfYearFromDate(int month, int dayOfMonth)
{
    if (month < 1 || month > 12 || dayOfMonth < 1 || dayOfMonth > kCalendarDaysInMonth[month - 1])
        return std::nullopt;
    int day = dayOfMonth;
    for (int m = 1; m < month; ++m)
        day += kCalendarDaysInMonth[m - 1];
    return day;
}

bool IsDigits(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
}

std::optional<int> WholeNumber(std::string_view text)
{
    if (!IsDigits(text))
        return std::nullopt;
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

// 1 to 12 for "June", "june", "JUN"; nothing for anything else.
std::optional<int> MonthFromName(std::string_view word)
{
    if (word.size() < kMonthAbbreviationLength)
        return std::nullopt;
    for (size_t month = 0; month < kCalendarMonthNames.size(); ++month)
    {
        const std::string_view name = kCalendarMonthNames[month];
        if (word.size() != kMonthAbbreviationLength && word.size() != name.size())
            continue;
        const bool matches = std::equal(word.begin(), word.end(), name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
        if (matches)
            return static_cast<int>(month) + 1;
    }
    return std::nullopt;
}

// A day-of-month word with an optional English ordinal ending: "21", "21st".
std::optional<int> DayOfMonthFromWord(std::string_view word)
{
    if (word.size() > 2)
    {
        const std::string_view ending = word.substr(word.size() - 2);
        if (ending == "st" || ending == "nd" || ending == "rd" || ending == "th")
            word.remove_suffix(2);
    }
    return WholeNumber(word);
}

std::vector<std::string_view> SplitDateText(std::string_view text, std::string_view separators)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = std::min(text.find_first_of(separators, start), text.size());
        if (end > start)
            parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string_view TrimmedDateText(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

// "21/6": day first, so "6/7" is 6 July; "6/21" reads month first only because it is no date day first.
std::optional<int> DayOfYearFromSlashDate(std::string_view text)
{
    const std::vector<std::string_view> parts = SplitDateText(text, "/");
    if (parts.size() != 2)
        return std::nullopt;
    const std::optional<int> first = WholeNumber(TrimmedDateText(parts[0]));
    const std::optional<int> second = WholeNumber(TrimmedDateText(parts[1]));
    if (!first || !second)
        return std::nullopt;
    if (const std::optional<int> dayFirst = DayOfYearFromDate(*second, *first))
        return dayFirst;
    return DayOfYearFromDate(*first, *second);
}

// "2026-06-21": year, month, day; the year is read and ignored.
std::optional<int> DayOfYearFromIsoDate(std::string_view text)
{
    const std::vector<std::string_view> parts = SplitDateText(text, "-");
    if (parts.size() != 3 || parts[0].size() != kYearDigits || !IsDigits(parts[0]))
        return std::nullopt;
    const std::optional<int> month = WholeNumber(parts[1]);
    const std::optional<int> day = WholeNumber(parts[2]);
    if (!month || !day)
        return std::nullopt;
    return DayOfYearFromDate(*month, *day);
}

// "21 June", "June 21", "21st of June", "June 21, 2026".
std::optional<int> DayOfYearFromNamedMonth(std::string_view text)
{
    std::optional<int> month;
    std::optional<int> day;
    for (const std::string_view word : SplitDateText(text, " ,."))
    {
        if (word == "of")
            continue;
        if (const std::optional<int> named = MonthFromName(word))
        {
            if (month)
                return std::nullopt;
            month = named;
            continue;
        }
        if (word.size() == kYearDigits && IsDigits(word) && day)
            continue;
        const std::optional<int> number = DayOfMonthFromWord(word);
        if (!number || day)
            return std::nullopt;
        day = number;
    }
    if (!month || !day)
        return std::nullopt;
    return DayOfYearFromDate(*month, *day);
}

// "21/6" and "2026-06-21" read as arithmetic too; in this field they are dates or nothing.
bool IsNumericDateShape(std::string_view text)
{
    text = TrimmedDateText(text);
    return text.find('/') != std::string_view::npos ||
           (text.size() > kYearDigits && IsDigits(text.substr(0, kYearDigits)) && text[kYearDigits] == '-');
}
} // namespace

std::string DateOfDay(int dayOfYear)
{
    int day = std::clamp(dayOfYear, 1, Rendering::kDaysInCalendarYear);
    for (size_t month = 0; month < kCalendarDaysInMonth.size(); ++month)
    {
        if (day <= kCalendarDaysInMonth[month])
            return std::to_string(day) + " " + std::string(kCalendarMonthNames[month]);
        day -= kCalendarDaysInMonth[month];
    }
    return {};
}

bool TryParseDayOfYear(std::string_view text, int& outDay)
{
    text = TrimmedDateText(text);
    std::optional<int> day;
    if (text.find('/') != std::string_view::npos)
        day = DayOfYearFromSlashDate(text);
    else if (text.find('-') != std::string_view::npos)
        day = DayOfYearFromIsoDate(text);
    else
        day = DayOfYearFromNamedMonth(text);
    if (!day)
        return false;
    outDay = *day;
    return true;
}

void AcceptDateEntry(IntField& field)
{
    field.SetParseFunction([](const std::string& text, int& out, bool isFinal) {
        if (!isFinal)
            return false;
        if (TryParseDayOfYear(text, out))
            return true;
        return !IsNumericDateShape(text) && IntField::TryParseInt(text, out);
    });
}

} // namespace GameEngine
