# Changelog

What changed, per version, newest first. Applications read this to know what a
version brings before they take it; the git history is the long version, this is
the one that fits in a pull-request description.

## How versions work

Semantic Versioning, in its pre-1.0 reading:

- **0.x.y** — the API is still moving. A `y` release only fixes; an `x` release
  may change behaviour or the API, and its entry says so. Nothing an application
  can see changes without a line in here.
- **1.0.0** — when the surface documented in
  [docs/design/interfaces.md](docs/design/interfaces.md) stops moving. From that
  point every vtbl is frozen: interfaces can only be added, never changed.

`PIPLUGIN_API_VERSION` (`include/piplugin/pi_plugin_types.h`) carries the same
`major.minor`. It is what a host compares against a plugin's `api_version` before
instantiating it, so bumping it here is also the statement: *plugins built
against this version are accepted by hosts of this version and newer, within the
same major.*

A release is one commit on `main` that bumps the version in `CMakeLists.txt` and
`conanfile.py` and adds the entry below it, tagged `vX.Y.Z`.

## [Unreleased]

### Changed

- **The Qt adapter kit is SHARED (APP-08).** `piplugin_qt` owns process-level
  state - the single `QApplication` and the live-view registry - so as a static
  library every Qt plugin DLL carried its own copy: a process that loaded two of
  them tried to construct a second `QApplication`, which Qt answers with
  `ASSERT failure in QCoreApplication: "there should be only one application
  object"` (reproduced while building this change; see
  `tests/test_host_multi`). As a DLL there is one copy per process and the second
  plugin reuses the `QApplication` the first one created. Two consequences for
  consumers: plugin deployments must ship `piplugin_qt<debug-suffix>.dll` next to
  the plugin (`PI_QT_API` now exports the kit's four functions explicitly, and the
  build deploys the DLL), and a plugin that tears its UI down from
  `pi_terminate()` should call the new `pi_qt_view_shutdown_owner(owner)` - it
  only touches ITS views, where the process-wide `pi_qt_view_shutdown()` (kept,
  now documented as a last-resort hammer) would reach into other Qt plugins that
  are still loaded and delete their widgets. The `QApplication` is destroyed by
  the last view that goes away, whoever that is.

### Added

- **The multi-plugin acceptance host (APP-08).** `tests/test_host_multi` is a
  deliberately plain Win32 host (no D3D, no resize loop, so the delicate
  rendering machinery in `tests/test_host` is not perturbed by it) that loads TWO
  different Qt plugin modules into two slots of one session, embeds each in its
  own container, drives both from its own loop, and unloads both. It asserts that
  each plugin's native window is a real, visible child of ITS container, and that
  both plugins' Qt timers keep posting (the two variants report on different
  message codes). `ctest` case `multi_plugin_qt_in_one_process`. To have two
  distinct modules from one source, `tests/test_plugin` now builds
  `pi_test_plugin_qt2.dll` from the same sources as `pi_test_plugin_qt.dll`,
  differing only in class GUID, display name and heartbeat code.
- **A service plugin and its headless acceptance run (APP-07).**
  `tests/test_plugin_service/` is a pure-C plugin that declares
  `PI_IID_SERVICE PROVIDES`, has no UI at all (`pi_get_view` answers
  `PI_E_NOINTERFACE`) and implements start/poll/get_status/stop - the interface
  existed since the beginning but nothing in the repository ever implemented
  it, so the headless story of the framework was documented and untested. It is
  also the reference for giving one plugin object two interfaces in C: the
  instance carries `IPiPluginBase` and QueryInterface hands out a small wrapper
  with the service vtable, the same containment pattern the framework uses for
  `IPiHostUI`. The headless test host loads it and asserts the whole lifecycle
  step by step (required option missing -> `PI_E_MISSINGCAPABILITY`, polls that
  must show up as messages the host receives, idempotent stop, poll-after-stop
  failing, `get_status(NULL)` rejected, and the extra stop the unload sequence
  performs), with a non-zero exit code on the first mismatch; `ctest` case
  `headless_host_service_lifecycle`. The scenario matrix in
  `docs/todo/tests.md` gained its "headless + service plugin" row.
- **Optional C++ RAII layer (`piplugin/pi_cpp.h`).** Hand-written AddRef/Release
  pairs are the easiest thing to get wrong in a COM-style C API, so C++ hosts
  and plugins can now use `PiPtr<T>` (destructor releases, move-only, `qi_to<U>()`
  with a `PiIidOf<T>` type-to-IID map, `put()`/`detach()`/`add_ref()`) and
  `PiUniqueModule` (RAII `pi_module_unload`, `load()` + `factory()`), plus
  `pi_cpp_destroy<T>` for the refcount destroy thunk that both test plugins used
  to define for themselves. It adds no ABI, no exported symbol and no runtime
  dependency, and it is deliberately **not** included by `pi_plugin.h`: the C
  umbrella stays pure C and the C++ layer is one explicit include. `PiPtr`
  *adopts* the reference the framework hands out (every returned interface
  pointer is already AddRef'd) instead of adding one more, which is what keeps a
  QI call site free of manual releases; the constructor is explicit and copying
  is deleted.
- Both test plugins and the Qt test host now use the layer, and the new
  `tests/unit_cpp` target (`ctest` name `unit_cpp`) covers PiPtr/PiUniqueModule
  semantics against a counted object and a real plugin module, with
  `_CrtDumpMemoryLeaks()` deciding the exit code in Debug builds - so "no leaks"
  is an assertion rather than a promise.
- **Composable host services (`pi_host_services_create_ex`) — channel B.** The
  default host object answers the framework's three IIDs and nothing else, so an
  app could not hand its plugins a service of its own (the mirror image of the
  app-defined *plugin* protocol the framework already supported). `create_ex`
  adds an extra-QI hook (`PiHostExtraQiProc`): every IID the framework does not
  recognise is forwarded to the app, which answers with an AddRef'd interface
  pointer. The plugin side needs **no new API** — it is an ordinary
  `QueryInterface()` on the host object it was already handed, and a plugin that
  does not know the service gets `PI_E_NOINTERFACE` and keeps running.
  `pi_host_services_create_default()` now just calls `create_ex()` with a NULL
  hook, so no-hook hosts take the identical code path. Both test hosts expose an
  app-defined service and both test plugins query it and call it; new `ctest`
  cases assert that round trip, and the unit suite pins the hook contract
  (forwarding, failure codes passed through, `*out` cleared on failure,
  framework IIDs never forwarded, NULL hook == `create_default`).

### Fixed

- **The test plugins were initialized twice.** `CreateInstance()` initialized
  the instance and the host then called `pi_initialize()` on it as well —
  which is the documented lifecycle, and what `pi_host_create_plugin()` and the
  host kit both do. The host pointer was therefore AddRef'd twice and the second
  `QueryInterface(PI_IID_HOST_UI)` overwrote (and leaked) the first wrapper.
  Both test plugins now make `Initialize()` idempotent, and
  `docs/tutorial/write-plugin.md` states that rule for plugin authors.

## [0.2.0] - 2026-09-22

**The first public release.**

### Framework core

- **Pure C, COM-style ABI.** Interfaces are plain C structs of function
  pointers (`PI_CALL`, `__stdcall` on Windows); no C++ types cross the boundary,
  so the ABI is stable across compilers and usable from Rust / C# / Python FFI.
  No dependencies beyond the platform's own libraries.
- **`IPiUnknown` root interface** with `QueryInterface` / `AddRef` / `Release`,
  plus `PiRefCountedBase` for plugin authors who want the standard refcount
  implementation (optional `destroy` callback, so C++ objects run their
  destructor).
- **Lifecycle**: `pi_module_load` → factory → descriptor → capability check →
  `create_instance` → `initialize` → optional view/service → `terminate` →
  release → `pi_module_unload`. The unload order is documented and enforced by
  the host kit, because getting it wrong unloads code that is still on the stack.
- **LV2-style capability negotiation.** A plugin's descriptor declares
  REQUIRED / OPTIONAL / PROVIDES per GUID; hosts inspect it *before*
  instantiating. `HEADLESS` hosts simply do not expose `IPiHostUI`, and plugins
  that mark it OPTIONAL keep working without a UI.

### Version gating

- `pi_api_version_compatible(host, plugin)` and the `api_version` checks in the
  host kit: a plugin whose `major` differs, or that is newer than the host, is
  rejected with `PI_E_VERSIONMISMATCH` **before** `create_instance` — previously
  the field was written by every plugin and read by nobody.

### Host-side kits (`host_kits/`)

- **L0 — `piplugin_host`** (static, pure C, no GUI): a session object that owns
  loading, the bidirectional capability gate (what the plugin REQUIRES and what
  the app REQUIRES the plugin to PROVIDE), instantiation, multi-plugin slots and
  the seven-step unload sequence. `inspect()` + `instantiate()` expose the
  pre-instantiation gate on its own; `drive_idle()` pumps the views. A log
  callback keeps the caller's diagnostics.
- **L1 — `piplugin_host_qt`** (static, Qt 5): `PiPluginEmbedArea`, an embed area
  the host creates, positions and styles; it only attaches, forwards resizes and
  drives idle. It binds `(session, slot)` rather than a raw view pointer, so an
  unloaded plugin cannot leave a dangling one.
- **L1 — `piplugin_host_dx11`** (static, Windows): the flip-model swap chain
  creation parameters that make an embedded child window survive presentation
  (`DXGI_SCALING_NONE` with both fallbacks, frame-latency waitable object,
  background colour), the grow-never-shrink resize policy, and the container /
  `WS_CLIPCHILDREN` window styles.

### Plugin-side UI adapter kits (`adapters/`)

- **`piplugin_imgui`** (static): immediate-mode plugins render inside the host's
  frame with an isolated ImGui context.
- **`piplugin_qt`** (static): Qt-based plugins get the module's single
  `QApplication`, embedding and event-loop pumping; the plugin supplies only a
  widget factory.

### Tests and tooling

- `ctest` runs the core unit suite, a headless-host smoke test and a negative
  version-gate case; GUI hosts ship `--cycles` as a conformance harness
  (`scripts/run_selftest.ps1`) plus a pixel-level resize regression
  (`scripts/verify_resize_fix.ps1`).
- CI on GitHub Actions: build, `ctest` and the conformance harness;
  clang-format drift is reported but not yet enforced.

### Known limitations (stated, not hidden)

- **Windows only as far as verified.** The framework and the adapter kits are
  written to compile on Linux and macOS, but nothing in this repository builds or
  runs there, and UI embedding on those platforms is not implemented.
- Pre-1.0: interfaces may still change between `x` releases; the freeze starts at
  1.0.0. See `docs/design/interface-freeze-review.md` for the audit and the
  items deliberately left for 1.0.
