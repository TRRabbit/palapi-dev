# Plugin API

A PalApi plugin is a native DLL that the framework loads from
`<server>\Pal\Binaries\Win64\PalApi\Plugins\<Name>\<Name>.dll`, alongside a `PluginInfo.json`
manifest. The plugin exports two C entry points and talks to the framework through a struct of
function pointers (`include/palapi/palapi.h`). No import library is required.

```cpp
#include "palapi/palapi.h"

extern "C" __declspec(dllexport) void Plugin_Init(const PalApi* api);
extern "C" __declspec(dllexport) void Plugin_Unload(void); // optional
```

## Manifest

`PluginInfo.json` next to the DLL:

```json
{ "FullName": "Example", "Version": 0.2, "MinApiVersion": 2, "Enabled": true, "Dependencies": [] }
```

| Key | Meaning |
|---|---|
| `FullName` | Name used in logs and by other plugins' `Dependencies`. Defaults to the folder name. |
| `Version` | Yours; logged at load. |
| `MinApiVersion` | The plugin is skipped if this exceeds the framework's ABI version. |
| `Enabled` | `false` parks the plugin: it is skipped and its DLL is never loaded. Absent = enabled. |
| `Dependencies` | Names (`FullName`) of plugins yours needs. **Reported, not ordered** — see below. |
| `Config` | Flat object of your own settings, readable at runtime via `api->config` (ABI v6). |

**What `Dependencies` does and does not do.** After every plugin has loaded, the framework warns for
each declared name that is not among the loaded plugins. It does **not** load your dependency first
(plugins load in directory order), and it does **not** unload you if a dependency is missing. Treat it
as a diagnostic for the server operator, not a guarantee: if your plugin needs another one to be
initialised, check for it yourself at the moment you use it.

## Shipping your own DLLs beside your plugin

A plugin may ship third-party DLLs (a database client, a protocol library…) in its own folder,
next to its `.dll`. What is and is not covered:

- **Covered — static imports.** When the framework loads your plugin, its import table is resolved
  from **your plugin's folder first**, then the server directory, then the system. Your folder is
  yours alone at this stage: another plugin's folder is never searched, so two plugins can each ship
  the same DLL name in different versions without stepping on one another — and a plugin whose
  dependency is missing fails loudly (error 126 in the log) instead of silently borrowing a
  neighbour's copy.
- **Covered — later `LoadLibraryEx` opt-in loads.** Your folder stays registered (via
  `AddDllDirectory`) for the plugin's lifetime, so a `LoadLibraryEx(name, nullptr,
  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` you make later finds DLLs beside you. Note this registration is
  process-wide by nature: such opt-in lookups see **every** plugin's folder, in unspecified order —
  prefer an absolute path when the exact file matters.
- **Not covered.** A bare `LoadLibrary("name.dll")` and MSVC delay-load (`/DELAYLOAD`) do **not**
  search your folder. For those, build an absolute path from your own module location.

## Hot reload (replace a plugin without restarting the server)

Copy the **new** dll into the plugin's folder as **`<Name>.dll.palapi`** (e.g.
`Plugins\Example\Example.dll.palapi`). A watcher (poll every 5s by default) notices it, unloads the
running plugin cleanly — `Plugin_Unload`, then hooks, timers, commands and queued jobs are removed,
and the framework waits for in-flight callbacks to drain — swaps the files, and loads the new dll.
The previous dll is kept as `<Name>.dll.old` (one generation) so an operator can roll back by hand.

Rules and limits:

- A plugin that installed its own **engine detours** (`api->hook.set_hook`) is **refused** hot
  reload: removing a detour cannot drain a thread currently executing its body, so unmapping right
  after removal is a crash window. Restart the server to replace such a plugin. (Observers, chat
  commands and timers are all fine -- they go through framework registries that can be drained.)
- If the old plugin otherwise **cannot be unloaded safely**, the reload is **refused**: the new dll
  is parked as `<Name>.dll.rejected`, an error is logged, and that plugin is not retried until the
  server restarts. Two live copies of one plugin hooked into the engine would crash it; the
  framework prefers a leak (documented policy: leak > crash).
- If the **new dll fails to load**, the previous version is restored and reloaded automatically
  (the bad dll is parked as `<Name>.dll.rejected`).
- Security note: anyone who can write into `Plugins\` can already run arbitrary code in the server
  process -- that is what a plugin IS. Keep that directory writable by trusted operators only; the
  hot-reload sentinel does not change this boundary, it only makes the existing one convenient.
- A folder dropped after boot works too (fresh install through the same sentinel), **except** on a
  server that booted with an empty `Plugins` directory: there the chat/timer services were never
  brought up, so the very first plugin requires a restart.
- `Plugin_Init` of the new version runs on the watcher thread (not the game thread), exactly like
  at boot. Registrations made there are safe.
- Config: `hot_reload = false` disables the watcher; `hot_reload_poll_ms` (default 5000, with a 1s
  minimum floor to protect the disk; no upper limit) tunes the poll.

## The PalApi struct

`api->abi_version` is the ABI version the framework provides (the current header is ABI v14). The
struct is extended only by **appending at the root of `PalApi`**, so a plugin built against an older
header keeps working — check `api->abi_version` before using a field newer than the version you built
against. Note for contributors: a new call must go in a **new** sub-struct appended at the end, never
as a new field inside an already-shipped sub-struct (those are embedded by value, so growing one
shifts every member after it and breaks older plugins).

> The full, always-current reference for every field and function (with signatures and per-call
> notes) is **`docs/palapi-api.html`** — open it in a browser. The list below is a quick map.

Foundation (v1–v3):

- `api->log.info / warn / error` — write to the framework log.
- `api->reflect` — `find_class(name)`, `name_of(obj,...)`, `class_name_of(obj,...)`.
- `api->reflect2` — `find_object_by_name(name)`, `find_live_instance(class_substring)`,
  `process_event_addr()`.
- `api->call` — `call_getter_int(class_substring, func, &out)` and
  `call_function_ints(instance, func, args, argc, &out)`. **Must be called from the game thread**
  (see below): they dispatch the engine's `ProcessEvent`.
- `api->state` — `read_int32` / `read_float` (certified reads), `resolve_field_addr`,
  `write_scalar(addr, src, n)` and `undo_last()`. Writes **and** undo **must** run on the game thread.
- `api->thread.run_on_game_thread(fn, user, plugin_handle)` — run `fn(user)` on the engine game
  thread. Pass `api->plugin_handle` so a queued job is dropped if your plugin unloads first.
- `api->hook.set_hook(target_addr, detour, &original, label, plugin_handle)` and
  `remove_my_hooks(plugin_handle)` — for hooking functions OTHER than `ProcessEvent`.
- `api->events` (ABI v3) — `register_processevent(pre, post, user, plugin_handle)` and
  `unregister_processevent(handle)`: observe `ProcessEvent` without hooking it (see below).
- `api->events2` (ABI v13) — `register_interceptor(cb, user, plugin_handle)` / `unregister_interceptor(handle)`:
  like an observer, but the callback returns a verdict and can **block** the engine call
  (`PALAPI_EVENT_BLOCK`). The anti-cheat / filter / guard building block. A block is honoured for **any**
  function the interceptor targets — the framework does not second-guess it (trust model: a plugin is
  trusted native code; the boundary is the operator's choice of which plugins to enable). If the blocked
  function is not a plain void (it returns a value or has an out/reference parameter), its engine caller may
  read a stale return/out slot as if the call had run — you own that risk, so **test what you block**; the
  framework logs a one-time warning for visibility but honours the block. A faulting interceptor is treated
  as *proceed* (fail-open). It shares the single ProcessEvent dispatcher with the observers — no extra hook.
  **You MUST filter by function name and proceed for the rest** (compare `api->reflect.name_of(function)`);
  blocking everything would stop the server. Game thread.
- `api->server.broadcast_message(utf8)` (ABI v3) — show a server notice to every connected player
  (game thread).
- `api->plugin_handle` — your unique handle; pass it to the hook, thread, event, command, timer and
  RCON calls.

Surface added later (each appended, guarded by `abi_version`):

- `api->commands` (v4) / `api->commands2` (v8) — register a `!`-prefixed **chat command** handler
  (with an optional help line listed by the built-in `!help`). Fires on the game thread.
- `api->timers` (v5) — `schedule_after` / `schedule_every` / `cancel_timer`: run a callback on the
  game thread after a delay or on an interval. Dropped automatically on unload.
- `api->config` (v6) — read your OWN settings from the manifest `Config` object
  (`get_string/int/double/bool`). Read-only, callable from any thread (even `Plugin_Init`).
- `api->players` (v7) — `count` / `for_each` connected players and `read_int` / `read_string` a
  certified field per player. Read-only scan; keep it OFF the engine hot path (it walks the whole
  object graph).
- `api->call2.call_function_typed` (v9) — call a function with typed by-value arguments (scalars and
  plain-data structs) and read a typed return. Game thread.
- `api->rcon` (v10) — register a custom **RCON command** handler (`register_command` /
  `unregister_command`). **The operator must enable RCON interception** for it to fire: set
  `rcon_intercept = true` in `palapi.cfg` (see [Custom RCON commands](#custom-rcon-commands-abi-v10)
  below). Registration itself always succeeds; without interception armed it simply never dispatches
  and native RCON is untouched.
- `api->host.panic(plugin_handle, reason)` (v11) — deliberately shut the server down, attributed to
  you (see the trust-model section).
- `api->server2` (v12) — `send_message_to_player(uid16, channel, utf8)` posts a free-text chat line
  naming ONE player; `send_message_to_players(uids16, count, channel, utf8)` does the same for a
  packed list of 16-byte PlayerUIds (`count` 1..128) and returns how many were delivered.
  **Not private:** these route through the replicated chat pipeline, so the line is broadcast to
  **every** connected player **and attributed to the target player** (it reads as if that player
  typed it). Use them for a per-player-addressed *public* line, not for private feedback. `channel`
  selects the chat category byte; 0 is the default channel known to display in-game, other values are
  forwarded as-is (the plugin owns testing what a given category does on the live build). Game thread
  only; at most 512 UTF-16 units of text (malformed or over-long UTF-8 is refused, never truncated).
- `api->server3.send_private_message(uid16, utf8)` (v14) — deliver one **truly private** free-text
  line to a single player: visible to THAT player only, shown as a system message (not attributed to
  the player, not broadcast). It routes through the standard Unreal NetClient path
  (`PlayerController::ClientMessage`), which the server replicates to the owning client alone. Same
  512-UTF-16-unit cap and game-thread contract as above. Returns 1 when delivered to the client, 0
  when the player is offline or a signature gate refused. This is the primitive to use for private
  ORP / admin feedback.

## Custom RCON commands (ABI v10)

`api->rcon.register_command(prefix, handler, user, api->plugin_handle)` routes RCON command lines
whose leading token matches `prefix` (e.g. `"palapi"` matches `palapi` and `palapi status`) to your
handler; write the reply into the supplied buffer and return 1 (handled) or 0 (let native RCON take
it). A prefix that collides case-insensitively with a native admin command (`Info`, `Save`,
`Shutdown`, `Broadcast`, `KickPlayer`, …) is refused — a plugin must never swallow an admin op.

Two things the operator controls:

- **Interception is opt-in.** Your command only fires when the operator sets `rcon_intercept = true`
  in `palapi.cfg`. Left off (the default), your registration is harmless and never dispatches; native
  RCON behaves exactly as stock. This exists because intercepting RCON means hooking the live server's
  command path, which an operator should consciously turn on.
- **Threading (important).** On the current Palworld build the RCON dispatcher runs on the **game
  thread**, so a handler that blocks or loops would freeze the WHOLE server. Keep it to quick,
  self-contained work that produces a short reply string; do NOT call any game-thread API
  (`broadcast` / `call_*` / `write_scalar` / `players.*`) from it. See the header comment on
  `PalApiRcon` for the full contract.

## The game-thread rule

Calling engine functions or writing state off the game thread races the live server and is
**refused whenever the call is not on the adopted game thread** (the call returns 0/failure). It is
not a switch that opens once a hook is active: if the marshaling hook never came up, these calls stay
refused rather than run unsafely. `Plugin_Init` runs on a background loader thread, so do that work
inside a `run_on_game_thread` callback instead:

```cpp
static void OnGameThread(void* user) {
    const PalApi* api = static_cast<const PalApi*>(user);
    int max = -1;
    uint64_t gs = api->reflect2.find_live_instance("PalGameStateInGame");
    if (gs) api->state.read_int32(gs, "MaxPlayerNum", &max);
    // call_function_ints / write_scalar are also safe here
}

void Plugin_Init(const PalApi* api) {
    api->thread.run_on_game_thread(&OnGameThread, const_cast<PalApi*>(api), api->plugin_handle);
}
```

## Observing ProcessEvent (the common case)

`ProcessEvent` is the engine's universal call dispatch -- every `UFunction` call flows through it,
so it is the one function many plugins want to watch (to react to a specific game event, count
calls, etc.). **Do not `set_hook` it.** The framework already owns a single real hook on
`ProcessEvent`; register an observer instead and it fans out to you:

```cpp
static void OnProcessEvent(void* self, void* function, void* parms, void* user) {
    // Runs on the game thread for EVERY ProcessEvent call. Keep it fast. `function` is the
    // UFunction being dispatched -- compare it to a resolved address to act on one function only.
}

void Plugin_Init(const PalApi* api) {
    if (api->abi_version >= 3)
        api->events.register_processevent(/*pre*/nullptr, /*post*/&OnProcessEvent,
                                          /*user*/nullptr, api->plugin_handle);
}
```

Several plugins can observe at once -- they share the single hook, so there is no stacking and no
ordering hazard. Pre-callbacks run before the original, post-callbacks after. Observers are removed
automatically on unload (tagged with your handle); you may also drop one with
`unregister_processevent(handle)`. A callback that itself dispatches its own target must guard
against recursion (it fires at every nesting depth).

## Hooks (other functions)

`set_hook` detours a resolved address and hands back a pointer to the original to chain to. Your
detour must match the target's calling convention and forward to the original. Hooks are removed
automatically when the plugin unloads (before the DLL is unmapped); you may also remove them
explicitly with `remove_my_hooks(api->plugin_handle)`.

**One hook per target address.** `set_hook` refuses to install a second hook on an address that is
already hooked (by any plugin) -- stacking two independent Detours attaches on the same address
reproducibly crashed the server in testing. For the one
target where multiple consumers are common -- `ProcessEvent` -- use the observer registry above
instead. `set_hook(process_event_addr(), ...)` is refused (the framework's own hook already holds
that address).

A worked plugin exercising the whole surface lives in `plugins/example/`.

## Trust model and deliberate shutdown (`host.panic`, ABI v11)

A plugin is trusted, in-process native code. It is free to do anything the host process can do,
including deliberately crashing the server -- which is sometimes exactly the intended behaviour. The
security boundary is the **operator's choice of which plugins to enable**, not the framework
second-guessing a plugin. What the framework guarantees is not prevention but **attribution**: when a
plugin faults, it is isolated and the log names the plugin (and, for a ProcessEvent observer, the
engine function it was reacting to) so the operator can disable it or report it to its author; on a
full crash, a `palapi_crash_*.txt` next to the logs names the last plugin that was running.

When a hard stop is the behaviour you want, do it cleanly and attributably with `host.panic`:

```cpp
if (something_unrecoverable) {
    api->host.panic(api->plugin_handle, "save file is corrupt, refusing to run");
    // does not return
}
```

This logs `PANIC: plugin '<name>': <reason>`, writes a `palapi_panic_*.txt` naming your plugin and the
reason next to the logs, then terminates the process -- guaranteed even if called from inside a
framework-guarded callback (an intentional stop is not isolated like an accidental fault).

## Building a plugin

Compile as an x64 DLL against `include/`, statically linking the CRT to match the framework:

```
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /I <palapi>/include example.cpp /Fe:Example.dll
```
