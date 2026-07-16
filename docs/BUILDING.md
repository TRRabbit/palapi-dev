# Building PalApi

## Requirements

- Windows x64.
- MSVC Build Tools 2022 (the "Desktop development with C++" workload). The build scripts call
  `vcvars64.bat` from the default install path; adjust the path in the scripts if yours differs.

## Build the framework

```
build\build.bat
```

Produces `build\version.dll` — the loader/core/resolver/plugin-manager, ready to deploy (see
`docs/TESTING.md`).

## Run the unit tests

```
tests\build_tests.bat
```

Builds and runs the off-process unit tests (name-table decoding, object-array validation and
indexing, JSON manifest parsing, config parsing). These require no game server.

## Build the demo plugin

```
plugins\demo\build_demo.bat
```

Produces `plugins\demo\demo.dll`, a minimal read-only example. See `docs/TESTING.md` for how to
deploy a plugin.

## Build the loader (alternative deployment)

```
build\build_core.bat
build\build_loaderexe.bat
```

Produces `build\PalApiCore.dll` (the same framework as `version.dll`, but without the version.dll
export-forwarding, meant to be injected rather than picked up by the OS's DLL search order) and
`build\PalApiLoader.exe` (the launcher-injector). This is an ALTERNATIVE to the `version.dll` proxy,
not a replacement: the proxy stays the simple default. See `docs/TESTING.md` for how to deploy and
run the loader.

## Layout

```
include/palapi/palapi.h   Public plugin API (what a plugin includes)
src/loader/               Layer 1: proxy DLL (dllmain.cpp) + injected-DLL entry (coremain.cpp) + deferred start-up
src/loaderexe/            Launcher-injector (PalApiLoader.exe): process creation, DLL injection, watchdog
src/core/                 Logging, guarded memory access, config, JSON
src/resolver/             Layer 2/3: GNames, object array, reflection walks, auto-documentation
src/hooks/                Game-thread marshaling + hook manager (Detours)
src/plugins/              Layer 4: plugin manager
plugins/demo/             Example plugin (read-only)
plugins/example/          Example plugin (active: call/state/hook/game-thread)
tests/                    Off-process unit tests
```
