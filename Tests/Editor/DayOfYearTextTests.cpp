// The sky's Day of year field reads a typed date as the day it falls on, so nobody has to count the
// days to 21 June by hand; anything that is not a date or a number leaves the day as it was.

#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "Inspectors/DayOfYearText.h"
#include "UI/Controls/IntField.h"

#include <string>

using namespace GameEngine;

namespace
{
constexpr int kJuneSolstice = 172;

TextInput* EditorOf(IntField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

// Type the text into the field the way a keyboard does, then press Enter.
void TypeAndCommit(IntField& field, const std::string& text)
{
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    for (const char c : text)
        editor->OnChar(static_cast<unsigned char>(c));
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
}

int ParsedDay(const std::string& text)
{
    int day = -1;
    EXPECT_TRUE(TryParseDayOfYear(text, day)) << text;
    return day;
}
} // namespace

TEST(DayOfYearText, ReadsEveryDateForm)
{
    EXPECT_EQ(ParsedDay("21 June"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("June 21"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("21 jun"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("21st of June"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("June 21, 2026"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("21/6"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("6/21"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("2026-06-21"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("1999-06-21"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("1 January"), 1);
    EXPECT_EQ(ParsedDay("31 December"), 365);
}

// A slash date is read day first, as the field's tooltip says, and month first only where that is
// the one reading that is a date.
TEST(DayOfYearText, ASlashDateReadsDayFirst)
{
    EXPECT_EQ(ParsedDay("21/6"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("6/21"), kJuneSolstice);
    EXPECT_EQ(ParsedDay("6/6"), 157);
    EXPECT_EQ(ParsedDay("6/7"), 187);
    EXPECT_EQ(ParsedDay("1/12"), 335);
}

TEST(DayOfYearText, EveryDayReadsBackFromItsDate)
{
    for (int day = 1; day <= 365; ++day)
        EXPECT_EQ(ParsedDay(DateOfDay(day)), day) << DateOfDay(day);
}

TEST(DayOfYearText, RejectsWhatIsNoDate)
{
    for (const char* text : {"hello", "30 February", "29 February", "31/4", "32/1", "2026-13-01", "June", "21",
                             "June 21 July", "", "Sept 21", "21 juin", "06/21/2026"})
    {
        int day = -1;
        EXPECT_FALSE(TryParseDayOfYear(text, day)) << text;
        EXPECT_EQ(day, -1) << text;
    }
}

// The Day of year field installs its own parse function; the field's range still holds a typed
// number past the last day to that day.
TEST(DayOfYearText, ANumberPastTheYearCommitsTheLastDay)
{
    IntField field;
    field.SetRange(1, 365);
    AcceptDateEntry(field);
    field.SetValue(80);
    TypeAndCommit(field, "400");
    EXPECT_EQ(field.GetValue(), 365);
}

// Through the field: a typed date commits its day, a number still commits itself, and text that is
// neither (or a slash date that does not exist) leaves the field's value unchanged.
TEST(DayOfYearText, TheFieldTakesATypedDateAndKeepsItsValueOtherwise)
{
    IntField field;
    field.SetRange(1, 365);
    AcceptDateEntry(field);
    field.SetValue(80);

    TypeAndCommit(field, "21 June");
    EXPECT_EQ(field.GetValue(), kJuneSolstice);
    TypeAndCommit(field, "140");
    EXPECT_EQ(field.GetValue(), 140);
    TypeAndCommit(field, "2026-06-21");
    EXPECT_EQ(field.GetValue(), kJuneSolstice);
    TypeAndCommit(field, "21/12");
    EXPECT_EQ(field.GetValue(), 355);

    TypeAndCommit(field, "hello");
    EXPECT_EQ(field.GetValue(), 355);
    TypeAndCommit(field, "31/4");
    EXPECT_EQ(field.GetValue(), 355);
    TypeAndCommit(field, "6/7");
    EXPECT_EQ(field.GetValue(), 187);
}
