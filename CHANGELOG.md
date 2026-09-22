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

Nothing yet. Add entries here as work lands; they move under the next version
when it is cut.

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
