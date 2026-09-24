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

Why there is no 0.3.0 entry: it is deliberate, not an oversight. The descriptor
layout change (APP-04, `properties` / `property_count`) pushed the API version
0.2 -> 0.3, and events (APP-06, `IPiEventSink` / `IPiHostEvents`) pushed it
0.3 -> 0.4 the next day; no release was cut in between, so both minor bumps
shipped together in 0.4.0. "0.3" therefore exists only as a value a plugin may
declare in `api_version` (the `PI_PLUGIN_API_VERSION` line in
`include/piplugin/pi_plugin_types.h` carries `0, 4`): a host accepts it under
the same-major rule in [docs/design/interfaces.md](docs/design/interfaces.md) 1.5,
and `pi_descriptor_find_property()` already treats a plugin `minor < 3` as
"the pre-properties descriptor layout", so nothing else needed a 0.3.0 of its
own.

## [0.5.0]

### Changed

- **This library's own names now carry its own prefix, and the vocabulary shared
  across the family moved out to the base layer `pibase`.** Two changes that
  belong in one entry, because the first is what made the second visible.

  *Names.* Every identifier this library defines is now `pi_plugin_*` (functions)
  / `PiPlugin*` (types) / `PI_PLUGIN_*` (macros). The bare `pi_` / `Pi` / `PI_`
  prefix is reserved for vocabulary the whole family shares, so a reader can tell
  from the identifier alone whether a name is family-level or this library's. In
  practice everything a host or plugin author writes is affected:
  `pi_module_load` -> `pi_plugin_module_load`, `PiServiceOption` ->
  `PiPluginServiceOption`, `PI_CAP_OPTIONAL` -> `PI_PLUGIN_CAP_OPTIONAL`,
  `pi_event_deliver` -> `pi_plugin_event_deliver`. `PI_EXPORT` becomes
  `PI_PLUGIN_API`, and `PI_PLUGIN_EXPORT` (which exports a *plugin's* entry point)
  becomes `PI_PLUGIN_ENTRY_EXPORT` so it stops reading like its new neighbour.
  Not renamed, because they were already right: `pi_plugin_entry`,
  `PI_PLUGIN_API_VERSION`, `PiPluginDescriptor`, `IPiPluginFactory`, and the
  `pi_plugin_*.h` file names.

  *Layer.* The family-level vocabulary - the result-code list, `PiGuid`,
  `PiNativeWindow`, `IPiUnknown` with its reference counting and `PI_IID_UNKNOWN`,
  and the ABI/platform plumbing (`PI_CALL`, `PI_EXPORT`, `PI_IMPORT`,
  `PI_LOCAL`, `PI_PLATFORM_*`) - now comes from the header-only `pibase` package
  as `<pibase/pi_base.h>`. `pi_plugin_unknown.h` is gone: its entire contents
  were family vocabulary. Six symbols therefore leave this library's export
  surface - `pi_guid_equal`, `pi_refcounted_init`,
  `pi_refcounted_init_with_destroy`, `pi_refcounted_add_ref`,
  `pi_refcounted_release` and the `PI_IID_UNKNOWN` data symbol - because they
  are `static inline` or header constants now. Consumers need
  `find_package(pibase)` (or the Conan package) reachable; the exported target
  already carries that dependency.

  The result-code partition that previously existed only in prose is now
  recorded in the header with the codes: app and plugin error codes take values
  <= -100, and an interface that needs an "accepted, result later" success state
  must introduce a **positive** result code, because `PI_SUCCEEDED` /
  `PI_FAILED` decide by sign and a negative "pending" would make `PI_FAILED`
  answer its own question wrongly.

  *Rebuild everything together.* The entry point's name is unchanged
  (`pi_plugin_entry` is still `pi_plugin_entry`), but the names of the exported
  symbols a plugin imports changed, so a module built against 0.4 will not load
  into a 0.5 host. That is the ordinary pre-1.0 rule above: hosts and plugins
  move as a set across an `x` release. Nothing about the vtable layouts, the
  descriptor layout, or the descriptor's `api_version` semantics changed.

## [Unreleased]

### Added

- **Linux is compiled by CI now, and getting there found four real defects (W-10).**
  A `linux` job builds the core and both host kits with **gcc *and* clang** and runs the
  C++ layer, which reports `checks=52 failures=0` under each. It does not run
  `conan install`, and that is not an omission: with Qt and imgui off, nothing left
  needs a third-party package. Four things stood between the repository and that green
  run. `dlsym`'s object-pointer-to-function-pointer cast is an *error* under the flags
  this project already sets for GNU compilers (`--pedantic-errors`), so the entry point
  is fetched through a `memcpy` now; `strdup` and `syscall` are hidden behind glibc's
  feature macros once `-std=c11` defines `__STRICT_ANSI__`, so `pi_plugin_host.c`
  defines `_DEFAULT_SOURCE` before its first include; and `$<TARGET_PDB_FILE>` - an
  MSVC-only generator expression that no `if(WIN32)` guards - failed at *generate*
  time on Linux, so the deploy step is now a Windows and a non-Windows command in
  `src/piplugin/CMakeLists.txt` and `adapters/qt/CMakeLists.txt`. Separately,
  `PiNativeWindow`'s Linux `unsigned long` was verified against real Xlib headers
  (`sizeof(Window) == sizeof(XID) == sizeof(PiNativeWindow)`, round trip through both
  types, compiled with the project's own flags), which settles the suggestion to move
  it to `uintptr_t`: unnecessary. What the job deliberately does **not** claim, recorded
  in `docs/todo/platform.md` #3 together with the fix: `unit`, `unit_threads` and the
  headless host call `nanosleep()` without those feature macros and do not compile on
  Linux under gcc >= 14, every plugin target is still behind `if(NOT WIN32)` so there
  is not a single loadable plugin, and `add_test` hardcodes a `.exe` suffix that ctest
  cannot launch off Windows. The card asked for a headless smoke on Linux; rather than
  paper over those three with an extra `-D`, the job says what it covers and the README
  says the same thing.

- **An AddressSanitizer track, and the three things measuring it on Windows taught us
  (W-03).** `scripts/verify_asan.ps1` reconfigures an existing build tree with
  `/fsanitize=address` *in place*, builds, runs the non-GUI part of the suite under it,
  and then puts the cache values back and rebuilds, so `bin/<Config>` never disagrees
  with the tree that owns it. In place, rather than in a second build tree, because
  `pi_project.cmake` deploys every test binary into `<repo>/bin/<Config>`: a sibling tree
  would overwrite those files with instrumented copies while the first tree still
  believed they were its own, and the next `verify.ps1` would fail to even start them.
  No `CMakeLists.txt` change was needed - the flags ride in as cache variables on the
  configure command line - so the work stays inside the CI/script files. CI gained an
  `asan` job that runs it with `-SkipRestore`. Measured rather than assumed: **MSVC's
  AddressSanitizer implements no leak detection on Windows**, and asking for
  `detect_leaks=1` kills the runtime before `main()` with "detect_leaks is not supported
  on this platform", so the leak half of the check stays with the `_CrtDumpMemoryLeaks()`
  assertion in `unit_cpp` and the script says so instead of implying otherwise; the GUI
  cases are excluded for a reason that turned out to be concrete - a full `ctest` under
  ASan reported a **heap-use-after-free inside a third-party input method**
  (`SogouPY.ime`) reached through `USER32`/`MSCTF` from an example host creating its
  window, which is not this project's bug and not something a sanitizer job should be
  red about; and the track has teeth, which was checked by building a deliberately
  out-of-bounds probe with the same flags - ASan reports `heap-buffer-overflow` with the
  source line and exits 1. The nine non-GUI cases are green under it.

- **The repository can no longer contradict itself in its own documentation (W-13).**
  `scripts/verify.ps1` has a fifth check driven by `scripts/doc_drift_rules.json`: a
  feature that exists makes a set of statements illegal (`tests/unit/pi_unit_tests.c`
  exists, so nothing may still say there are no unit tests), and because a rule only
  fires while its feature really is in the tree, the table cannot rot into assertions
  about a repository that moved on. The documents come from `git ls-files '*.md'` - so
  the temporary dispatch board, which is never committed, is naturally not part of the
  check - and are read as UTF-8 explicitly. A section whose heading carries a done
  marker is excused, because the todo files deliberately keep the original wording of a
  finished item underneath its status note; a platform-specific claim can additionally
  be required to sit on a line naming that platform, so the macOS row one line below a
  Linux one is not dragged in. Verified by reintroducing a drift on purpose: dropping a
  finished item's done marker makes the check report
  `docs/todo/tests.md:13: says '没有单元测试', but tests/unit/pi_unit_tests.c exists`
  and fail the run, and restoring it takes 37 documents back to green. The clang-format
  check still reports instead of enforcing, but it now prints the drift list itself,
  which is the material for that decision. clang-tidy and cppcheck were evaluated, not
  adopted: cppcheck is installed nowhere here, and clang-tidy - which is - enables **no
  checks at all by default** since LLVM 17 (a bare run exits with
  `Error: no checks enabled.`), so adopting it means choosing a check set first; with a
  conservative one (`clang-diagnostic-*`, `bugprone-*`, `performance-*`) the core
  reports 4 warnings, one of them glibc's own `_DEFAULT_SOURCE` feature macro, which
  says an allow-list is needed before any of this can be enforced.

- **Windows binaries identify themselves now (W-07).** Every product DLL's property
  page showed empty version information: the `.rc` template had been "about to be
  provided" since the beginning (the `version_dll.rc.in 暂未提供` comments in the
  target CMakeLists), so a user reporting a problem could not tell which build they
  had. `cmake/version_dll.rc.in` plus `cmake/version_resource.cmake`
  (`piplugin_add_version_resource(<target> "<description>")`) now inject the project
  facts, generate the resource per configuration and attach it to the core library,
  both adapter kits and all four host kits - `tests/` and `examples/` stay out by
  design. `piplugind.dll` and `piplugin_qtd.dll` report `FileVersion`/`ProductVersion`
  `0.4.0`, `FileDescription` "piplugin framework core library" / "piplugin Qt adapter
  kit", `CompanyName` and an `OriginalFilename` carrying the Debug `d` suffix, all
  derived from `project(VERSION)`, so the numbers cannot drift from the release. Two
  things are worth knowing before trusting "the kits have versions too": a resource
  inside a STATIC library never reaches the consuming binary (measured - MSVC's
  linker pulls library members by symbol need, and a resource-only member resolves
  nothing, so an exe linked against a library carrying a `9.9.9.9` resource ends up
  with empty version info), which is why only the two SHARED product libraries show
  anything today; and a generator-expression-looking literal in the template's own
  comment is evaluated by `file(GENERATE)` like any other content (`Expression did
  not evaluate to a known generator expression`), so the template keeps such literals
  out of its prose.

- **`cpack` produces the archive somebody actually downloads, and it is verified
  from the outside (W-08).** The distribution had two shapes - a Conan package and a
  bare `cmake --install` tree - and no artifact. `CPack` adds the third: a ZIP whose
  root is `bin/`, `lib/`, `include/` and the three root documents (`LICENSE`,
  `README.md`, `CHANGELOG.md`, which no install rule had ever carried). Clearing
  `CPACK_PACKAGING_INSTALL_PREFIX` is deliberate: on Windows CPack inherits
  `CMAKE_INSTALL_PREFIX`, which would bury the whole tree under
  `/Program Files/piplugin/`. The archive name carries the build configuration, but
  not from a generator expression - `CPACK_PACKAGE_FILE_NAME` ignores those, and the
  literal text it leaves behind contains characters Windows rejects in a path, so
  packaging died with `Problem creating temporary directory`; the configuration is
  appended in `cmake/CPackProjectConfig.cmake` instead, which CPack includes at
  package time, when `CPACK_BUILD_CONFIG` is set. `PI_CPACK_NSIS=ON` adds an NSIS
  installer for whoever has NSIS. `scripts/verify_package.ps1` grew a phase C that
  unpacks the ZIP into a clean directory, asserts its layout, and then asserts the
  only thing that matters: a host program configures and builds against the unpacked
  archive with NO Conan toolchain and NO repository path on `PATH`, and runs. That
  phase earned its keep on its first real run by catching a regression the new docs
  install rules introduced: `conanfile.py` did not export `LICENSE` / `README.md` /
  `CHANGELOG.md`, so `conan create` failed in `package()` with `file INSTALL cannot
  find .../LICENSE: File exists.` The export list carries them now, which is also what
  finally makes the Conan package ship its license file.

- **A conan-free way in: `CMakePresets.json` lives in the repository now (W-09).**
  Configuring required Conan for a mundane reason: `CMakeUserPresets.json` only
  appears after `conan install`, yet it was committed, and it `include`s
  `build/generators/CMakePresets.json`, which is generated. CMake does not tolerate a
  missing include - on a fresh clone every `cmake --preset ...` invocation died with
  `Could not read presets ... File not found` before it could even look at the preset
  that was asked for. The repository now carries `CMakePresets.json` (`default`:
  VS 2022 / x64 into `build/generic`; `default-unix`: Ninja; matching build and test
  presets; no Conan-generated content) and `CMakeUserPresets.json` is no longer
  tracked, which is what CMake's own documentation asks for a per-machine file that
  Conan rewrites. Missing Qt and imgui keep disabling their targets with the existing
  actionable message instead of breaking the configure, so `cmake --preset default`
  followed by `cmake --build --preset default` builds everything that needs no
  third-party package - verified from a simulated clean checkout (both preset files
  hidden) with Qt present and imgui absent. The one thing that lived only in the
  deleted local preset was the pinned install prefix, so see the entry under
  *Changed*.

- **Two imgui plugin modules in one process (W-05).** The matrix had "several Qt
  plugins in one process" and "imgui plugin in a non-imgui host", but nothing for
  the combination that shares process-level resources between two imgui plugins:
  each view registers its own window class, builds its OWN ImGui context and its
  OWN D3D11 device, and the kit swaps the current context in and out around every
  callback. The kit is a STATIC library, so nothing is shared between modules -
  which is exactly why two DIFFERENT module files (not one file loaded twice) are
  the interesting case, and why this is the path where the old single
  process-wide window-class name crashed. `tests/test_plugin_imgui` now builds
  two variants (`pi_test_plugin_imgui.dll` / `pi_test_plugin_imgui2.dll`,
  differing only in class GUID, display name and heartbeat code), the imgui test
  plugin reports its frame counter through `pi_host_post_message` from inside its
  draw callback, and `tests/test_host_multi --imgui-pair` (ctest
  `multi_plugin_imgui_in_one_process`) loads both, gives each its own container,
  drives frames, and asserts that both windows are visible inside their OWN
  container, that both frame counters ADVANCE (a widget drawn once and then
  frozen is exactly what a "window exists" check would miss), that neither window
  wandered into the other's container, and that both are gone after a clean
  unload.

- **Runtime container switching is automated - the last empty cell of the
  scenario matrix (W-02).** `pi_host_default_set_ui_window()` has always let a
  host point its services at another container at runtime, and `pi_attach()` has
  always accepted a new parent, but nothing exercised the switch: load -> attach
  into container A -> switch to B -> switch back to A -> resize round trip ->
  unload, with the plugin window's parent, visibility and geometry asserted at
  every step. Two ctest cases in `tests/test_host_multi` (`container_switch_runtime`
  with the imgui plugin, so it also runs where Qt is off, and
  `container_switch_runtime_qt` for the Qt kit's widget teardown/rebuild path).
  The case also pins the part that makes switching safe: `pi_view_detach()` is
  synchronous, so the old native window must be gone before the new one is
  created - asserted directly, along with the old window's disappearance after
  unload. The three-step recipe (detach -> `pi_host_default_set_ui_window()` ->
  attach) is now written down in `docs/design/interfaces.md` next to the call,
  since the call alone does not move anyone's window.

- **Thread-safety coverage, and the `pi_qt_view_post()` bug it uncovered (W-04).**
  The three cross-thread paths that had never been automated now have ctest cases:

  1. **A plugin subthread posting to the host** (`ctest unit_threads`, plus the
     plugin-level half of `qt_view_post_from_worker_thread`): threads call
     `pi_host_post_message()` while the host does what the contract says it must
     - receives on whatever thread the call arrives on, queues it, and delivers
     it on its own loop. The case asserts the callback really is entered on a
     non-main thread (otherwise the scenario is not being exercised at all), that
     nothing is lost or duplicated, and that every message is delivered exactly
     once on the main thread.
  2. **Concurrent AddRef/Release** (`ctest unit_threads`): paired add/release
     from four threads must return the count to its base without destroying;
     N threads releasing concurrently must hit zero with `destroy` called exactly
     once; and a tight add-only loop from four threads must produce the exact
     count (a lost update accumulates, where paired operations would cancel).
     Both the exported helpers and the vtbl slots are driven.
  3. **`pi_qt_view_post()` from a non-UI thread** (new `pi_qt_view_post` +
     `ctest qt_view_post_from_worker_thread`): the header said "run fn on the
     host GUI thread" and the docs said "callable from any thread, marshalled to
     the Qt thread", but the implementation simply ran the callback inline - so a
     plugin author following the documented contract would touch QWidget from a
     background thread. It now keeps that promise the cheap way: called on the
     host GUI thread it still runs inline; called from anywhere else the call is
     queued and run by the host's next `pi_on_idle()` slice, without blocking and
     **without reintroducing the private Qt thread** the kit deliberately
     abandoned (the README's "do not go back" note is unchanged). Queued calls
     are dropped when the view is detached or destroyed, and before the first
     attach - the kit logs each drop under `PI_QT_VIEW_TRACE=1`. The kit's header,
     `adapters/qt/README.md`, `docs/design/interfaces.md` (its thread-model table
     also still described a private Qt thread that no longer exists) and
     `docs/tutorial/adapters.md` now say the same thing.
     Verified by reverting the implementation to the inline call: the new case
     fails on both of its assertions, every run.
     The new cases are registered by `tests/test_host_multi` (which grew modes for
     `--post-thread` and `--container-switch`) and `tests/unit`
     (`pi_unit_threads`), so `unit_threads` also runs where Qt is not available.

- **The load-error string is per-thread now, with a copy-out variant (W-01).**
  `pi_module_get_load_error()` read one process-wide `static char
  g_load_error[256]`, so a host that loads plugins from several threads could
  only ever see whichever thread wrote last - a thread frequently read back
  somebody else's failure reason. The string is thread-local now
  (`__declspec(thread)` / `_Thread_local`), which keeps the old signature, the
  old "valid until the next load" lifetime and the old single-threaded behaviour
  unchanged, and `tests/unit` races four threads (32 rounds each, a distinct
  bogus path per thread and round, with a widened write-to-read window)
  asserting every thread reads back its own reason; run against the old
  process-global buffer the case fails in ~75% of rounds. New alongside it:
  `pi_module_get_load_error_r(buf, size)` copies this thread's message into a
  caller-owned buffer, so the reason survives later loads - what a host wants
  when it reports or stores a failure (`PI_E_INVALIDARG` for a NULL buffer or
  size 0; truncation is still NUL-terminated). Export surface 27 -> 28; no
  existing symbol changed shape. Closes freeze-review finding F6
  (`docs/design/interface-freeze-review.md`).

- **A packaged-consumer test, and the packaging bugs it found (ECO-04).**
  `examples/conan_consumer/` is a consumer in the literal sense - it does not
  `add_subdirectory` anything, it just does `find_package(piplugin)` and links the
  official targets - and `scripts/verify_package.ps1` builds and RUNS it against
  both shapes of the distribution: a `cmake --install` tree and a `conan create`
  package. Unit tests and the conformance harness cannot answer "does somebody
  else's project actually work against what we ship"; this can, and on its first
  runs it failed three times:

  1. **`find_package(piplugin)` could not find the install tree at all.** The CMake
     configs were installed to `lib/cmake/pi/` - named after the namespace - but
     `find_package(<name>)` only searches `<prefix>/lib/cmake/<name>*/`, so they
     were invisible to `find_package(piplugin)`. They now live in
     `lib/cmake/piplugin/` (matching the package name, and matching what CMakeDeps
     generates), the core's component config is `pipluginCoreConfig.cmake` so it
     does not collide with the umbrella's name, and `src/cmake/piForwardConfig.cmake.in`
     keeps the old `find_package(pi)` entry point working. The umbrella is also
     component-aware now: a component that is explicitly requested gets a hard
     `find_dependency`, one that merely happens to be installed is pulled in only
     if its dependency is already findable, and otherwise it is skipped with a
     STATUS line - so a consumer that only wants the core no longer fails to
     configure on a machine without Qt5.
  2. **`package_info()` declared components that were not in the package.** CMake
     can silently drop targets (a missing Qt5 disables all four Qt targets), but
     the recipe declared the Qt host kit anyway, so every CMakeDeps consumer died
     with "Library 'piplugin_host_qtd' not found in package". `_packaged()` now
     checks the package folder for each library before describing it.
  3. **Components do not inherit the package-level directories.** `libdirs`,
     `bindirs` and `includedirs` set on `cpp_info` are not used for components:
     CMakeDeps generated `<pkg>/lib` while the libraries are in `<pkg>/lib/Debug`,
     and the kits' header directories were missing entirely. Both are now set per
     component (the KIT headers are flattened into
     `include/piplugin/<host_kits|adapters>/<kit>/`, so `#include "pi_host_session.h"`
     keeps working for consumers exactly as it does in the build tree).

  Result: install-tree consumer links core + host kit L0 + event router + imgui
  adapter and runs; Conan consumer links core + host kits + event router + imgui
  adapter and runs. Two things the imgui kit needs under Conan are now recorded
  rather than hidden:

  - **The consumer must require the external package too.** Under Conan 2.10.1 +
    CMakeDeps a component-level *external* require is only propagated when the
    consumer itself also requires that package; otherwise it is dropped SILENTLY -
    no imgui-config.cmake is generated, `piplugin_FIND_DEPENDENCY_NAMES` stays
    empty, the component's DEPENDENCIES list keeps only `pi::piplugin`, and linking
    `pi::piplugin_imgui` fails with 29 unresolved imgui symbols. The Conan code
    responsible is `get_deps_targets_names()` in
    `conan/tools/cmake/cmakedeps/templates/target_configuration.py`, which resolves
    the declared component requires against the consumer's requirements and does
    `except KeyError: pass`. `scripts/verify_package.ps1` therefore declares
    `imgui/<version>` (parsed from this recipe, so they cannot drift) in the
    consumer's conanfile.txt, and `examples/conan_consumer/README.md` has the
    before/after table. The CMake install tree has no such limitation.
  - **Static kits must declare their platform libraries** (our bug, fixed):
    `piplugin_imgui` links `user32 d3d11 dxgi d3dcompiler` and `piplugin_host_dx11`
    links `d3d11 dxgi`. CMake's exported targets carry those automatically, but
    CMakeDeps only knows `cpp_info.system_libs`, so a Conan consumer hit
    `unresolved external symbol D3D11CreateDeviceAndSwapChain`. Both components now
    declare them on Windows.
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
- **The Qt 5.15 end-of-life question got an answer instead of a shrug (W-12).**
  `docs/design/qt6-assessment.md` is the FUT-09 evaluation, and it separates what
  "EOL" actually means for 5.15 from what it sounds like: regular commercial LTS
  support ended 2025-05-26 and the online installer's last public Qt 5 binary is
  5.15.2, but the public archive still publishes OPEN SOURCE 5.15 source packages
  and CVE diffs (`qt-everywhere-opensource-src-5.15.19.tar.xz`, 2026-05), so the
  situation is "self-maintained", not "abandoned". Its conclusion is a decision,
  not a survey: stay on a single Qt 5.15.2 through the 1.0 freeze, refuse the
  long-lived dual-version kit, and migrate to Qt6 in one step - gated on a Qt6
  build lane existing first. Why dual-version loses even though APP-08 made the
  kit SHARED: one process cannot host two Qt majors regardless (two event
  dispatchers, one Win32 message queue), so "two kits" only ever means "two
  builds, two deployment matrices, no mixed plugins" - a maintenance tax for a
  scenario nobody here has. The doc also records that the migration surface is
  thin (a scan for Qt5-only API in the kit, host kit, Qt example and Qt test host
  comes back empty, and the private `_q_embedded_native_parent_handle` mechanism
  the embedding depends on still exists in Qt 6.10's `qwidget.cpp` and
  `qwindowswindow.cpp`) and the three triggers that would reopen the decision.
- **A Qt host can run Qt plugins - the missing first-class path (W-06).** The Qt
  adapter kit is for hosts that do NOT run Qt (it brings the process's
  `QApplication` and pumps it from the host's loop), so a Qt host loading a
  kit-based plugin got no UI: `PiQtView::attach()` bails at `piqt_app_create()`,
  which constructs a second `QApplication` on top of the host's own (Qt allows
  exactly one). Re-checked while writing this: with `PI_QT_VIEW_TRACE=1` the trace
  stops after `attach: enter` and the process had to be killed 5s later - whether
  it fails quietly or stalls depends on the build, and either way it is unusable.
  `docs/tutorial/qt-host-direct.md` is the missing page: a host x plugin toolkit
  decision table, the direct integration (the plugin hands over a `QWidget*`, the
  host adopts it in its own `QLayout`, the host's own event loop drives it), the
  four rules that make it safe (GUI thread only; destroy the widget BEFORE
  unloading the module, because its signal/slot bodies live there; require the app
  protocol so a mis-targeted plugin fails the gate instead of loading and showing
  nothing; never link the kit into a direct plugin), and an error-to-symptom table.
  **`examples/qt_host_direct/`** runs it end to end: an app-defined protocol
  carrying `QWidget*` (channel A - the plugin declares `PI_CAP_PROVIDES`, the host
  `pi_host_session_require()`s it), a host with its own `QApplication` and layout,
  and a `--self-test` mode that clicks the plugin's button programmatically,
  asserts the message reached the host, unloads in order and exits on the verdict
  (`RESULT: PASS`, run). Pointed at the kit-based example plugin it fails by name
  before instantiation:
  `plugin does not provide iid data1=0x9C3E71B5 required by this host`.
  While documenting this, `docs/tutorial/adapters.md` 3.3 turned out to still
  describe the kit's abandoned design (a private `QThread` running
  `QApplication::exec()`, `SetParent`, `wakeUp()`), which `adapters/qt/README.md`
  explicitly forbids; it now describes the shipped thread model.
- **Plugin discovery got its first code fact - a directory scanner (W-11).**
  FUT-07 (discovery/distribution) was a direction with no implementation to argue
  from. `examples/plugin_scan/` is the smallest thing that answers a real design
  question - can a host learn a folder's contents from descriptors alone? - and
  the answer is yes: walk the directory, `pi_host_session_inspect()` each
  candidate (load + version gate + capability gates, no instantiation), read the
  descriptor, then unload immediately. Run over `bin/<CONFIG>` it reports 11
  usable plugins out of 18 DLLs and, just as usefully, the 7 that are not
  plugins here - the framework DLLs and Qt runtime ("does not export
  pi_plugin_entry"), the bad-api_version test plugin and the GUI-required one
  (this scanner is a headless host), each with the gate's own reason. Version
  selection is deliberately a PLACEHOLDER (same descriptor name -> highest
  numeric version) and says so in its own output; a real resolver would also
  weigh api_version, capabilities, platform and dependencies. The example also
  documents the lifetime trap every manifest tool hits once: descriptor strings
  belong to the module, so everything kept is deep-copied before the unload.
  No core file and no ABI changed.

### Fixed

- **Neither distribution shape carried what it needs to stand on its own (ECO-04).**
  The Conan package left both of its dependencies behind, and the cpack archive left
  the base layer behind. `scripts/verify_package.ps1` - the script that asks what an
  outside consumer receives - failed on the second of its three phases, and its third
  phase did not exist yet, so both defects were only visible once the phases were made
  to prove it. *Conan.* A downstream requirement this package does not mark as
  transitive is dropped on the way to the consumer: the graph still contains the node,
  but `CMakeDeps` generates no `<pkg>-config.cmake` for it and no target carries its
  include directory. For `pibase` that meant a consumer requiring only `piplugin`
  could not compile a single public header (`#include <pibase/pi_base.h>` -> C1083),
  so the requirement is now declared `transitive_headers=True`; for `imgui` it meant
  the imgui adapter kit's component lost `imgui::imgui` and the consumer had to
  declare imgui itself to link at all, so it is now `transitive_libs=True`. With both
  traits a consumer's `conanfile.txt` needs one line - `piplugin/0.5.0` - which is what
  the verification script's generated consumer now asserts. The component-level form
  also had to be `pibase::pibase`: the base layer declares no components, and
  `pibase::base` fails with "Component not found" while a bare `pibase` is read as an
  internal component of this package. *cpack.* When the base layer is obtained with
  `PI_PLUGIN_PIBASE_PROVIDER=fetch` it was added with `EXCLUDE_FROM_ALL`, which keeps
  a subdirectory's install rules out of the parent's `cmake_install.cmake` as well as
  out of `ALL` - so its headers and package config never reached the prefix and never
  entered the ZIP. That archive configures, builds and runs here (where `pibase`
  happens to be installed) and fails on the machine that downloads it, which is the
  one failure this check exists to catch, so phase C now packages from a tree
  configured with `provider=fetch` and unpacks it into a clean directory with no
  toolchain. `EXCLUDE_FROM_ALL` is gone: the base layer has one `INTERFACE` target and
  no self-test to build, so keeping it out of `ALL` bought nothing.

- **The core did not actually compile off Windows (W-10).** Three defects were only
  visible once a real Linux build ran, and none of them needed a platform branch that
  was missing - they needed the one that was there to be correct. `pi_module_load`
  cast the result of `dlsym` straight to a function pointer, which ISO C forbids and
  `--pedantic-errors` (already enabled for GNU compilers by `cmake/pi/pi_project.cmake`)
  rejects outright, so the entry point now comes back through a `memcpy`; `strdup` and
  `syscall` were implicit declarations because `-std=c11` defines `__STRICT_ANSI__` and
  glibc hides both behind its feature macros, so `pi_plugin_host.c` defines
  `_DEFAULT_SOURCE` before its first include (the Linux thread id in
  `pi_host_services_create_ex` depends on `syscall`, so this was not cosmetic); and the
  `POST_BUILD` deploy step used `$<TARGET_PDB_FILE>`, an MSVC-only generator expression
  that no `if(WIN32)` protects and that fails during *generation* on any other platform
  with `TARGET_PDB_FILE is not supported by the target linker`. Windows behaviour is
  unchanged - the PDB copy is still there, now inside `if(WIN32)` - and the full Windows
  `ctest` suite (21 cases) is green after the change. A fourth Linux-only fix rides
  along in the same shape: `adapters/qt/CMakeLists.txt` had the same PDB deploy step,
  which would have blocked the first Qt-on-Linux configuration.

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

- **The default install prefix is the build system's job now (W-09).** It used to be
  pinned by a hand-written `conan-default-local` preset living in the committed
  `CMakeUserPresets.json`, which meant only somebody who kept that file got the sane
  default (`<build>/install`) instead of `C:/Program Files/piplugin` - where an
  unprivileged `cmake --install` fails outright (ECO-04). That file is no longer
  tracked (see the W-09 entry under *Added*), so the pin moved into the root
  `CMakeLists.txt`: when CMake itself initialized the prefix
  (`CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT`), it becomes `<build>/install`, and
  an explicit prefix from the user, Conan or a toolchain is still respected verbatim.
  All three flows - Conan, plain CMake, cpack - now behave the same way.

- **Qt is no longer hard-coded (ECO-05).** See the 0.4.0 entry for the switch
  description - it landed after that section was written, so it is recorded here:
  the discovery order is `PI_QT_PREFIX` -> `Qt5_DIR`/`CMAKE_PREFIX_PATH` (including
  the environment, and PATH on Windows), the maintainer's usual location is only a
  hint when nothing else is given, and a missing Qt disables the four Qt targets
  with an actionable message instead of breaking the configure.

- **The documents were audited against the repository, and the eleven places where
  they disagreed were fixed.** A full pass compared every statement that can be
  checked against what is actually in the tree. The unit suite is **210** assertions
  now (plus `unit_threads` 70 and `unit_cpp` 52), not the 109 that `README.md`,
  `docs/README.en.md` and `docs/todo/tests.md` still claimed; `ctest` is 21 cases.
  `host_kits/core/pi_host_session.h` still said the `api_version` gate had not landed
  and that `pi_api_version_compatible` did not exist - it is in `pi_host_session_load`
  with a ctest case; `docs/design/architecture.md` still quoted the hard-coded Qt
  path that ECO-05 removed, still said "three hosts and two plugins", and still
  labelled three IIDs "1.1.0 additions" (a version this project never had, and one
  `src/pi_plugin_unknown.c` comment repeated). `docs/todo/README.md`'s summary still
  listed W-07/W-08/W-09 as open work that the topic files had already closed.
  Tracked files - including four comments in `host_kits/` - pointed at the release
  roadmap and the dispatch board, both of which are untracked working documents, so
  a fresh clone got a dead link; those references now point at `host_kits/README.md`
  and the equivalent design notes instead. (Both files stay untracked on purpose.) `docs/tutorial/quickstart.md`'s FAQ numbered its entries 4.1-4.8 under a
  "5. Common problems" heading, and its CI section described one job where there are
  three. The English README had neither the preset-based configure nor the
  `examples/` row. `docs/todo/tests.md` #7 pointed at a `g_frameLatencyWaitable` in
  the test host; it lives in `host_kits/dx11` now. Two gaps in the roadmap's mapping
  table were closed: `build.md` #6 (library naming) had no mapping at all, and
  `build.md` #1's "Conan package ships no LICENSE" leftover was silently resolved by
  W-08. Freeze-review finding F6 is fixed, so it moved from "open before 1.0" to
  "fixed". Nothing in the code changed except two comments.

- **The conventions behind these documents are in the repository now, and two
  build/test traps are written down.** `CONTRIBUTING.md` records what until now
  lived only in a private working note: the `docs/design` / `docs/tutorial` /
  `docs/todo` split and how to tell them apart, the rule that a finished todo item
  hands its general knowledge to a standalone `docs/design/` document and keeps
  only a conclusion plus a link, the language split (`docs/` Chinese,
  `CHANGELOG.md` English, English conventional commit subjects), and the commit
  discipline (explicit pathspecs, never `git add -A`, the one-off planning
  documents stay untracked so tracked documents must not name them, `.workbuddy/`
  stays ignored). `docs/tutorial/quickstart.md` gained 5.9 - `cmake --build` can
  stall silently after the root `CMakeLists.txt` changes, because the build first
  triggers a nested CMake regeneration; configure explicitly once and it proceeds
  - and a note that `verify.ps1`'s documentation-drift step reports a false failure
  outside a git checkout, since it scans `git ls-files '*.md'`.
  `scripts/verify_asan.ps1`'s header now names the two log lines that are expected
  noise (LNK4044 / LNK4300) and how a startup failure presents itself - every case
  failing its regex and `0% tests passed`, which reads like broken assertions
  rather than a runtime that never started. `docs/todo/tests.md` #4 records the
  environmental degradation that makes the imgui cases time out in a long session
  - which is why a change is evidenced by one full `ctest` run taken right after
  it, not by re-running later.
  `scripts/probe_x11_native_window.sh` is the Xlib probe behind
  `docs/todo/platform.md` #5, committed so the next change to `PiNativeWindow`
  can re-run it instead of trusting a number in a document.

- **The documents are a snapshot of the repository again, not an archive.** The five
  topic todo files were 79-91% finished-item text: each entry kept the wording of the
  plan it replaced underneath its status note. That was history living in live
  documents, and it cost real money in the last audit - the preserved text is exactly
  where the stale "109 assertions", "no CI configuration" and "the api_version gate has
  not landed" statements were found. A finished entry now carries only what is true
  today: a `> **结论**` block saying what exists and where it is verified, plus a
  pointer at the design document that owns the mechanism. Everything else moved:
  general knowledge into `docs/design/` (the Xlib probe behind `platform.md` #5 is
  `scripts/probe_x11_native_window.sh` now, and the "why two distinct plugin modules"
  rationale behind the imgui concurrency case reached `docs/design/adapter-spec.md`
  §8), evidence into the entry point that reproduces it (the ASan log-reading notes
  live in `scripts/verify_asan.ps1`'s header), and the rest of the plan into git and
  into this file. Open items that were buried inside finished entries are entries of
  their own now: `build.md` #8 (the three packaging leftovers) and #9 (the
  unscheduled toolchain-file question). `docs/todo/README.md` is the list of open
  items plus the calls only the maintainer can make - the four pending decisions that
  until now existed only in an untracked dispatch board (ABI 2.0 timing, the
  clang-tidy check set, Qt runtime redistribution, the dual-home contingency) are in
  the tracked tree for the first time. `docs/todo/install-design-review-prompt.md` is
  deleted: it was a spent prompt for an external model, and its own header said the
  situation it described no longer held.

- **The documentation-drift check has no hiding place left.** Its done-marker
  exemption existed for one reason - to let the todo files keep that finished-item
  wording - so the exemption, the `done_markers` key and the paragraph in
  `doc_drift_rules.json` that explained it are gone. A finished entry is a statement
  about today and is checked like any other sentence; `CHANGELOG.md` stays excluded
  because it is the one document whose subject is the past. The check was run with
  the exemption disabled before removing it (clean either way) and after removing it:
  clean over 36 scanned documents.

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
