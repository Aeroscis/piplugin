# Credits and attribution

This project is MIT licensed (see [LICENSE](LICENSE)). It vendors a small
amount of third-party code and depends on one third-party library; both are
recorded here. Nothing in this file changes the terms of those licenses.

## Dear ImGui — vendored Win32 / DX11 backends

`adapters/imgui/backends/` holds copies of two official Dear ImGui backends.
They are in-tree rather than taken from Conan because the plugin-side imgui
adapter kit needs exactly these platform backends; the imgui core itself is a
normal Conan dependency (see below). Why they are vendored is also recorded in
`adapters/imgui/CMakeLists.txt`.

- **Repository:** https://github.com/ocornut/imgui
- **Author:** Omar Cornut and Dear ImGui contributors
- **License:** MIT — Copyright (c) 2014-2026 Omar Cornut
- **Files:** `imgui_impl_win32.cpp`, `imgui_impl_win32.h`,
  `imgui_impl_dx11.cpp`, `imgui_impl_dx11.h`
- **Upstream license text:** kept next to the files as
  `adapters/imgui/backends/LICENSE.txt`. That file is the authoritative license
  for those sources and is *not* replaced by this project's LICENSE.

## Dear ImGui — core library (Conan dependency)

The imgui adapter kit and the imgui test host link the core library from the
`imgui/1.92.8` Conan package; no copy of it lives in this repository.

- **Repository:** https://github.com/ocornut/imgui
- **Author:** Omar Cornut and Dear ImGui contributors
- **License:** MIT — Copyright (c) 2014-2026 Omar Cornut

## Qt 5 — optional UI toolkit (local install, not vendored)

The Qt adapter kit and the Qt test host link Qt 5 Widgets when a local Qt 5
installation is available (`CMAKE_PREFIX_PATH`). No Qt code is copied into this
repository.

- **Project:** https://www.qt.io/
- **License:** LGPLv3 / commercial (your installation's terms apply)
