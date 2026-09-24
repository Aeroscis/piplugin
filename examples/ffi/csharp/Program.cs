// piplugin example - the C ABI from C#  [roadmap ECO-06]
//
// The framework promises "pure C ABI". This program is the proof for .NET: it
// loads the framework DLL and a plugin with P/Invoke, drives the COM-style
// lifecycle, and builds a HOST OBJECT in C# (a vtbl of [UnmanagedFunctionPointer]
// delegates, laid out in unmanaged memory) to hand to pi_plugin_factory_create_instance.
//
// Run from the repository root (the plugin must sit next to piplugind.dll):
//
//     dotnet run --project examples/ffi/csharp -- bin/Debug/pi_plugin_test_plugin_imgui.dll
//
// Exit code 0 = every step succeeded.

using System;
using System.IO;
using System.Runtime.InteropServices;

internal static class PiFfiDemo
{
    private const int PI_OK = 0;
    private const int PI_E_NOINTERFACE = -2;

    // -----------------------------------------------------------------------
    // ABI types (mirrors include/piplugin/*.h)
    // -----------------------------------------------------------------------
    [StructLayout(LayoutKind.Sequential)]
    private struct PiGuid
    {
        public uint data1;
        public ushort data2;
        public ushort data3;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
        public byte[] data4;
    }

    private static PiGuid Guid(uint data1, ushort data2, ushort data3, params byte[] tail)
    {
        var g = new PiGuid { data1 = data1, data2 = data2, data3 = data3, data4 = new byte[8] };
        Array.Copy(tail, g.data4, tail.Length);
        return g;
    }

    // Framework IIDs (src/pi_plugin_unknown.c); plugin/app IIDs are random UUIDs.
    private static readonly PiGuid PI_IID_UNKNOWN = Guid(0x00000000, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
    private static readonly PiGuid PI_PLUGIN_IID_PLUGIN_FACTORY = Guid(0x00000001, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
    private static readonly PiGuid PI_PLUGIN_IID_HOST_SERVICES = Guid(0x00000010, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);

    [StructLayout(LayoutKind.Sequential)]
    private struct PiPluginDescriptor
    {
        public IntPtr name;
        public IntPtr vendor;
        public IntPtr version;
        public IntPtr category;
        public uint api_version;
        public IntPtr capabilities;
        public uint capability_count;
        public IntPtr properties;
        public uint property_count;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PiPluginProperty
    {
        public IntPtr key;
        public IntPtr value;
    }

    // Vtables are read as raw pointers and turned into delegates on demand -
    // simpler and less error-prone than declaring every field as a delegate type.
    [StructLayout(LayoutKind.Sequential)]
    private struct IPiUnknownVtbl
    {
        public IntPtr qi, addref, release;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IPiPluginFactoryVtbl
    {
        public IPiUnknownVtbl base_;
        public IntPtr get_descriptor, get_class_count, get_class_guid, create_instance;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IPiPluginBaseVtbl
    {
        public IPiUnknownVtbl base_;
        public IntPtr initialize, terminate, get_view;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int QiProc(IntPtr self, IntPtr iid, out IntPtr outPtr);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int CreateInstanceProc(IntPtr self, IntPtr guid, IntPtr host, out IntPtr plugin);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetClassGuidProc(IntPtr self, uint index, IntPtr guid);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate IntPtr GetDescriptorProc(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int InitializeProc(IntPtr self, IntPtr host);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int TerminateProc(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate uint ReleaseProc(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate IntPtr AllocProc(IntPtr self, UIntPtr size);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void FreeProc(IntPtr self, IntPtr ptr);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void PostProc(IntPtr self, uint msg, UIntPtr wparam, IntPtr lparam);

    // -----------------------------------------------------------------------
    // kernel32
    // -----------------------------------------------------------------------
    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr LoadLibraryA(string name);

    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32")]
    private static extern bool FreeLibrary(IntPtr module);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate IntPtr ModuleLoadProc(string path);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int ModuleGetFactoryProc(IntPtr module, out IntPtr factory);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void ModuleUnloadProc(IntPtr module);

    private static T Sym<T>(IntPtr module, string name)
    {
        IntPtr addr = GetProcAddress(module, name);
        if (addr == IntPtr.Zero) throw new Exception($"missing export {name}");
        return Marshal.GetDelegateForFunctionPointer<T>(addr);
    }

    // -----------------------------------------------------------------------
    // A host services object, implemented IN C#.
    //
    // The delegates are held in static fields on purpose: the GC would otherwise
    // collect them and leave the vtbl pointing at freed thunks.
    // -----------------------------------------------------------------------
    private static readonly QiProc HostQi = HostQueryInterface;
    private static readonly ReleaseProc HostAddRef = _ => 1;
    private static readonly ReleaseProc HostRelease = _ => 0;
    private static readonly AllocProc HostAlloc = (_, size) => Marshal.AllocHGlobal((int)size);
    private static readonly FreeProc HostFree = (_, ptr) => Marshal.FreeHGlobal(ptr);
    private static readonly PostProc HostPost =
        (_, msg, wparam, _) => Console.WriteLine($"     [plugin message] msg=0x{msg:X4} wparam={wparam}");

    private static int HostQueryInterface(IntPtr self, IntPtr iid, out IntPtr outPtr)
    {
        outPtr = IntPtr.Zero;
        if (iid == IntPtr.Zero) return -3;
        uint wanted = (uint)Marshal.ReadInt32(iid);   // data1 is the first field
        if (wanted == PI_IID_UNKNOWN.data1 || wanted == PI_PLUGIN_IID_HOST_SERVICES.data1)
        {
            outPtr = self;
            return PI_OK;
        }
        return PI_E_NOINTERFACE;
    }

    /// <summary>Build the C-side view of our host object: [vtable*][refcount].</summary>
    private static IntPtr CreateHostObject()
    {
        IntPtr vtbl = Marshal.AllocHGlobal(IntPtr.Size * 6);
        Marshal.WriteIntPtr(vtbl, 0 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostQi));
        Marshal.WriteIntPtr(vtbl, 1 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostAddRef));
        Marshal.WriteIntPtr(vtbl, 2 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostRelease));
        Marshal.WriteIntPtr(vtbl, 3 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostAlloc));
        Marshal.WriteIntPtr(vtbl, 4 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostFree));
        Marshal.WriteIntPtr(vtbl, 5 * IntPtr.Size, Marshal.GetFunctionPointerForDelegate(HostPost));

        IntPtr obj = Marshal.AllocHGlobal(IntPtr.Size);
        Marshal.WriteIntPtr(obj, vtbl);
        return obj;
    }

    private static void Check(bool condition, string what)
    {
        Console.WriteLine((condition ? "  ok   " : "  FAIL ") + what);
        if (!condition) Environment.Exit(1);
    }

    private static string Str(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(p) ?? "";

    private static int Main(string[] args)
    {
        string pluginArg = args.Length > 0
            ? args[0]
            : Path.Combine("bin", "Debug", "pi_plugin_test_plugin_imgui.dll");
        if (!File.Exists(pluginArg))
        {
            Console.WriteLine($"plugin not found: {pluginArg}");
            Console.WriteLine("build first (cmake --build --preset conan-debug) or pass a path");
            return 1;
        }

        Console.WriteLine("== piplugin FFI demo (C# / P-Invoke) ==");
        Console.WriteLine($"plugin: {pluginArg}\n");

        string binDir = Path.GetDirectoryName(Path.GetFullPath(pluginArg)) ?? ".";
        string? corePath = null;
        foreach (string candidate in new[] { "piplugind.dll", "piplugin.dll" })
        {
            string p = Path.Combine(binDir, candidate);
            if (File.Exists(p)) { corePath = p; break; }
        }
        if (corePath == null)
        {
            Console.WriteLine($"framework core not found next to the plugin in {binDir}");
            return 1;
        }

        IntPtr core = LoadLibraryA(corePath);
        Check(core != IntPtr.Zero, "load the framework core");
        var moduleLoad = Sym<ModuleLoadProc>(core, "pi_plugin_module_load");
        var moduleGetFactory = Sym<ModuleGetFactoryProc>(core, "pi_plugin_module_get_factory");
        var moduleUnload = Sym<ModuleUnloadProc>(core, "pi_plugin_module_unload");

        // 1) load the plugin THROUGH the framework
        Console.WriteLine("- load");
        string pluginFull = Path.GetFullPath(pluginArg);
        IntPtr module = moduleLoad(pluginFull);
        Check(module != IntPtr.Zero, "pi_plugin_module_load");

        int hr = moduleGetFactory(module, out IntPtr factory);
        Check(hr == PI_OK && factory != IntPtr.Zero, $"pi_plugin_module_get_factory -> hr={hr}");

        var factoryVtbl = Marshal.PtrToStructure<IPiPluginFactoryVtbl>(
            Marshal.ReadIntPtr(factory));
        var qi = Marshal.GetDelegateForFunctionPointer<QiProc>(factoryVtbl.base_.qi);
        var release = Marshal.GetDelegateForFunctionPointer<ReleaseProc>(factoryVtbl.base_.release);

        // 2) QueryInterface, COM style
        Console.WriteLine("\n- QueryInterface");
        IntPtr iidPtr = Marshal.AllocHGlobal(Marshal.SizeOf<PiGuid>());
        Marshal.StructureToPtr(PI_PLUGIN_IID_PLUGIN_FACTORY, iidPtr, false);
        hr = qi(factory, iidPtr, out IntPtr outPtr);
        Check(hr == PI_OK && outPtr != IntPtr.Zero, $"QI(PI_PLUGIN_IID_PLUGIN_FACTORY) -> hr={hr}");
        Check(outPtr == factory, "the factory answers with a stable identity");

        Marshal.StructureToPtr(PI_PLUGIN_IID_HOST_SERVICES, iidPtr, false);
        hr = qi(factory, iidPtr, out outPtr);
        Check(hr == PI_E_NOINTERFACE && outPtr == IntPtr.Zero,
              $"QI(unknown IID) -> PI_E_NOINTERFACE and *out = NULL (hr={hr})");

        // 3) descriptor
        Console.WriteLine("\n- descriptor");
        var getDescriptor = Marshal.GetDelegateForFunctionPointer<GetDescriptorProc>(factoryVtbl.get_descriptor);
        IntPtr descPtr = getDescriptor(factory);
        Check(descPtr != IntPtr.Zero, "pi_plugin_get_descriptor");
        var desc = Marshal.PtrToStructure<PiPluginDescriptor>(descPtr);
        Console.WriteLine($"     name={Str(desc.name)} vendor={Str(desc.vendor)} version={Str(desc.version)} " +
                          $"api=0x{desc.api_version:X8} capabilities={desc.capability_count} properties={desc.property_count}");
        Check(desc.api_version != 0, "api_version is a real version");
        for (uint i = 0; i < desc.property_count; ++i)
        {
            IntPtr propPtr = IntPtr.Add(desc.properties, (int)i * Marshal.SizeOf<PiPluginProperty>());
            var prop = Marshal.PtrToStructure<PiPluginProperty>(propPtr);
            Console.WriteLine($"     property {Str(prop.key)} = {Str(prop.value)}");
        }

        // 4) create an instance with a HOST OBJECT BUILT IN C#
        Console.WriteLine("\n- create / initialize / terminate");
        IntPtr host = CreateHostObject();
        var getClassGuid = Marshal.GetDelegateForFunctionPointer<GetClassGuidProc>(factoryVtbl.get_class_guid);
        IntPtr classGuidPtr = Marshal.AllocHGlobal(Marshal.SizeOf<PiGuid>());
        hr = getClassGuid(factory, 0, classGuidPtr);
        Check(hr == PI_OK, "pi_plugin_get_class_guid(0)");

        var createInstance = Marshal.GetDelegateForFunctionPointer<CreateInstanceProc>(factoryVtbl.create_instance);
        hr = createInstance(factory, classGuidPtr, host, out IntPtr plugin);
        Check(hr == PI_OK && plugin != IntPtr.Zero, $"pi_plugin_create_instance -> hr={hr}");

        var baseVtbl = Marshal.PtrToStructure<IPiPluginBaseVtbl>(Marshal.ReadIntPtr(plugin));
        var initialize = Marshal.GetDelegateForFunctionPointer<InitializeProc>(baseVtbl.initialize);
        var terminate = Marshal.GetDelegateForFunctionPointer<TerminateProc>(baseVtbl.terminate);
        var pluginRelease = Marshal.GetDelegateForFunctionPointer<ReleaseProc>(baseVtbl.base_.release);

        hr = initialize(plugin, host);
        Check(hr == PI_OK, $"pi_plugin_initialize -> hr={hr}");
        hr = terminate(plugin);
        Check(hr == PI_OK, $"pi_plugin_terminate -> hr={hr}");

        uint rc = pluginRelease(plugin);
        Check(rc == 0, $"release(plugin) -> refcount {rc}");

        // 5) unload, then load again: a clean unload is what makes that possible
        Console.WriteLine("\n- unload / reload");
        moduleUnload(module);
        IntPtr module2 = moduleLoad(pluginFull);
        Check(module2 != IntPtr.Zero, "the plugin loads a second time after a clean unload");
        moduleUnload(module2);

        FreeLibrary(core);
        Console.WriteLine("\nRESULT: PASS");
        return 0;
    }
}
