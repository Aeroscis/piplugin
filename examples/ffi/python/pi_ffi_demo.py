#!/usr/bin/env python3
"""
piplugin example - the C ABI from Python (ctypes)  [roadmap ECO-06]

The framework's promise is "pure C ABI": anything that can call a C function can
host a plugin. This file proves it for Python by doing what a real host does,
using nothing but the standard library:

  1. load the plugin DLL (ctypes.WinDLL / CDLL);
  2. find the exported entry point and get the factory;
  3. QueryInterface the factory (PI_IID_PLUGIN_FACTORY) - the COM-style step;
  4. read the descriptor (name / version / api_version / capabilities);
  5. BUILD A HOST OBJECT IN PYTHON - a vtable of Python callbacks - and create,
     initialize and terminate a plugin instance with it;
  6. release everything and unload the module.

Step 5 is the interesting one: the host side of the contract is just a struct of
function pointers, so it can be implemented in any language with C calling
conventions. Nothing here is piplugin-specific glue - it is all plain ctypes.

Run (from the repository root, after a build; the plugin must sit next to
piplugind.dll, i.e. in bin/<CONFIG>):

    python examples/ffi/python/pi_ffi_demo.py bin/Debug/pi_test_plugin_imgui.dll

Exits 0 when every step succeeded.
"""
import ctypes as C
import os
import sys

PI_OK = 0
PI_E_NOINTERFACE = -2
PI_INVALID_WINDOW = None

# ---------------------------------------------------------------------------
# Types (mirrors include/piplugin/pi_plugin_types.h)
# ---------------------------------------------------------------------------
CALL = C.WINFUNCTYPE if os.name == "nt" else C.CFUNCTYPE


class PiGuid(C.Structure):
    _fields_ = [("data1", C.c_uint32), ("data2", C.c_uint16),
                ("data3", C.c_uint16), ("data4", C.c_uint8 * 8)]


def guid(data1, data2, data3, tail):
    return PiGuid(data1, data2, data3, (C.c_uint8 * 8)(*tail))


# The framework's exported IIDs (src/pi_plugin_unknown.c); a plugin's own IIDs
# are random UUIDs, see docs/design/interfaces.md 5.1.
PI_IID_UNKNOWN = guid(0x00000000, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46])
PI_IID_PLUGIN_FACTORY = guid(0x00000001, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46])
PI_IID_HOST_SERVICES = guid(0x00000010, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46])

QiProc = CALL(C.c_int32, C.c_void_p, C.POINTER(PiGuid), C.POINTER(C.c_void_p))
AddRefProc = CALL(C.c_uint32, C.c_void_p)
ReleaseProc = CALL(C.c_uint32, C.c_void_p)


class IPiUnknownVtbl(C.Structure):
    _fields_ = [("pi_query_interface", QiProc),
                ("pi_add_ref", AddRefProc),
                ("pi_release", ReleaseProc)]


class IPiUnknown(C.Structure):
    _fields_ = [("lpVtbl", C.POINTER(IPiUnknownVtbl))]


class PiPluginCapability(C.Structure):
    _fields_ = [("iid", PiGuid), ("flags", C.c_uint32)]


class PiPluginProperty(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class PiPluginDescriptor(C.Structure):
    _fields_ = [("name", C.c_char_p), ("vendor", C.c_char_p),
                ("version", C.c_char_p), ("category", C.c_char_p),
                ("api_version", C.c_uint32),
                ("capabilities", C.POINTER(PiPluginCapability)),
                ("capability_count", C.c_uint32),
                ("properties", C.POINTER(PiPluginProperty)),
                ("property_count", C.c_uint32)]


GetDescriptorProc = CALL(C.POINTER(PiPluginDescriptor), C.c_void_p)
GetClassCountProc = CALL(C.c_uint32, C.c_void_p)
GetClassGuidProc = CALL(C.c_int32, C.c_void_p, C.c_uint32, C.POINTER(PiGuid))
CreateInstanceProc = CALL(C.c_int32, C.c_void_p, C.POINTER(PiGuid),
                          C.c_void_p, C.POINTER(C.c_void_p))


class IPiPluginFactoryVtbl(C.Structure):
    _fields_ = IPiUnknownVtbl._fields_ + [
        ("pi_get_descriptor", GetDescriptorProc),
        ("pi_get_class_count", GetClassCountProc),
        ("pi_get_class_guid", GetClassGuidProc),
        ("pi_create_instance", CreateInstanceProc),
    ]


PI_CAP_REQUIRED, PI_CAP_OPTIONAL, PI_CAP_PROVIDES = 1, 2, 4

# libc, for the host object's allocator (declared with real types: without
# restype/argtypes ctypes truncates pointers to 32 bits on 64-bit hosts).
# On Windows CDLL(None) is not a thing - open the universal CRT by name.
if os.name == "nt":
    try:
        _libc = C.CDLL("ucrtbase.dll")
    except OSError:                      # very old Windows
        _libc = C.CDLL("msvcrt.dll")
else:
    _libc = C.CDLL(None)
_libc.malloc.restype = C.c_void_p
_libc.malloc.argtypes = [C.c_size_t]
_libc.free.restype = None
_libc.free.argtypes = [C.c_void_p]

# ---------------------------------------------------------------------------
# A host services object, implemented IN PYTHON
#
# This is what a host hands to a plugin. The framework only ever calls through
# this struct, so a Python object with C-callable methods is a legal host.
# ---------------------------------------------------------------------------
AllocProc = CALL(C.c_void_p, C.c_void_p, C.c_size_t)
FreeProc = CALL(None, C.c_void_p, C.c_void_p)
PostProc = CALL(None, C.c_void_p, C.c_uint32, C.c_size_t, C.c_ssize_t)


class IPiHostServicesVtbl(C.Structure):
    _fields_ = IPiUnknownVtbl._fields_ + [
        ("pi_host_alloc", AllocProc),
        ("pi_host_free", FreeProc),
        ("pi_host_post_message", PostProc),
    ]


class PythonHost:
    """A minimal IPiHostServices: allocate/free/post_message + QueryInterface."""

    def __init__(self):
        self.messages = []

        # ctypes needs each Python callback wrapped in a C function pointer, and
        # the wrappers must stay referenced for as long as the vtable lives.
        self._qi_cb = QiProc(self._qi)
        self._addref_cb = AddRefProc(self._add_ref)
        self._release_cb = ReleaseProc(self._release)
        self._alloc_cb = AllocProc(self._alloc)
        self._free_cb = FreeProc(self._free)
        self._post_cb = PostProc(self._post)

        self._vtbl = IPiHostServicesVtbl(
            self._qi_cb, self._addref_cb, self._release_cb,
            self._alloc_cb, self._free_cb, self._post_cb)
        # A struct whose first member is the vtable pointer - exactly the layout
        # the framework expects.
        self._iface = IPiUnknown()
        self._iface.lpVtbl = C.cast(C.pointer(self._vtbl), C.POINTER(IPiUnknownVtbl))
        self._self_ptr = C.cast(C.pointer(self._iface), C.c_void_p)

    @property
    def pointer(self):
        return self._self_ptr

    def _qi(self, this, iid, out):
        if not out:
            return -3
        wanted = iid[0].data1
        if wanted in (PI_IID_UNKNOWN.data1, PI_IID_HOST_SERVICES.data1):
            out[0] = this
            self._add_ref(this)
            return PI_OK
        out[0] = None
        return PI_E_NOINTERFACE

    def _add_ref(self, this):
        return 1

    def _release(self, this):
        return 0

    def _alloc(self, this, size):
        return _libc.malloc(size)          # libc malloc: the plugin frees it the same way

    def _free(self, this, ptr):
        _libc.free(ptr)

    def _post(self, this, msg, wparam, lparam):
        self.messages.append((msg, wparam, lparam))


# ---------------------------------------------------------------------------
# The demo
# ---------------------------------------------------------------------------
def check(condition, what):
    print(("  ok   " if condition else "  FAIL ") + what)
    if not condition:
        raise SystemExit(1)


def main(argv):
    dll_path = argv[1] if len(argv) > 1 else os.path.join("bin", "Debug",
                                                           "pi_test_plugin_imgui.dll")
    if not os.path.exists(dll_path):
        print(f"plugin not found: {dll_path}")
        print("build first (cmake --build --preset conan-debug) or pass a path")
        return 1

    print("== piplugin FFI demo (Python / ctypes) ==")
    print(f"plugin: {dll_path}\n")

    # 0) the framework core itself: FFI users call its host API (pi_module_load,
    #    pi_factory_create_instance, ...) exactly like a C host would.
    loader = C.WinDLL if os.name == "nt" else C.CDLL
    bin_dir = os.path.dirname(os.path.abspath(dll_path))
    core_names = ["piplugind.dll", "piplugin.dll", "libpiplugin.so", "libpiplugin.dylib"]
    core_path = next((os.path.join(bin_dir, n) for n in core_names
                      if os.path.exists(os.path.join(bin_dir, n))), None)
    if not core_path:
        print(f"framework core not found next to the plugin in {bin_dir}")
        return 1
    core = loader(core_path)
    core.pi_module_load.restype = C.c_void_p
    core.pi_module_load.argtypes = [C.c_char_p]
    core.pi_module_get_factory.restype = C.c_int32
    core.pi_module_get_factory.argtypes = [C.c_void_p, C.POINTER(C.c_void_p)]
    core.pi_module_get_load_error.restype = C.c_char_p
    core.pi_module_unload.restype = None
    core.pi_module_unload.argtypes = [C.c_void_p]

    # 1) load the module through the framework (not by hand: the loader owns the
    #    factory reference and knows the unload order)
    print("- load")
    module = core.pi_module_load(os.path.abspath(dll_path).encode())
    check(bool(module), "pi_module_load")

    factory = C.c_void_p()
    hr = core.pi_module_get_factory(module, C.byref(factory))
    check(hr == PI_OK and factory.value, f"pi_module_get_factory -> hr={hr}")

    factory_vtbl = C.cast(factory, C.POINTER(IPiUnknown)).contents.lpVtbl

    # 2) QueryInterface the factory (COM-style, the framework's核心 step)
    print("\n- QueryInterface")
    out = C.c_void_p()
    hr = factory_vtbl.contents.pi_query_interface(factory, C.byref(PI_IID_PLUGIN_FACTORY),
                                                  C.byref(out))
    check(hr == PI_OK and out.value, f"QI(PI_IID_PLUGIN_FACTORY) -> hr={hr}")
    check(out.value == factory.value, "the factory answers with a stable identity")

    out = C.c_void_p()
    hr = factory_vtbl.contents.pi_query_interface(factory, C.byref(PI_IID_HOST_SERVICES),
                                                  C.byref(out))
    check(hr == PI_E_NOINTERFACE and not out.value,
          f"QI(unknown IID) -> PI_E_NOINTERFACE and *out = NULL (hr={hr})")

    # 3) read the descriptor
    print("\n- descriptor")
    fvtbl = C.cast(factory, C.POINTER(C.POINTER(IPiPluginFactoryVtbl))).contents.contents
    desc = fvtbl.pi_get_descriptor(factory)
    check(bool(desc), "pi_get_descriptor")
    d = desc.contents
    print(f"     name={d.name.decode()} vendor={d.vendor.decode()} "
          f"version={d.version.decode()} api=0x{d.api_version:08X} "
          f"capabilities={d.capability_count} properties={d.property_count}")
    check(d.api_version != 0, "api_version is a real version")
    if d.property_count:
        for i in range(d.property_count):
            p = d.properties[i]
            print(f"     property {p.key.decode()} = {p.value.decode()}")

    # 4) create an instance with a HOST OBJECT WE BUILT IN PYTHON
    print("\n- create / initialize / terminate")
    host = PythonHost()
    guid_out = PiGuid()
    hr = fvtbl.pi_get_class_guid(factory, 0, C.byref(guid_out))
    check(hr == PI_OK, "pi_get_class_guid(0)")

    plugin = C.c_void_p()
    hr = fvtbl.pi_create_instance(factory, C.byref(guid_out), host.pointer,
                                  C.byref(plugin))
    check(hr == PI_OK and plugin.value, f"pi_create_instance -> hr={hr}")

    # IPiPluginBase: initialize / terminate (the first three slots are IPiUnknown)
    base_vtbl = C.cast(plugin, C.POINTER(IPiUnknown)).contents.lpVtbl
    InitializeProc = CALL(C.c_int32, C.c_void_p, C.c_void_p)
    TerminateProc = CALL(C.c_int32, C.c_void_p)
    base = C.cast(base_vtbl, C.POINTER(C.c_void_p * 6)).contents
    initialize = InitializeProc(base[3])
    terminate = TerminateProc(base[4])

    hr = initialize(plugin, host.pointer)
    check(hr == PI_OK, f"pi_initialize -> hr={hr}")
    hr = terminate(plugin)
    check(hr == PI_OK, f"pi_terminate -> hr={hr}")

    rc = base_vtbl.contents.pi_release(plugin)
    check(rc == 0, f"release(plugin) -> refcount {rc}")

    # Drop OUR factory reference. It stays at >= 1 because a plugin with a static
    # factory holds a reference of its own for the module's lifetime - which is
    # exactly why hosts release what they took and then unload the module.
    rc = factory_vtbl.contents.pi_release(factory)
    check(rc >= 1, f"release(our factory ref) -> refcount {rc}")

    # 5) unload, then do it all again: a clean unload is what makes the second
    #    load possible, and it is the step hosts get wrong most often.
    print("\n- unload / reload")
    core.pi_module_unload(module)
    module2 = core.pi_module_load(os.path.abspath(dll_path).encode())
    check(bool(module2), "the plugin loads a second time after a clean unload")
    core.pi_module_unload(module2)

    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
