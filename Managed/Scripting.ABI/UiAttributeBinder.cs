using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Turning an authored attribute's string into the value a member takes, and assigning it.
    /// Stateless, and deliberately the only place that knows a document's spelling of a value
    /// differs from the CLR's.
    /// </summary>
    internal static class UiAttributeBinder
    {
        internal static void Assign(object target, MemberInfo member, string raw)
        {
            Type memberType = member is PropertyInfo p ? p.PropertyType : ((FieldInfo)member).FieldType;
            if (!TryConvert(raw, memberType, out object? converted))
            {
                UiElementTypeLog.Report($"attribute value '{raw}' is not a valid {memberType.Name} for " +
                       $"{target.GetType().FullName}.{member.Name}; the member was left unchanged.");
                return;
            }

            if (member is PropertyInfo prop)
                prop.SetValue(target, converted);
            else
                ((FieldInfo)member).SetValue(target, converted);
        }

        private static bool TryConvert(string raw, Type target, out object? result)
        {
            result = null;
            try
            {
                if (target == typeof(string)) { result = raw; return true; }
                if (target.IsEnum) { result = Enum.Parse(target, raw, ignoreCase: true); return true; }
                if (target == typeof(bool))
                {
                    // The document spells booleans the way .uxml and CSS do, not the way
                    // bool.Parse does.
                    if (raw.Equals("true", StringComparison.OrdinalIgnoreCase) || raw == "1")
                    { result = true; return true; }
                    if (raw.Equals("false", StringComparison.OrdinalIgnoreCase) || raw == "0")
                    { result = false; return true; }
                    return false;
                }
                // Invariant culture throughout: a document is not locale-dependent, and parsing
                // "0.5" against a comma-decimal locale would silently read 5.
                result = Convert.ChangeType(raw, target, CultureInfo.InvariantCulture);
                return result != null;
            }
            catch
            {
                return false;
            }
        }
    }
}
