**English** | [简体中文](../README.md)

# piplugin · a pure C plugin framework

[![CI](https://github.com/Aeroscis/piplugin/actions/workflows/ci.yml/badge.svg)](https://github.com/Aeroscis/piplugin/actions/workflows/ci.yml)

> **Two homes (deliberate)**: the primary repository is on
> [Gitee](https://gitee.com/Aeroscis/piplugin) (which the CMake `HOMEPAGE_URL` and
> the Conan `url` point to); [GitHub](https://github.com/Aeroscis/piplugin) is the
> mirror, and CI (Actions, the badge above) runs on the mirror.

Host and plugin talk through a **pure C ABI only** (COM-style vtables plus LV2-style capability
negotiation), so a plugin can be written in any language and built with any toolchain, and the host
never has to know whether the plugin uses Qt, imgui or raw Win32 inside.

> This is the English README. Design notes live in [`design/`](design/), tutorials in
> [`tutorial/`](tutorial/); the Chinese version is [`../README.md`](../README.md).

---

## Why it exists

Existing plugin schemes each tie you to one thing:

- **`QPluginLoader` and other C++ plugin systems**: host and plugin must be built with the same
  compiler, the same standard library and the same C++ ABI. Change the compiler, or take an MSVC
  update, and the plugin no longer loads.
- **LV2 / VST**: strong capability negotiation, but aimed at one domain — audio processing.
- **This library**: takes LV2's capability model to the **general** case — any host that loads
  external modules at runtime, where those modules may bring their own GUI — and leaves the
  differences between UI toolkits to **adapter kits**.

## Features

| Feature | What it means |
|---|---|
| **Pure C ABI** | Interfaces are structs of function pointers (`PI_CALL`, `__stdcall` on Windows) with no C++ types; Rust / C# / Python FFI can declare the same structs |
| **COM-style lifetime** | `QueryInterface` / `AddRef` / `Release`, with `IPiUnknown` as the root; a published interface is only ever added to, never changed |
| **LV2-style capability negotiation** | The descriptor declares `REQUIRED` / `OPTIONAL` / `PROVIDES` per GUID, and the host decides to accept or reject **before instantiating** |
| **Gates in both directions** | The host checks what the plugin requires *and* requires what its own ecosystem needs from the plugin — an app can define a protocol without forking the framework |
| **Version gate** | The plugin declares `api_version`; a different major, or a plugin newer than the host, is rejected before `create_instance` (`PI_E_VERSIONMISMATCH`) |
| **Same API GUI or headless** | A headless host simply does not expose `IPiPluginHostUI`; plugins that marked it OPTIONAL degrade to running without a UI |
| **Embeds any GUI toolkit** | The plugin hands out its native window handle and the host embeds it in its own container; adapter kits absorb the Qt / imgui differences |
| **Zero-dependency core** | The core needs only the platform's own libraries — no C++ runtime, no vendored third-party code |
| **Host-side kits** | Optional static libraries that fix in place loading, the gates and the seven-step unload sequence (`piplugin_host`), plus Qt and D3D11 embedding glue (`piplugin_host_qt`, `piplugin_host_dx11`) |
| **One-command acceptance** | `ctest` for the core regression; `--cycles` makes a host run any plugin DLL through its whole lifecycle and a resize round trip |

## Platform support

| Platform | Core | UI adapter kits | UI embedding | Verified in this repo |
|---|---|---|---|---|
| **Windows** | full | full (imgui / Qt) | full (Win32 child window + flip-model swap chain) | **yes** — `ctest`, the conformance harness and the pixel-level resize regression all pass |
| Linux | **proven by CI** (core + host kits, gcc/clang) | expected to compile (never built) | **not implemented** (X11 XEmbed is roadmap FUT-01) | **partly** — the CI `linux` job builds the core and the host kits and runs the C++ layer on Linux; plugin DLLs and the headless host are not built off Windows yet (`docs/todo/platform.md` #3) |
| macOS | expected to compile | expected to compile | **not implemented** (NSView is roadmap FUT-02) | **no** (no CI, never run by hand) |

**No over-promising**: under v0.x only Windows is verified by automation *in full*. On Linux CI
builds the core and the host kits and runs the C++ layer (the `linux` job in
`.github/workflows/ci.yml`); the UI adapter kits, the plugin DLLs and the headless host have
never been built there, and "expected to compile" is exactly about those parts — the code
carries the platform branches it needs (thread identity, dynamic loading, symbol visibility)
and nothing more. The full statement is
in [`design/interface-freeze-review.md`](design/interface-freeze-review.md), section 5.

## Getting started

### With Conan (recommended — pulls in imgui)

```bash
conan install . --build=missing          # the first run builds imgui locally; a few minutes
cmake --preset conan-default
cmake --build --preset conan-debug
ctest --test-dir build -C Debug --output-on-failure
```

### With plain CMake (only MSVC / the Windows SDK needed)

No Conan: the repository ships `CMakePresets.json` with generic presets (no Conan-generated ones),
and targets that need Qt or imgui disable themselves with a hint, leaving the core library, the host
kits, the headless host and the console tests.

```powershell
cmake --preset default              # build tree build/generic, clear of Conan's build/
cmake --build --preset default      # Debug
ctest --preset default
```

- `-DPI_QT_PREFIX=<path>` (or `-DQt5_DIR=<path>/lib/cmake/Qt5`) still works, as it does with Conan;
- on a non-Windows machine use `cmake --preset default-unix` (Ninja);
- the default install prefix is `<root>/build/install`, the same in all three flows (Conan, plain
  CMake, cpack);
- without a preset, the equivalent configure is
  `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DPI_BUILD_ADAPTERS=OFF`.

### See it run

```powershell
cd bin\Debug
.\pi_test_host_headless.exe pi_test_plugin_qt.dll     # headless: negotiation + full lifecycle
.\pi_test_host_imgui.exe  pi_test_plugin_qt.dll       # imgui host embedding a Qt plugin
.\pi_test_host_qt.exe     pi_test_plugin_imgui.dll    # Qt host embedding an imgui plugin
```

Want to write one yourself? [`../examples`](../examples/README.md) has runnable minimal demos
(host, imgui plugin, Qt plugin, service plugin, specialised app), each a standalone "three steps"
project:

```powershell
.\pi_example_minimal_host.exe pi_example_plugin_imgui.dll
.\pi_example_specialized_app.exe pi_example_specialized_plugin.dll pi_example_service.dll
```

Writing a host: [`tutorial/write-host.md`](tutorial/write-host.md) (the host kits are documented
there too). Writing a plugin: [`tutorial/write-plugin.md`](tutorial/write-plugin.md). Adapter kits:
[`tutorial/adapters.md`](tutorial/adapters.md). If the HOST is itself a Qt program, read
[`tutorial/qt-host-direct.md`](tutorial/qt-host-direct.md) *before* reaching for the Qt kit
(`examples/qt_host_direct/` is the runnable version).

## Architecture

```
Host
  ├─ GUI host        (imgui / Qt / raw Win32)
  ├─ Headless host   (task server / CLI; no IPiPluginHostUI, plugins degrade)
  └─ any host
        │  pi_plugin_module_load / pi_plugin_host_create_plugin
        ▼  dynamic loading (LoadLibrary / dlopen)
Plugin DLL (.dll / .so / .dylib)
  └─ pi_plugin_entry() -> IPiPluginFactory
       ├─ PiPluginDescriptor (name / version / capabilities)
       └─ CreateInstance -> IPiPluginBase
            ├─ IPiPluginView  (GUI plugins: attach / on_idle / on_resize)
            └─ IPiPluginService     (headless / service plugins)
```

The full architecture, interface family and threading model are in
[`design/architecture.md`](design/architecture.md) and
[`design/interfaces.md`](design/interfaces.md).

| Directory | Contents |
|---|---|
| `include/piplugin/` | public headers (`pi_plugin.h` is the master include) |
| `src/` | the core, in C |
| `adapters/` | plugin-side UI adapter kits (`qt/` SHARED, `imgui/` STATIC) |
| `host_kits/` | host-side kits (`core/` session, `events/` event routing, `qt/` and `dx11/` embedding glue) |
| `examples/` | minimal runnable demos (host / imgui plugin / Qt plugin / service plugin / specialised app / FFI / plugin discovery — see [`examples/README.md`](../examples/README.md)) |
| `scripts/` | one-command acceptance entry points (`verify.ps1` and friends) |
| `tests/` | test hosts, test plugins and the unit suite |
| `docs/` | design notes and tutorials |

## Where it sits

| | This library | `QPluginLoader`-style C++ plugins | LV2 / VST |
|---|---|---|---|
| Plugin language | any (pure C ABI) | same C++ ABI required | any (C ABI) |
| Compiler upgrade | unaffected | everything must be rebuilt | unaffected |
| Capability negotiation | GUID declarations, gated before instantiation | none | GUID declarations (this library borrows the idea) |
| Host's own GUI toolkit | embeds another toolkit's native window | usually requires the same toolkit | aimed at audio plugin GUIs |
| Domain | general: any host loading external modules | desktop applications | audio processing |
| Dependencies | none (the core has none) | none | depends on the host |

## Tests and acceptance

| Entry point | What it covers |
|---|---|
| `scripts/verify.ps1` | **every check in one command** (this is what CI calls, five of them): `ctest` + the conformance harness + the FFI examples (Python / Rust / C#) + the clang-format drift report (report-only) + the **documentation drift check** (enforced) |
| `ctest -C Debug` | 21 cases: the core unit suite `unit` (210 assertions), `unit_threads` (70), `unit_cpp` (52, with a CRT leak assertion), headless smoke test, negative version-gate case |
| `scripts/run_selftest.ps1` | **conformance harness**: runs every given plugin DLL through `load → attach → idle → resize round trip → unload` and decides by exit code |
| `scripts/verify_resize_fix.ps1` | pixel-level regression for the resize fix (measures panel and plugin edges in screenshots) |

CI (GitHub Actions, [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) only installs
dependencies and builds on every push and pull request; the checks themselves are
`scripts/verify.ps1`, so they can be reproduced locally.

## Ecosystem

**The rule: passing `--cycles` (the conformance harness, exit code 0) is what puts you in this table.**

| Adapter kit | Side | Status |
|---|---|---|
| `piplugin_qt` | plugin-side: Qt widget plugins | shipped here |
| `piplugin_imgui` | plugin-side: immediate-mode UI plugins | shipped here |

Community adapters and plugins are welcome in this table; what delivery has to satisfy is in
[`design/conformance.md`](design/conformance.md).

## Versions and compatibility

- Currently **0.x**: the API can still move. A `y` release only fixes; an `x` release may change
  behaviour or the API, and every one of those changes gets a line in [`CHANGELOG.md`](../CHANGELOG.md).
- `1.0.0` is reserved for the moment the ABI is frozen: from then on vtables are only ever added to.
- A plugin declares `PI_PLUGIN_API_VERSION` in its descriptor and the host gates on it before
  instantiating. The rule is in [`design/interfaces.md`](design/interfaces.md), section 1.5.

## License and third parties

[MIT](../LICENSE) © 2026 Aeroscis.

Third-party code and dependencies (the vendored Dear ImGui Win32/DX11 backends, the imgui core from
Conan, and optionally Qt) are listed one by one in [`../CREDITS.md`](../CREDITS.md); their licenses
and copyrights stay with their authors.
