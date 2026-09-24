//! piplugin example - the C ABI from Rust  [roadmap ECO-06]
//!
//! The framework promises "pure C ABI": no C++ types, no name mangling, no
//! runtime to link against. This program is the proof for Rust - it loads the
//! framework DLL and a plugin, drives the COM-style lifecycle, and builds a HOST
//! OBJECT in Rust (a `#[repr(C)]` struct of `extern "system"` functions) to hand
//! to `pi_plugin_factory_create_instance`.
//!
//! No external crates: the loader is `kernel32` through `extern "system"`, and
//! everything else is plain FFI. That is deliberate - if this needed a binding
//! generator, the "any language" promise would be weaker than advertised.
//!
//! Run from the repository root (the plugin must sit next to piplugind.dll):
//!
//!     cargo run --manifest-path examples/ffi/rust/Cargo.toml -- bin/Debug/pi_plugin_test_plugin_imgui.dll
//!
//! Exit code 0 = every step succeeded.

use std::env;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

// ---------------------------------------------------------------------------
// ABI types (mirrors include/piplugin/*.h). PI_CALL is __stdcall on Windows,
// which is exactly Rust's extern "system".
// ---------------------------------------------------------------------------
type PiResult = i32;
const PI_OK: PiResult = 0;
const PI_E_NOINTERFACE: PiResult = -2;

#[repr(C)]
#[derive(Copy, Clone)]
struct PiGuid {
    data1: u32,
    data2: u16,
    data3: u16,
    data4: [u8; 8],
}

const fn guid(data1: u32, data2: u16, data3: u16, data4: [u8; 8]) -> PiGuid {
    PiGuid { data1, data2, data3, data4 }
}

// Framework IIDs (src/pi_plugin_unknown.c); plugin/app IIDs are random UUIDs.
const PI_IID_UNKNOWN: PiGuid = guid(0x0000_0000, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46]);
const PI_PLUGIN_IID_PLUGIN_FACTORY: PiGuid = guid(0x0000_0001, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46]);
const PI_PLUGIN_IID_HOST_SERVICES: PiGuid = guid(0x0000_0010, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46]);

#[repr(C)]
struct IPiUnknownVtbl {
    pi_query_interface: extern "system" fn(*mut c_void, *const PiGuid, *mut *mut c_void) -> PiResult,
    pi_add_ref: extern "system" fn(*mut c_void) -> u32,
    pi_release: extern "system" fn(*mut c_void) -> u32,
}

#[repr(C)]
struct PiPluginCapability {
    iid: PiGuid,
    flags: u32,
}

#[repr(C)]
struct PiPluginProperty {
    key: *const c_char,
    value: *const c_char,
}

#[repr(C)]
struct PiPluginDescriptor {
    name: *const c_char,
    vendor: *const c_char,
    version: *const c_char,
    category: *const c_char,
    api_version: u32,
    capabilities: *const PiPluginCapability,
    capability_count: u32,
    properties: *const PiPluginProperty,
    property_count: u32,
}

#[repr(C)]
struct IPiPluginFactoryVtbl {
    base: IPiUnknownVtbl,
    pi_plugin_get_descriptor: extern "system" fn(*mut c_void) -> *const PiPluginDescriptor,
    pi_plugin_get_class_count: extern "system" fn(*mut c_void) -> u32,
    pi_plugin_get_class_guid: extern "system" fn(*mut c_void, u32, *mut PiGuid) -> PiResult,
    pi_plugin_create_instance: extern "system" fn(*mut c_void, *const PiGuid, *mut c_void,
                                            *mut *mut c_void) -> PiResult,
}

#[repr(C)]
struct IPiPluginBaseVtbl {
    base: IPiUnknownVtbl,
    pi_plugin_initialize: extern "system" fn(*mut c_void, *mut c_void) -> PiResult,
    pi_plugin_terminate: extern "system" fn(*mut c_void) -> PiResult,
    pi_plugin_get_view: extern "system" fn(*mut c_void, *mut *mut c_void) -> PiResult,
}

// ---------------------------------------------------------------------------
// A host services object, implemented IN RUST.
//
// The framework only ever calls through this vtable, so a Rust struct with
// extern "system" methods is a legal host - that is the whole point of a C ABI.
// ---------------------------------------------------------------------------
#[repr(C)]
struct RustHost {
    lp_vtbl: *const IPiPluginHostServicesVtbl,
    posts: u32,
}

#[repr(C)]
struct IPiPluginHostServicesVtbl {
    base: IPiUnknownVtbl,
    pi_plugin_host_alloc: extern "system" fn(*mut c_void, usize) -> *mut c_void,
    pi_plugin_host_free: extern "system" fn(*mut c_void, *mut c_void),
    pi_plugin_host_post_message:
        extern "system" fn(*mut c_void, u32, usize, isize),
}

extern "system" fn host_qi(this: *mut c_void, iid: *const PiGuid, out: *mut *mut c_void) -> PiResult {
    if out.is_null() {
        return -3;
    }
    unsafe {
        let wanted = (*iid).data1;
        if wanted == PI_IID_UNKNOWN.data1 || wanted == PI_PLUGIN_IID_HOST_SERVICES.data1 {
            *out = this;
            return PI_OK;
        }
        *out = std::ptr::null_mut();
    }
    PI_E_NOINTERFACE
}

extern "system" fn host_add_ref(_this: *mut c_void) -> u32 { 1 }
extern "system" fn host_release(_this: *mut c_void) -> u32 { 0 }

extern "system" fn host_alloc(_this: *mut c_void, size: usize) -> *mut c_void {
    // A real host would use its own allocator; this one keeps the block size in a
    // 16-byte header so that host_free can release it again. Both calls go through
    // THIS object, so the allocator never has to match the plugin's CRT.
    unsafe {
        let total = size + 16;
        let layout = std::alloc::Layout::from_size_align_unchecked(total, 16);
        let raw = std::alloc::alloc(layout);
        if raw.is_null() {
            return std::ptr::null_mut();
        }
        *(raw as *mut usize) = total;
        raw.add(16) as *mut c_void
    }
}

extern "system" fn host_free(_this: *mut c_void, ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let raw = (ptr as *mut u8).sub(16);
        let total = *(raw as *mut usize);
        let layout = std::alloc::Layout::from_size_align_unchecked(total, 16);
        std::alloc::dealloc(raw, layout);
    }
}

extern "system" fn host_post(this: *mut c_void, _msg: u32, _wparam: usize, _lparam: isize) {
    unsafe {
        let host = this as *mut RustHost;
        if !host.is_null() {
            (*host).posts += 1;
        }
    }
}

static HOST_VTBL: IPiPluginHostServicesVtbl = IPiPluginHostServicesVtbl {
    base: IPiUnknownVtbl {
        pi_query_interface: host_qi,
        pi_add_ref: host_add_ref,
        pi_release: host_release,
    },
    pi_plugin_host_alloc: host_alloc,
    pi_plugin_host_free: host_free,
    pi_plugin_host_post_message: host_post,
};

// ---------------------------------------------------------------------------
// kernel32 (no crates)
// ---------------------------------------------------------------------------
extern "system" {
    fn LoadLibraryA(name: *const c_char) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
}

type EntryProc = extern "system" fn(*mut *mut c_void) -> PiResult;
type ModuleLoadProc = extern "system" fn(*const c_char) -> *mut c_void;
type ModuleGetFactoryProc = extern "system" fn(*mut c_void, *mut *mut c_void) -> PiResult;
type ModuleUnloadProc = extern "system" fn(*mut c_void);

fn sym<T>(module: *mut c_void, name: &str) -> T {
    let cname = CString::new(name).unwrap();
    let addr = unsafe { GetProcAddress(module, cname.as_ptr()) };
    assert!(!addr.is_null(), "missing export {name}");
    unsafe { std::mem::transmute_copy(&addr) }
}

fn check(condition: bool, what: &str) {
    println!("  {} {what}", if condition { "ok  " } else { "FAIL" });
    if !condition {
        std::process::exit(1);
    }
}

fn cstr_or_empty(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned() }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let plugin_arg = args.get(1).cloned().unwrap_or_else(|| {
        Path::new("bin").join("Debug").join("pi_plugin_test_plugin_imgui.dll").to_string_lossy().into_owned()
    });
    let plugin_path = PathBuf::from(&plugin_arg);
    if !plugin_path.exists() {
        println!("plugin not found: {plugin_arg}");
        println!("build first (cmake --build --preset conan-debug) or pass a path");
        std::process::exit(1);
    }

    println!("== piplugin FFI demo (Rust) ==");
    println!("plugin: {plugin_arg}\n");

    let bin_dir = plugin_path.parent().map(Path::to_path_buf).unwrap_or_default();
    let core_path = ["piplugind.dll", "piplugin.dll", "libpiplugin.so", "libpiplugin.dylib"]
        .iter()
        .map(|n| bin_dir.join(n))
        .find(|p| p.exists());
    let Some(core_path) = core_path else {
        println!("framework core not found next to the plugin in {}", bin_dir.display());
        std::process::exit(1);
    };

    let core_c = CString::new(core_path.to_string_lossy().as_bytes()).unwrap();
    let core = unsafe { LoadLibraryA(core_c.as_ptr()) };
    check(!core.is_null(), "load the framework core");
    let module_load: ModuleLoadProc = sym(core, "pi_plugin_module_load");
    let module_get_factory: ModuleGetFactoryProc = sym(core, "pi_plugin_module_get_factory");
    let module_unload: ModuleUnloadProc = sym(core, "pi_plugin_module_unload");

    // 1) load the plugin THROUGH the framework (the loader owns the factory
    //    reference and knows the unload order)
    println!("- load");
    let plugin_c = CString::new(plugin_path.to_string_lossy().as_bytes()).unwrap();
    let module = module_load(plugin_c.as_ptr());
    check(!module.is_null(), "pi_plugin_module_load");

    let mut factory: *mut c_void = std::ptr::null_mut();
    let hr = module_get_factory(module, &mut factory);
    check(hr == PI_OK && !factory.is_null(), &format!("pi_plugin_module_get_factory -> hr={hr}"));

    let factory_vtbl = unsafe { &*(*(factory as *const *const IPiPluginFactoryVtbl)) };

    // 2) QueryInterface, COM style
    println!("\n- QueryInterface");
    let mut out: *mut c_void = std::ptr::null_mut();
    let hr = (factory_vtbl.base.pi_query_interface)(factory, &PI_PLUGIN_IID_PLUGIN_FACTORY, &mut out);
    check(hr == PI_OK && !out.is_null(), &format!("QI(PI_PLUGIN_IID_PLUGIN_FACTORY) -> hr={hr}"));
    check(out == factory, "the factory answers with a stable identity");

    out = std::ptr::null_mut();
    let hr = (factory_vtbl.base.pi_query_interface)(factory, &PI_PLUGIN_IID_HOST_SERVICES, &mut out);
    check(hr == PI_E_NOINTERFACE && out.is_null(),
          &format!("QI(unknown IID) -> PI_E_NOINTERFACE and *out = NULL (hr={hr})"));

    // 3) descriptor
    println!("\n- descriptor");
    let desc = (factory_vtbl.pi_plugin_get_descriptor)(factory);
    check(!desc.is_null(), "pi_plugin_get_descriptor");
    let d = unsafe { &*desc };
    println!("     name={} vendor={} version={} api=0x{:08X} capabilities={} properties={}",
             cstr_or_empty(d.name), cstr_or_empty(d.vendor), cstr_or_empty(d.version),
             d.api_version, d.capability_count, d.property_count);
    check(d.api_version != 0, "api_version is a real version");
    for i in 0..d.property_count as usize {
        let p = unsafe { &*d.properties.add(i) };
        println!("     property {} = {}", cstr_or_empty(p.key), cstr_or_empty(p.value));
    }

    // 4) create an instance with a HOST OBJECT BUILT IN RUST
    println!("\n- create / initialize / terminate");
    let mut host = RustHost { lp_vtbl: &HOST_VTBL, posts: 0 };
    let host_ptr = &mut host as *mut RustHost as *mut c_void;

    let mut class_guid = PiGuid { data1: 0, data2: 0, data3: 0, data4: [0; 8] };
    let hr = (factory_vtbl.pi_plugin_get_class_guid)(factory, 0, &mut class_guid);
    check(hr == PI_OK, "pi_plugin_get_class_guid(0)");

    let mut plugin: *mut c_void = std::ptr::null_mut();
    let hr = (factory_vtbl.pi_plugin_create_instance)(factory, &class_guid, host_ptr, &mut plugin);
    check(hr == PI_OK && !plugin.is_null(), &format!("pi_plugin_create_instance -> hr={hr}"));

    let base_vtbl = unsafe { &*(*(plugin as *const *const IPiPluginBaseVtbl)) };
    let hr = (base_vtbl.pi_plugin_initialize)(plugin, host_ptr);
    check(hr == PI_OK, &format!("pi_plugin_initialize -> hr={hr}"));
    let hr = (base_vtbl.pi_plugin_terminate)(plugin);
    check(hr == PI_OK, &format!("pi_plugin_terminate -> hr={hr}"));

    let rc = (base_vtbl.base.pi_release)(plugin);
    check(rc == 0, &format!("release(plugin) -> refcount {rc}"));

    // Drop our factory reference: a plugin with a static factory keeps one of its
    // own for the module's lifetime, which is why hosts release theirs and then
    // unload the module.
    let rc = (factory_vtbl.base.pi_release)(factory);
    check(rc >= 1, &format!("release(our factory ref) -> refcount {rc}"));

    println!("\n- unload / reload");
    module_unload(module);
    let module2 = module_load(plugin_c.as_ptr());
    check(!module2.is_null(), "the plugin loads a second time after a clean unload");
    module_unload(module2);
    unsafe { FreeLibrary(core) };

    println!("\nRESULT: PASS");
}
