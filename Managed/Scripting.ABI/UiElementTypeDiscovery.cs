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
    /// The reflection scan: which types in an assembly are <c>[UiElement]</c> types, and which of
    /// their members bind to authored attributes.
    /// <para>
    /// A separate class because it holds no state and touches nothing — no lock, no table, no ABI
    /// call — which is also what lets it run on whichever thread a reload handler landed on.
    /// </para>
    /// </summary>
    internal static class UiElementTypeDiscovery
    {
        // Attribute identity is compared by FULL NAME, never by typeof. That is the house
        // pattern for cross-context discovery (HotReloadManager, EditorMenuBridge): a script
        // that ships its own copy of the attribute would be a different Type with the same
        // name, and refusing to see it would be a silent, baffling non-registration.
        private const string kUiElementAttrFullName = "GameEngine.Scripting.UiElementAttribute";
        private const string kUiAttributeAttrFullName = "GameEngine.Scripting.UiAttributeAttribute";

        internal static List<(string Tag, Type Type)> Discover(Assembly assembly)
        {
            var results = new List<(string, Type)>();

            Type[] types;
            try { types = assembly.GetTypes(); }
            catch (ReflectionTypeLoadException ex)
            {
                var kept = new List<Type>();
                if (ex.Types != null)
                {
                    foreach (Type? t in ex.Types)
                        if (t != null)
                            kept.Add(t);
                }
                types = kept.ToArray();
            }
            catch { return results; }

            foreach (Type t in types)
            {
                if (t == null || t.IsAbstract || !t.IsClass)
                    continue;

                string? tag = ReadTag(t);
                if (string.IsNullOrEmpty(tag))
                    continue;

                // Both are hard requirements of the factory contract, and a type that fails one
                // would otherwise fail silently at the first <Tag/> in a document — long after
                // the mistake, and with nothing naming it.
                if (!typeof(Ui.Element).IsAssignableFrom(t))
                {
                    UiElementTypeLog.Report($"[UiElement(\"{tag}\")] {t.FullName} does not derive from Ui.Element; " +
                           "the tag was not registered.");
                    continue;
                }
                if (t.GetConstructor(Type.EmptyTypes) == null)
                {
                    UiElementTypeLog.Report($"[UiElement(\"{tag}\")] {t.FullName} has no public parameterless " +
                           "constructor; the tag was not registered.");
                    continue;
                }
                results.Add((tag!, t));
            }
            return results;
        }

        private static string? ReadTag(Type t)
        {
            IList<CustomAttributeData> attrs;
            try { attrs = t.GetCustomAttributesData(); }
            catch { return null; }

            foreach (CustomAttributeData cad in attrs)
            {
                string? full = null;
                try { full = cad.AttributeType?.FullName; } catch { }
                if (!string.Equals(full, kUiElementAttrFullName, StringComparison.Ordinal))
                    continue;
                if (cad.ConstructorArguments.Count < 1)
                    continue;
                return cad.ConstructorArguments[0].Value as string;
            }
            return null;
        }

        internal static Dictionary<string, MemberInfo> BuildMemberMap(Type type)
        {
            // Case-insensitive: the engine lowercases authored attribute names, and a document
            // author writing `Percent=` should not silently miss `percent`.
            var map = new Dictionary<string, MemberInfo>(StringComparer.OrdinalIgnoreCase);
            const BindingFlags flags =
                BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;

            MemberInfo[] members;
            try { members = type.GetMembers(flags); }
            catch { return map; }

            foreach (MemberInfo m in members)
            {
                if (m is not PropertyInfo && m is not FieldInfo)
                    continue;

                IList<CustomAttributeData> attrs;
                try { attrs = m.GetCustomAttributesData(); }
                catch { continue; }

                foreach (CustomAttributeData cad in attrs)
                {
                    string? full = null;
                    try { full = cad.AttributeType?.FullName; } catch { }
                    if (!string.Equals(full, kUiAttributeAttrFullName, StringComparison.Ordinal))
                        continue;

                    string name = m.Name;
                    if (cad.ConstructorArguments.Count >= 1 &&
                        cad.ConstructorArguments[0].Value is string explicitName &&
                        !string.IsNullOrEmpty(explicitName))
                    {
                        name = explicitName;
                    }

                    if (m is PropertyInfo p && !p.CanWrite)
                    {
                        UiElementTypeLog.Report($"[UiAttribute] {type.FullName}.{m.Name} has no setter and was ignored.");
                        break;
                    }
                    map[name] = m;
                    break;
                }
            }
            return map;
        }
    }
}
