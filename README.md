# PalApi - plugin dev kit

This is the kit for writing your own **PalApi plugins** (server-side Palworld mods). If you just want
to run PalApi on your server, you dont need this repo - grab the ready-to-use release instead.

A plugin is a small Windows DLL. PalApi loads it into the dedicated server and hands it a C API so it
can read game state, call engine functions, run code on the game thread, react to chat, schedule
timers, answer custom RCON commands, and more. Clients stay vanilla.

## Whats in here

- `include/palapi/palapi.h` - the whole plugin API in one header (stable C ABI, version 11). This is
  the only file you include.
- `docs/PLUGIN_API.md` - the guide: how a plugin is structured, what each part of the API does, and
  the safety rules (very important: some calls are game-thread only, RCON handlers must never block).
- `docs/BUILDING.md` - how to build with the MSVC toolchain.
- `example/` - a full working plugin (`example.cpp` + `PluginInfo.json` + `build_example.bat`). Best
  starting point: copy it and change it.
- `bin/` - the PalApi binaries so you can load and test your plugin on a server (`version.dll`, or
  `PalApiLoader.exe` + `PalApiCore.dll`). Turn on `debug_log=true` in `palapi.cfg` while developing.

## Quick start

1. Copy the `example` folder, rename it.
2. Edit `example.cpp` - your entry points are `Plugin_Init(const PalApi* api)` and `Plugin_Unload()`.
3. Edit `PluginInfo.json` (name, and `MinApiVersion` = the API version you rely on).
4. Build it (see `build_example.bat`, it just calls the compiler with the right flags).
5. Drop the resulting folder in `PalApi\Plugins\` on a test server and start it. Watch `palapi.log`
   (or `palapi_debug.log` with `debug_log=true`).

## The one rule to remember

Read `docs/PLUGIN_API.md`, but if you remember one thing: **respect the thread rules**. Game state
reads/writes and calls happen on the game thread (use the provided marshaling). A custom RCON command
handler must return fast and never block - on the current build it runs on the game thread, so a slow
handler freezes the whole server. When in doubt, do less and return.

## License

MIT (see the source repo). Please dont resell it, and if you make the kit better, send it back.
