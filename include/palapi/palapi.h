// PalApi public plugin interface.
//
// A plugin is a DLL that exports Plugin_Init and (optionally) Plugin_Unload. The framework passes
// a pointer to a PalApi struct of function pointers; the plugin calls back through it. This keeps
// a stable C ABI with no import library required.
//
//   extern "C" __declspec(dllexport) void Plugin_Init(const PalApi* api);
//   extern "C" __declspec(dllexport) void Plugin_Unload(void);
//
// The ABI grows by APPENDING: each version adds fields at the end of PalApi and bumps
// PALAPI_ABI_VERSION, so an older plugin's layout is never disturbed and it keeps working. A plugin
// checks `abi_version` before using a field newer than the version it was built against. Version map:
// v2 active surface (calls with parameters, certified state read/write, game-thread marshaling,
// hooking); v3 ProcessEvent observers + broadcast; v4 chat commands; v5 timers; v6 plugin config;
// v7 player utilities; v8 command help/aliases; v9 typed calls; v10 custom RCON commands;
// v11 host.panic (deliberate, attributed server crash). Each plugin
// receives its OWN PalApi whose `plugin_handle` uniquely identifies
// it (pass it to hook/command/timer calls so the framework can remove that plugin's registrations
// when it unloads).
#pragma once

#include <stdint.h>

#define PALAPI_ABI_VERSION 11u
#define PALAPI_VERSION     "0.1.1"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PalApiLog {
    void (*info)(const char* message);
    void (*warn)(const char* message);
    void (*error)(const char* message);
} PalApiLog;

typedef struct PalApiReflect {
    // Resolve a UClass definition by name. Returns its address, or 0 if not present.
    uint64_t (*find_class)(const char* class_name);
    // Object name / class name by reflection. Returns the length, 0 on failure.
    int (*name_of)(uint64_t object, char* out, int cap);
    int (*class_name_of)(uint64_t object, char* out, int cap);
} PalApiReflect;

// ---- ABI v2 ----------------------------------------------------------------------------------

typedef struct PalApiReflect2 {
    // First object named exactly `name` (e.g. a class-default object "Default__X"). 0 if none.
    uint64_t (*find_object_by_name)(const char* name);
    // First live (non-CDO) instance whose class name contains `class_substring`. 0 if none.
    uint64_t (*find_live_instance)(const char* class_substring);
    // The verified ProcessEvent address (0 if not located/verified).
    uint64_t (*process_event_addr)(void);
} PalApiReflect2;

typedef struct PalApiCall {
    // Call a no-argument int32 getter on a live instance of a class (name contains substring).
    // Returns 1 on success (and fills *out), 0 otherwise. MUST be called from the game thread
    // (i.e. inside a run_on_game_thread callback): these dispatch the live engine's ProcessEvent,
    // and calling it off the game thread races the engine and is refused when the hook is active.
    int (*call_getter_int)(const char* class_substring, const char* func_name, int* out);
    // Call a function taking `argc` int32 inputs and returning int32, on `instance`. The parms
    // layout is derived from certified offsets. Returns 1 on success (fills *out_ret), 0 otherwise.
    // MUST be called from the game thread (see call_getter_int).
    int (*call_function_ints)(uint64_t instance, const char* func_name,
                              const int* args, int argc, int* out_ret);
} PalApiCall;

typedef struct PalApiState {
    // Read a certified scalar property by name from a live instance. 1 on success.
    int (*read_int32)(uint64_t instance, const char* prop, int* out);
    int (*read_float)(uint64_t instance, const char* prop, float* out);
    // Absolute address of a certified scalar property on a live instance of a class. 1 on success.
    int (*resolve_field_addr)(const char* class_substring, const char* prop, uint64_t* out_addr);
    // Guarded scalar write (n in {1,2,4,8}); records undo. MUST be called from run_on_game_thread.
    // Returns 1 on success, 0 if the safety gate refuses. Never faults the server.
    int (*write_scalar)(uint64_t addr, const void* src, uint32_t n);
    // Revert the most recent guarded write. Returns 1 if one was reverted.
    int (*undo_last)(void);
} PalApiState;

typedef struct PalApiThread {
    // Run fn(user) on the engine game thread (drained inside the next ProcessEvent tick). Returns 1
    // if posted. Requires the framework game-thread hook to be active. Pass `plugin_handle` so a
    // still-queued job is dropped if the plugin unloads before it runs (a function pointer into an
    // unmapped DLL would crash); use api->plugin_handle.
    int (*run_on_game_thread)(void (*fn)(void* user), void* user, const void* plugin_handle);
} PalApiThread;

typedef struct PalApiHook {
    // Detour `target_addr`; *out_original receives a callable pointer to the original. The hook is
    // tagged with `plugin_handle` so it is removed automatically when the plugin unloads. Returns 1
    // on success. `label` is for logging.
    //
    // ONE hook per target. To react to ProcessEvent -- the engine's universal call dispatch that
    // several plugins commonly want -- do NOT set_hook(process_event_addr()): stacking detours on one
    // address is unsafe and the framework refuses it. Use events.register_processevent instead (one
    // shared framework hook fans out to every observer). set_hook remains for hooking OTHER functions.
    int (*set_hook)(uint64_t target_addr, void* detour, void** out_original,
                    const char* label, const void* plugin_handle);
    // Remove every hook owned by this plugin. Returns the number removed.
    int (*remove_my_hooks)(const void* plugin_handle);
} PalApiHook;

// ---- ABI v3 ----------------------------------------------------------------------------------

// A ProcessEvent observer callback. `self`/`function`/`parms` are ProcessEvent's own arguments
// (the target UObject, the UFunction being called, and its packed parameter buffer). `user` is the
// pointer passed at registration. Runs ON THE GAME THREAD, once per ProcessEvent call. It MUST:
// return promptly (it is on the engine's hot path); NOT block, loop, or terminate the thread; and
// NOT let a C++ exception propagate across this C boundary. The framework isolates a faulting
// callback (it will not crash the host) but a callback that hangs the game thread stalls the server.
typedef void (*PalProcessEventCallback)(void* self, void* function, void* parms, void* user);

typedef struct PalApiEvents {
    // Register a pre and/or post ProcessEvent observer (either may be NULL, but not both). The
    // callbacks fire on the game thread for every ProcessEvent call -- pre before the original runs,
    // post after. Returns a handle > 0, or 0 on failure (the game-thread hook is not active, or both
    // callbacks NULL). Tagged with `plugin_handle` (pass api->plugin_handle) so the observer is
    // removed automatically when the plugin unloads. This is the correct way for several plugins to
    // react to the same engine calls without stacking hooks.
    int (*register_processevent)(PalProcessEventCallback pre, PalProcessEventCallback post,
                                 void* user, const void* plugin_handle);
    // Unregister a single observer by the handle returned above. Returns 1 if one was removed.
    int (*unregister_processevent)(int handle);
} PalApiEvents;

typedef struct PalApiServer {
    // Broadcast a server notice (a message shown on-screen to every connected player), via
    // PalGameStateInGame::BroadcastServerNotice. `utf8_text` is the message (UTF-8). Returns 1 on
    // success. MUST be called from the game thread (inside a run_on_game_thread callback): it
    // dispatches an engine function.
    int (*broadcast_message)(const char* utf8_text);
} PalApiServer;

// ---- ABI v4 ----------------------------------------------------------------------------------

// A custom chat-command handler. `text` is the full chat line a player typed (UTF-8), e.g.
// "!ping hello". `user` is the pointer passed at registration. Runs ON THE GAME THREAD, once per
// message whose leading token matches the registered prefix. It MUST return promptly (it is on the
// engine hot path), MUST NOT block/loop or terminate the thread, MUST NOT let a C++ exception cross
// this C boundary (the framework isolates a faulting handler but a hang stalls the server), and MUST
// NOT register/unregister commands from within itself. It MAY broadcast a reply or read/call engine
// functions directly (it is already on the game thread).
typedef void (*PalCommandCallback)(const char* text, void* user);

typedef struct PalApiCommands {
    // Register `callback` for chat lines whose leading token is `prefix` (e.g. "!ping"). Matching is
    // exact-token: "!ping" matches "!ping" and "!ping arg" but not "!pinguin". Returns a handle > 0,
    // or 0 on failure (null/empty prefix, null callback, registry full, or the game-thread hook is
    // not active). Tagged with `plugin_handle` (pass api->plugin_handle) so the command is removed
    // automatically when the plugin unloads. Incoming chat is read on the server via the shared
    // ProcessEvent dispatcher -- no hook stacking.
    int (*register_command)(const char* prefix, PalCommandCallback callback, void* user,
                            const void* plugin_handle);
    // Unregister one command by the handle returned above. Returns 1 if one was removed. Blocks until
    // any in-flight dispatch of that handler finishes, so the caller may then free related state.
    int (*unregister_command)(int handle);
} PalApiCommands;

// ---- ABI v8 ----------------------------------------------------------------------------------

typedef struct PalApiCommands2 {
    // As commands.register_command, plus a short help line listed by the built-in "!help" (e.g.
    // "reply pong"). `description` may be NULL/empty -- the command is then listed by prefix alone.
    // The description is COPIED, so it may point into your DLL. Same return/ownership rules.
    //
    // The framework registers "!help" itself AFTER all plugins have loaded, and only if no plugin
    // claimed that prefix first: register "!help" yourself during Plugin_Init to own it instead.
    int (*register_command_ex)(const char* prefix, const char* description,
                               PalCommandCallback callback, void* user, const void* plugin_handle);
} PalApiCommands2;

// ---- ABI v9 ----------------------------------------------------------------------------------

// One positional input of a typed call: `size` bytes at `data`, copied verbatim into the
// parameter's slot. `size` must equal the parameter's reflected size in the engine (int32 and
// float = 4, bool = 1, double / int64 / object pointer = 8, a plain-data struct = its full size,
// e.g. FVector = 24 on this engine). The framework CHECKS the size against the live reflection and
// refuses on mismatch; the bit pattern (is this 4-byte slot an int or a float?) is your contract.
typedef struct PalApiValue {
    uint32_t    size;
    const void* data;
} PalApiValue;

// ---- ABI v10 ---------------------------------------------------------------------------------

// A custom RCON command handler. `command` is the full RCON command line (UTF-8, e.g. "palapi status").
// Write the reply into reply_out[reply_cap] (UTF-8, NUL-terminated) and return 1 = handled (the reply
// is delivered to the RCON client), 0 = not handled (native RCON handles the command, or no reply).
// THREAD: do NOT assume which thread this runs on. On the current Palworld build the RCON dispatcher
// runs on the GAME thread (not a separate network thread), so a handler that BLOCKS or loops would
// FREEZE THE WHOLE SERVER (all players/ticks/timers), not just RCON. It MUST return promptly, MUST NOT
// block, MUST NOT call any game-thread API (broadcast / call_* / write_scalar / players.*) or otherwise
// marshal to another thread, and MUST NOT let a C++ exception cross this boundary (the framework
// isolates a FAULT with SEH, but SEH cannot break a hang or an infinite loop). Keep it to quick,
// self-contained work that produces a short status/reply string.
typedef int (*PalRconHandler)(const char* command, char* reply_out, int reply_cap, void* user);

typedef struct PalApiRcon {
    // Register `handler` for RCON command lines whose leading token is `prefix` (e.g. "palapi" matches
    // "palapi" and "palapi status" but not "palapiX"). Returns a handle > 0, or 0 on failure:
    // null/empty prefix or handler, a prefix that is too long or contains a space, the registry is
    // full, the prefix collides CASE-INSENSITIVELY with a native admin command (Info/Save/Shutdown/
    // DoExit/Broadcast/KickPlayer/BanPlayer/UnBanPlayer/TeleportToPlayer/TeleportToMe/ShowPlayers --
    // a plugin must never swallow an admin op), or the prefix is already taken by another plugin
    // (FIRST registered wins for RCON). Tagged with `plugin_handle` (pass api->plugin_handle) so it is
    // removed automatically when the plugin unloads.
    //
    // Registration succeeds regardless of whether RCON interception is armed on this server: the
    // command only actually FIRES when the operator enabled RCON interception AND the reply path
    // certified on the running Palworld build (a game update can move it). Where interception is not
    // active, the registration is harmless and simply never dispatches -- native RCON is untouched.
    int (*register_command)(const char* prefix, PalRconHandler handler, void* user,
                            const void* plugin_handle);
    // Unregister one RCON command by the handle above. Returns 1 if removed. Blocks until any in-flight
    // dispatch of that handler finishes, so the caller may then free related state.
    int (*unregister_command)(int handle);
} PalApiRcon;

typedef struct PalApiCall2 {
    // Call a function with arbitrary BY-VALUE parameters -- scalars and plain-data structs -- and
    // read back a scalar/struct return. Pass ret_size 0 and ret_buf NULL for a void function;
    // otherwise ret_size must equal the reflected return size (checked, refused on mismatch).
    // Reference parameters are accepted only when CONST; a non-const out/reference parameter is
    // refused (write-back is not surfaced). Engine-managed types (FString, TArray, ...) are NOT
    // safe here -- use broadcast_message and friends for those. MUST be called from the game
    // thread (see call.call_getter_int). Returns 1 on success, 0 on any refusal or fault.
    int (*call_function_typed)(uint64_t instance, const char* func_name,
                               const PalApiValue* args, int argc,
                               void* ret_buf, uint32_t ret_size);
} PalApiCall2;

// ---- ABI v5 ----------------------------------------------------------------------------------

// A timer callback. `user` is the pointer passed at scheduling. Runs ON THE GAME THREAD when the
// timer is due. Same rules as a command callback: return promptly, do not block/loop, do not let a
// C++ exception cross this boundary. It MAY broadcast/read/call engine functions directly. It MUST
// NOT schedule or cancel timers from within itself (the pump holds the timer registry lock while it
// runs; those calls are refused, returning 0/failure, rather than deadlocking). A recurring timer
// keeps firing on its interval until it is cancelled (from outside a callback) or the plugin unloads.
typedef void (*PalTimerCallback)(void* user);

typedef struct PalApiTimers {
    // Run `callback(user)` ONCE, `delay_ms` milliseconds from now, on the game thread. Returns a
    // handle > 0, or 0 on failure (null callback, timers unavailable, registry full). Tagged with
    // `plugin_handle` (pass api->plugin_handle) so it is dropped automatically when the plugin unloads.
    int (*schedule_after)(uint32_t delay_ms, PalTimerCallback callback, void* user,
                          const void* plugin_handle);
    // Run `callback(user)` REPEATEDLY every `interval_ms` milliseconds (first fire after one interval),
    // on the game thread. Same return/tagging as schedule_after. interval_ms 0 is floored to 1 ms.
    int (*schedule_every)(uint32_t interval_ms, PalTimerCallback callback, void* user,
                          const void* plugin_handle);
    // Cancel one timer by the handle returned above. Returns 1 if one was removed.
    int (*cancel_timer)(int handle);
} PalApiTimers;

// ---- ABI v6 ----------------------------------------------------------------------------------

typedef struct PalApiConfig {
    // Read this plugin's OWN settings, declared as a flat "Config" object in its PluginInfo.json:
    //   "Config": { "greeting": "hi", "max_kills": 10, "announce": true, "spawn_rate": 1.5 }
    // This is READ-ONLY in-memory data -- no engine call, no game thread -- so it is callable at any
    // time, from any thread (including straight from Plugin_Init). Pass api->plugin_handle as
    // `plugin_handle`: it selects the calling plugin's own config namespace, so each plugin reads its
    // own settings by default. This is COOPERATIVE namespacing, NOT a security boundary -- plugins are
    // trusted in-process native code and could pass another handle; it isolates by convention, not by
    // enforcement. A missing key, or a key whose stored type differs from the getter, returns `def` (a
    // JSON number answers BOTH get_int and get_double; get_int truncates toward zero and is clamped to
    // the int range).
    //
    // get_string copies up to cap-1 bytes of the value into `out` (NUL-terminated) and returns the
    // length that WOULD be written (like reflect.name_of); `out` may be NULL only when cap is 0.
    int    (*get_string)(const void* plugin_handle, const char* key, const char* def,
                         char* out, int cap);
    int    (*get_int)(const void* plugin_handle, const char* key, int def);
    double (*get_double)(const void* plugin_handle, const char* key, double def);
    // Returns 1/0 (C has no bool in this ABI). `def` is also 0/1.
    int    (*get_bool)(const void* plugin_handle, const char* key, int def);
} PalApiConfig;

// ---- ABI v7 ----------------------------------------------------------------------------------

// Visitor for players.for_each. `player` is a live player object address; `user` is the pointer
// passed at the call. Return non-zero to continue enumeration, 0 to stop early. Runs on the calling
// thread (players enumeration is read-only and not marshaled), once per matching player.
typedef int (*PalPlayerVisitor)(uint64_t player, void* user);

typedef struct PalApiPlayers {
    // Inspect connected players. This is READ-ONLY memory scanning/reading through reflection -- no
    // engine call, no game thread, no mutation. On an empty server there are zero players: these still
    // work and return 0 / visit nothing (a non-zero count requires a real client -- that is a live test).
    //
    // COST WARNING: count() and for_each() walk the ENTIRE live UObject graph (hundreds of thousands
    // of objects under load), with NO time cap. Cheap on an idle server, NOT free on a populated one.
    // Call them from a background thread or an occasional timer, NEVER from the engine hot path -- do
    // NOT call them from a ProcessEvent observer, a chat command handler, or any game-thread callback,
    // or you will stall the server on every engine call. read_int/read_string on ONE object are cheap.

    // Count live (non class-default) instances whose class name contains `class_substring` (e.g.
    // "PalPlayerState"). Returns the count (0 if none or class_substring is null/empty).
    int (*count)(const char* class_substring);
    // Visit each live instance whose class name contains `class_substring`, calling visitor(obj,user);
    // the visitor returns non-zero to continue or 0 to stop early. Returns the number visited. The
    // `player` address is valid only for the duration of the visitor call -- do not retain it.
    int (*for_each)(const char* class_substring, PalPlayerVisitor visitor, void* user);
    // Read a certified int32 property by name from a player object (as yielded by for_each). Returns 1
    // on success (fills *out), 0 otherwise. Read-only, never faults.
    int (*read_int)(uint64_t player, const char* prop, int* out);
    // Read an FString property by name from a player object, decoded to UTF-8 into out[cap] (NUL-
    // terminated). Returns the string length written on success, or -1 on failure (which includes a
    // buffer smaller than 2 bytes). Read-only, never faults.
    int (*read_string)(uint64_t player, const char* prop, char* out, int cap);
} PalApiPlayers;

// ---- ABI v11 ---------------------------------------------------------------------------------

// Trust model: a PalApi plugin is trusted, in-process NATIVE code. It is free to do anything the
// host process can do -- including deliberately crashing the server, which is sometimes exactly the
// intended behaviour. The security boundary is the OPERATOR's choice of which plugins to enable, not
// the framework second-guessing a plugin. The framework's job is to make failures VISIBLE (which
// plugin, on which function) so the operator can act -- not to prevent a plugin from doing its job.
typedef struct PalApiHost {
    // Deliberately shut the server down, ATTRIBUTED to this plugin. Use it when a hard stop is the
    // intended behaviour. PalApi logs "PANIC: plugin '<name>': <reason>" and writes a
    // palapi_panic_*.txt naming this plugin and the reason next to the logs, THEN terminates the
    // process. This DOES NOT RETURN, and it happens even if called from inside a framework-guarded
    // callback (it is an intentional stop, not a fault to be isolated). `reason` may be NULL. Pass
    // api->plugin_handle so the panic is attributed to you.
    void (*panic)(const void* plugin_handle, const char* reason);
} PalApiHost;

typedef struct PalApi {
    uint32_t      abi_version;       // equals PALAPI_ABI_VERSION for this header
    const char*   framework_version; // human-readable framework version
    PalApiLog     log;
    PalApiReflect reflect;
    // ABI v2 additions (appended; v1 layout above is unchanged):
    const void*    plugin_handle;    // unique per plugin; pass to hook calls as the owner token
    PalApiReflect2 reflect2;
    PalApiCall     call;
    PalApiState    state;
    PalApiThread   thread;
    PalApiHook     hook;
    // ABI v3 additions (appended; v1/v2 layout above is unchanged):
    PalApiEvents   events;
    PalApiServer   server;
    // ABI v4 additions (appended; v1/v2/v3 layout above is unchanged):
    PalApiCommands commands;
    // ABI v5 additions (appended; v1..v4 layout above is unchanged):
    PalApiTimers   timers;
    // ABI v6 additions (appended; v1..v5 layout above is unchanged):
    PalApiConfig   config;
    // ABI v7 additions (appended; v1..v6 layout above is unchanged):
    PalApiPlayers  players;
    // ABI v8 additions (appended; v1..v7 layout above is unchanged):
    PalApiCommands2 commands2;
    // ABI v9 additions (appended; v1..v8 layout above is unchanged):
    PalApiCall2 call2;
    // ABI v10 additions (appended; v1..v9 layout above is unchanged):
    PalApiRcon rcon;
    // ABI v11 additions (appended; v1..v10 layout above is unchanged):
    PalApiHost host;
} PalApi;

typedef void (*PalApiPluginInit)(const PalApi* api);
typedef void (*PalApiPluginUnload)(void);

#ifdef __cplusplus
} // extern "C"
#endif
