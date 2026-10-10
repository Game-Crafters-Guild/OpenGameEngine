#include "UI/UIStyle.h"

#include <string>

namespace GameEngine {

CustomPropertyScope::LookupStatus CustomPropertyScope::Lookup(StringId name, const Entry*& outValue) const
{
    outValue = nullptr;
    const CustomPropertyScope* cur = this;
    while (cur)
    {
        const Entry* v = cur->Local.Find(name);
        if (v)
        {
            if (v->Invalid)
                return LookupStatus::Invalid;
            outValue = v;
            return LookupStatus::Found;
        }
        cur = cur->Parent.get();
    }
    return LookupStatus::Missing;
}

} // namespace GameEngine
