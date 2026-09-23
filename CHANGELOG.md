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

`PI_PLUGIN_API_VERSION` (`include/piplugin/pi_plugin_types.h`) carries the same
`major.minor`. It is what a host compares against a plugin's `api_version` before
instantiating it, so bumping it here is also the statement: *plugins built
against this version are accepted by hosts of this version and newer, within the
same major.*

A release is one commit on `main` that bumps the version in `CMakeLists.txt` and
`conanfile.py` and adds the entry below it, tagged `vX.Y.Z`.

## [Unreleased]

### Added

- **FFI examples: the C ABI consumed from Python, Rust and C# (ECO-06).** "Pure C
  ABI" is a claim about *other* languages, so each example does the whole thing in
  its own language with no binding generator and no glue: load the framework DLL
  and an official test plugin, QueryInterface the factory (hit -> `PI_OK`, miss ->
  `PI_E_NOINTERFACE` with `*out == NULL`), read the descriptor (name, version,
  `api_version`, capabilities, properties), BUILD A HOST OBJECT IN THAT LANGUAGE (a
  table of C callbacks) and create/initialize/terminate an instance with it, then
  unload and load again - the clean-unload step hosts get wrong most often.
  `examples/ffi/python/` (ctypes), `examples/ffi/rust/` (no crates at all: the
  loader is `kernel32` through `extern "system"`, so it builds offline) and
  `examples/ffi/csharp/` (P/Invoke, vtbl built in unmanaged memory; the delegates
  live in static fields so the GC cannot collect the thunks).
  `scripts/verify_ffi.ps1` runs all three and is now check 3 of
  `scripts/verify.ps1`; a language whose toolchain is missing is reported as SKIP
  rather than failed, so a machine without cargo still verifies the other two.
- **The negative test set (ECO-08).** The four ways a plugin can be wrong now have
  automated assertions instead of a checklist. Three live in `tests/unit`: a
  missing DLL (`pi_module_load` returns NULL with the path in the message), a DLL
  without `pi_plugin_entry` (the test binary itself is used as that DLL, so the
  "does not export" path is exercised with a real module), and
  `pi_factory_create_instance` with an unknown class GUID - which checks the error
  code, the `*out = NULL` rule, the invalid-argument paths, and, importantly, a
  POSITIVE control with the real GUID so a run where everything fails cannot look
  like a pass. The fourth is a new test plugin that declares
  `PI_IID_HOST_UI (REQUIRED)`; the headless host must refuse it before
  instantiation, and ctest case `capability_gate_rejects_gui_required_plugin`
  asserts the refusal message ("plugin requires capability iid data1=0x00000011
  but this host does not provide it"). Switch `PI_BUILD_TEST_PLUGIN_GUIREQUIRED`
  (mirrored into `conanfile.py`).

- **`docs/design/adapter-spec.md` - the adapter kit contract (ECO-01).** What a UI
  adapter kit must do was spread across two READMEs and a lot of tribal knowledge.
  The spec now states it as clauses a third party can follow: the `IPiPluginView`
  slots one at a time (with the mistakes each invites), the lifecycle contract
  (no native resource before `pi_attach`, detach is synchronous and repeatable,
  object vs. resource), the thread model you must declare (all-host-thread vs.
  private-thread-and-marshal - including why the Qt kit abandoned the second one
  after it deadlocked), the shutdown-before-`FreeLibrary` contract, the
  `PI_<KIT>_TRACE=1` trace convention, the STATIC-vs-SHARED decision rule, how the
  conformance harness admits a kit to the ecosystem list, and a pre-delivery
  checklist.
  It is not just prose: **`examples/minimal_kit_win32/`** is a complete adapter kit
  written from that spec - ~250 lines of plain C with no toolkit at all (Win32 GDI
  child window, paint callback, per-owner shutdown, optional trace) plus a plugin
  that uses it. It passes the official conformance harness
  (`run_selftest.ps1 -Plugin pi_example_plugin_win32.dll -Cycles 3` -> PASS), so
  "a kit written from the spec works with an official host" is demonstrated rather
  than asserted. `scripts/verify.ps1` now feeds the example plugins to the harness
  as well, which is what keeps that claim honest.
- **`examples/` - the tutorial as runnable projects (ECO-03).** Each example is a
  self-contained directory with its own `CMakeLists.txt` and a README whose three
  steps actually work: `minimal_host` (a window, one container, one plugin - it
  drives a view OR a service), `service_plugin` (headless `IPiService`),
  `minimal_plugin_imgui` (one draw callback), `minimal_plugin_qt` (one widget
  factory) and `specialized_app` (an app-defined protocol on channel A, its own
  host service on channel B, and the capability gate rejecting a plugin that does
  not implement the protocol - before instantiation). They depend on public API
  only and reference nothing under `tests/`, so a single directory can be copied
  out as a starting point; the two without a GUI toolkit are also `ctest` cases
  (`example_minimal_host_service`, `example_specialized_app`). Switch:
  `PI_BUILD_EXAMPLES` (default ON, mirrored in `conanfile.py`).
- **`pi_descriptor_init(desc)` - zero a descriptor before filling it in.** See the
  fix below for why this exists.

### Fixed

- **Two test plugins violated the frozen out-parameter rule.** Both returned
  `PI_E_NOINTERFACE` from `pi_create_instance` without clearing `*out`, which the
  conventions (interfaces.md 2.4, decided in the interface freeze review) and every
  host rely on. The new unknown-GUID unit case caught both on its first run - a
  caller that passed a dirty `out` would have read its own stale value back.

- **A descriptor with automatic or dynamic storage could crash the HOST.** The
  descriptor gained appended optional fields in 0.3 (`properties` /
  `property_count`), which a plugin that fills its descriptor field by field - the
  pattern the tutorial taught - can forget to initialize. Nothing complains at
  compile time: the fields hold whatever the memory had (0xCDCDCDCD in a Debug
  build), the host sees a non-zero `property_count`, walks `properties` and faults
  inside ITSELF. That is exactly what happened while writing
  `examples/minimal_plugin_imgui`: `pi_test_host_imgui!LoadPlugin+0x35f` executing
  `cmp qword ptr [rax+8],0` with `rax = 0xCDCDCDCDCDCDCDCD`, captured with cdb.
  `pi_descriptor_init()` zeroes every field (documented as "call this first" in the
  header, interfaces.md 1.4 and the tutorial's factory snippet), the example uses
  it, and the conformance harness now passes on both example plugins. Static or
  global descriptors are zeroed by the language already.
- **`PI_PLUGIN_ENTRY_DECL` produced a C++-mangled entry point.** The macro is what
  a plugin author is told to use for `pi_plugin_entry`, but it lacked `extern "C"`,
  so in a C++ plugin the exported symbol was
  `?pi_plugin_entry@@YAHPEAPEAUIPiPluginFactory@@@Z` and the host - which looks the
  name up verbatim - answered "does not export pi_plugin_entry". The two C++
  example plugins hit this immediately; the in-tree C++ test plugins had each
  written `extern "C"` by hand, which is why nobody had noticed. The macro now
  expands to C linkage in C++ (and to nothing in C, where `extern "C"` is
  illegal), so plugin authors no longer have to know.
- **The imgui adapter kit used ONE process-wide window class name.** A fixed class
  name is a process-wide resource and Windows does not drop a class when the
  module that registered it is unloaded, so a second imgui plugin module - or the
  same plugin reloaded at a different address - registered the same name, got
  `RegisterClassExW == FALSE` (which was ignored), and then created its window
  with the PREVIOUS module's `WndProc`. It works until a message dispatches into
  code whose module state is gone. Caught by feeding the example plugins to the
  conformance harness alongside the official ones: the fault was
  `pi_example_plugin_imgui!ImGui_ImplWin32_GetDpiScaleForMonitor+0x42`, reached
  from `USER32!UserCallWinProcCheckWow` - a window procedure dispatch. The class
  is now unique per view and unregistered when that view's window is destroyed
  (the Win32 example kit does the same per module, with a re-register fallback for
  a reload at the same base). Five plugins, one process: PASS.
- **The imgui adapter kit left the plugin's ImGui context current after
  `pi_attach()`.** `pi_imgui_view_create()`'s attach path creates the plugin's
  context and switches to it (deliberately - the init callback runs there) but
  never switched back, so a host that itself uses Dear ImGui had every later
  `ImGui::NewFrame()`/backend call run against the plugin's context and device.
  The path now saves and restores the host's context. (Found while chasing the
  crash above; it was not that crash's cause, but it is wrong for imgui hosts.)

### Changed

- **Qt is no longer hard-coded (ECO-05).** See the 0.4.0 entry for the switch
  description - it landed after that section was written, so it is recorded here:
  the discovery order is `PI_QT_PREFIX` -> `Qt5_DIR`/`CMAKE_PREFIX_PATH` (including
  the environment, and PATH on Windows), the maintainer's usual location is only a
  hint when nothing else is given, and a missing Qt disables the four Qt targets
  with an actionable message instead of breaking the configure.

Nothing yet. Add entries here as work lands; they move under the next version
when it is cut.

## [0.4.0] - 2026-09-23

### Changed

- **Source-level rename: the `PIPLUGIN_*` macros are now `PI_PLUGIN_*`.** The
  framework's public macros all carry the same `PI_` prefix as the rest of the
  library family (`PI_PLUGIN_ENTRY_DECL`, `PI_PLUGIN_EXPORT`, `PI_PLUGIN_API_VERSION`,
  `PI_PLUGIN_BUILDING`, `PI_PLUGIN_QT_BUILDING`), so there is one prefix rule to
  remember instead of two. Binaries are unaffected (a macro name is not part of the
  ABI), but plugin sources must be updated; the build switches keep their
  deliberate `PI_BUILD_*` group prefix.

- **`PiPluginDescriptor` gained free-form metadata (APP-04) - a binary layout
  change.** Capabilities answer "what can this plugin do in the framework's
  vocabulary"; descriptive facts (which UI toolkit, supported file formats, a
  homepage) had nowhere to go and were being squeezed into GUIDs. The struct now
  ends with `properties` / `property_count` (`PiPluginProperty { key, value }`,
  UTF-8, `pi.` prefix reserved for the framework) and
  `pi_descriptor_find_property(desc, key)` reads it. `PI_PLUGIN_API_VERSION` goes
  0.2 -> 0.3 because of the layout change (the unit suite's version tripwire
  fails on that bump by design - it is the reminder to update this file and
  interfaces.md 1.5). One subtlety worth knowing: the version gate REJECTS a
  plugin newer than the host but ACCEPTS an older one (same major), and an older
  module's descriptor is shorter - so `pi_descriptor_find_property()` decides the
  layout from the plugin's own `api_version` (`minor < 3` means "no properties")
  instead of reading past the end of the object.
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

- **Events: a structured, two-way channel (APP-06).** `pi_host_post_message()`
  carries three integers and nothing to route on, and the host -> plugin direction
  only existed as the plugin's next `pi_on_idle()` poll - a plugin without a view
  had no callback at all. Two optional interfaces fix that, without touching a
  single published vtbl:
  - `IPiEventSink` (plugin side): the host queries it after instantiation and
    pushes ADDRESSED events into it with `pi_host_session_deliver_event()`;
  - `IPiHostEvents` (host side): plugins query it on the host object and then
    publish / subscribe by topic (`subscribe(topic, owner, cb, user, &handle)`,
    `unsubscribe`, `drop_owner`).
  An event is `{ type, topic, payload, payload_count, origin }`: `type` is the
  framework's small vocabulary (`PI_EVENT_NOTIFY` / `PI_EVENT_REQUEST` plus an app
  range), `topic` is a UTF-8 literal name (`pi.` reserved for the framework, and
  the framework defines no wildcard syntax), and the payload reuses APP-04's
  key/value strings - so an event is already serialisable for a future
  out-of-process host (FUT-05). Contract highlights: `publish` may be called from
  any thread and the host marshals everything - `pi_event_deliver`, subscription
  callbacks, subscribe/unsubscribe/drop_owner - to its own main thread, so a
  plugin sink needs no locking; delivery is BEST EFFORT (a host may merge or drop,
  and the drops are countable in the router's stats); and a subscription is owned
  by its `owner` token (the plugin instance), so a plugin that forgets to
  unsubscribe still cannot leave a dangling callback behind.
  The **host kit** does the bookkeeping the unload sequence makes easy to get
  wrong: it queries each slot's sink at instantiation and, during teardown, drops
  the owner's subscriptions BEFORE releasing the sink and unloading the module. A
  ready-made router (`PiEventRouter`, static library `piplugin_events`, opt-in - a
  host may implement the interface itself) supplies the subscription table, a
  bounded queue, `pump()`, drop counters, and `pi_event_router_extra_qi()`, which
  plugs straight into APP-01's `pi_host_services_create_ex()` hook.
  Acceptance: `tests/test_host_events` (`ctest` case `events_two_way_loop`, 36
  assertions, exit-code verdict) runs the whole loop - plugin publishes, host
  subscribes and pushes into the sink by address, the plugin answers from inside
  its sink, a broadcast reaches the plugin's own subscription - and pins the
  no-reentrancy pump rule, strict unsubscribe semantics, the owner drop on unload,
  and the degradation path (a plugin with no sink loads fine, delivery returns
  `PI_E_NOINTERFACE`). `PI_PLUGIN_API_VERSION` 0.3 -> 0.4 (new interfaces; the unit
  tripwire fails on that by design). Design and the D1-D9 decisions:
  `docs/design/events.md`.
- **The conanfile's switch tree is complete again.** `PI_BUILD_HOST_KIT_EVENTS`,
  `PI_BUILD_UNIT_CPP_TESTS`, `PI_BUILD_TEST_HOST_MULTI`,
  `PI_BUILD_TEST_HOST_EVENTS`, `PI_BUILD_TEST_PLUGIN_SERVICE` and
  `PI_BUILD_TEST_PLUGIN_EVENTS` were added by earlier commits without being
  mirrored into `conanfile.py`, which claims the two trees are one-to-one. The
  file also gained the matching adapter/host-kit dependency-table entries and the
  `piplugin_events` package component.
- **Descriptor properties, exercised end to end (APP-04).** All three test
  plugins declare properties (`com.example.kind`, the toolkit, a variant tag) and
  the headless test host lists them and looks one up by key; new `ctest` cases
  (`descriptor_properties_{imgui,qt,service}_plugin`) require the looked-up value
  to appear in the host's output, so "a plugin declares a property and the host
  reads it" is asserted rather than assumed. The GUI hosts show the properties in
  their panels and write them to their logs. `tests/unit` covers the reader
  itself: hit, miss, case sensitivity, whole-key comparison, empty values, UTF-8,
  NULL arguments, empty table, a NULL key inside the table, duplicate keys
  (first wins), and the pre-0.3 layout case.
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
