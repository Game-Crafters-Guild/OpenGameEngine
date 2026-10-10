#pragma once

#include <string>
#include <cmath>

#include "UI/Controls/FloatField.h"
#include "UI/Controls/TextField.h"
#include "NumericExpression/NumericExpression.h"

namespace GameEngine {

// Numeric field for editing a single integer value. Shares FloatField's math
// expression syntax (sin, sqrt, pi, etc.), rounding to the nearest integer on commit.
class IntField : public TextFieldBase<int>
{
public:
    using ValueType = int;

    IntField();

    // Optional inclusive clamp applied whenever the value is set or typed.
    // Call with equal min/max to enforce a fixed value.
    void SetRange(int minVal, int maxVal) { m_Min = minVal; m_Max = maxVal; m_HasRange = true; }
    void ClearRange() { m_HasRange = false; }

    void SetValue(const int& v) override
    {
        TextFieldBase<int>::SetValue(m_HasRange ? std::clamp(v, m_Min, m_Max) : v);
    }

    static bool TryParseInt(const std::string& text, int& outValue);

protected:
    int ConstrainTypedValue(const int& value) const override
    {
        return m_HasRange ? std::clamp(value, m_Min, m_Max) : value;
    }

private:
    static std::string FormatInt(int v);

    bool m_HasRange = false;
    int  m_Min = 0;
    int  m_Max = 0;
};

inline IntField::IntField()
{
    AddClass("int-field");
	    ConfigureEditorIdentity("int-input-", "int-field-input");
     if (TextInput* editor = GetTextInput())
         editor->AddClass("numeric-field-input");

    SetFilterFunction([](const std::string& raw, bool /*isFinal*/)
    {
        return FilterNumericExpressionInput(raw);
    });

    SetParseFunction([](const std::string& textValue, int& out, bool /*isFinal*/)
    {
        return TryParseInt(textValue, out);
    });

    SetFormatFunction([](int v)
    {
        return FormatInt(v);
    });

    SetEmptyValues(0, 0);

    SetValue(0);
}

inline std::string IntField::FormatInt(int v)
{
    return std::to_string(v);
}

inline bool IntField::TryParseInt(const std::string& text, int& outValue)
{
    float f = 0.0f;
    if (!FloatField::TryParseFloat(text, f))
    {
        return false;
    }
    outValue = static_cast<int>(std::lround(f));
    return true;
}

} // namespace GameEngine

