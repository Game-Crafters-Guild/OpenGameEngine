using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    // The C ABI for externally-defined element types, and the trampolines the engine calls back
    // through. Everything that crosses the native boundary lives here; nothing here decides
    // policy — see UiElementTypes.cs for staging and UiElementTypesLifecycle.cs for the seams.
    internal static partial class UiElementTypes
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_AcquireTypeOwner(out ulong outOwner);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_ReleaseTypeOwner(ulong owner);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int GE_UI_SetElementTypeCallbacks(
            delegate* unmanaged[Cdecl]<ulong, ulong, nint> create,
            delegate* unmanaged[Cdecl]<nint, byte*, byte*, void> applyAttribute,
            delegate* unmanaged[Cdecl]<nint, void> release,
            delegate* unmanaged[Cdecl]<void> performRegistrations);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_RequestTypeRegistrationWindow();

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_RegisterElementType(ulong owner,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagName, out ulong outTagId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_OrphanElementTypes(ulong owner, out ulong outOrphaned);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_NotifyReloadCompleted(ulong owner);

        private static unsafe void EnsureCallbacksInstalled()
        {
            lock (s_Lock)
            {
                if (s_CallbacksInstalled)
                    return;
                if (GE_UI_SetElementTypeCallbacks(&CreateTrampoline, &ApplyAttributeTrampoline,
                                                  &ReleaseTrampoline,
                                                  &PerformRegistrationsTrampoline) == 0)
                {
                    s_CallbacksInstalled = true;
                }
            }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static nint CreateTrampoline(ulong tagId, ulong instanceId)
        {
            try
            {
                TypeBinding? binding;
                lock (s_Lock)
                {
                    if (!s_ByTagId.TryGetValue(tagId, out binding))
                        return 0;
                }

                // The element the constructor is being run over. Cleared in the finally so a
                // throwing constructor cannot leave it set for the next unrelated `new`.
                ulong previous = Ui.Element.ConstructingInstanceId;
                Ui.Element.ConstructingInstanceId = instanceId;
                object instance;
                try
                {
                    instance = Activator.CreateInstance(binding.Type)!;
                }
                finally
                {
                    Ui.Element.ConstructingInstanceId = previous;
                }

                // STRONG, deliberately: the native element owns its instance, and the identity
                // table's references are weak. Released at Orphan and at element destruction,
                // which is what keeps this from pinning the context forever.
                return GCHandle.ToIntPtr(GCHandle.Alloc(instance));
            }
            catch (Exception ex)
            {
                // A throwing constructor is a Faulted element, not a dead engine. The native
                // side reads 0 and says so on screen and in the log.
                UiElementTypeLog.Report($"constructing a UI element type failed: {ex}");
                return 0;
            }
        }


        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static unsafe void ApplyAttributeTrampoline(nint handle, byte* name, byte* value)
        {
            try
            {
                if (handle == 0 || name == null)
                    return;
                if (GCHandle.FromIntPtr(handle).Target is not Ui.Element element)
                    return;

                string attrName = Marshal.PtrToStringUTF8((nint)name) ?? string.Empty;
                string attrValue = value != null ? (Marshal.PtrToStringUTF8((nint)value) ?? string.Empty)
                                                 : string.Empty;
                if (attrName.Length == 0)
                    return;

                TypeBinding? binding = FindBindingFor(element.GetType());
                if (binding == null || !binding.Members.TryGetValue(attrName, out MemberInfo? member))
                    return;

                UiAttributeBinder.Assign(element, member, attrValue);
            }
            catch (Exception ex)
            {
                UiElementTypeLog.Report($"applying a .uxml attribute to a UI element type failed: {ex}");
            }
        }


        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ReleaseTrampoline(nint handle)
        {
            try
            {
                if (handle == 0)
                    return;
                GCHandle gch = GCHandle.FromIntPtr(handle);
                if (gch.IsAllocated)
                    gch.Free();
            }
            catch
            {
                // Nothing useful to do at a release, and nothing may cross the boundary.
            }
        }

        private static TypeBinding? FindBindingFor(Type instanceType)
        {
            lock (s_Lock)
            {
                foreach (KeyValuePair<ulong, TypeBinding> kv in s_ByTagId)
                {
                    if (kv.Value.Type == instanceType)
                        return kv.Value;
                }
            }
            return null;
        }
    }
}
