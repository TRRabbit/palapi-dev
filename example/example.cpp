// PalApi active example plugin (ABI v7).
//
// Unlike the read-only demo, this exercises the full active surface through the framework: it calls
// an engine function with parameters, reads a certified game-state scalar, observes ProcessEvent to
// count engine calls, and posts work to the game thread. It never writes to the game; every effect
// is reversible and removed on unload. It shows a real plugin wiring, not a gameplay feature.
//
// Note (ABI v3): it counts ProcessEvent calls by REGISTERING AN OBSERVER (events.register_processevent),
// NOT by set_hook-ing ProcessEvent. Several plugins may observe ProcessEvent at once; they share the
// framework's single real hook. Stacking one's own detour on ProcessEvent is unsafe and refused.
#include "palapi/palapi.h"

#include <windows.h>
#include <cstdio>

static const PalApi* g_api        = nullptr;
static const void*   g_handle     = nullptr;
static int           g_peObserver = 0;     // ProcessEvent observer handle (0 = none)
static int           g_pingCmd    = 0;     // "!ping" command handle (0 = none)
static int           g_rconCmd    = 0;     // "palapi" RCON command handle (0 = none)
static int           g_tickTimer  = 0;     // recurring timer handle (0 = none)
static volatile LONG64 g_calls = 0;
static volatile LONG64 g_ticks = 0;

// Recurring timer callback (ABI v5): fires on the game thread every interval. Kept trivial (a
// counter); a real plugin might autosave, announce, or expire something here.
static void OnTick(void* user) {
    (void)user;
    InterlockedIncrement64(&g_ticks);
}

// Chat-command handler (ABI v4): a player typed a line whose leading token is "!ping". Runs on the
// game thread, so it may broadcast a reply directly. `text` is the full line (e.g. "!ping hello").
static void OnPingCommand(const char* text, void* user) {
    (void)user;
    const PalApi* api = g_api;
    if (!api) {
        return;
    }
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Example plugin: pong! (you typed \"%s\")", text);
    api->server.broadcast_message(buf); // on the game thread already: allowed
    api->log.info("example: handled !ping");
}

// RCON command handler (ABI v10): an operator sent an RCON command whose leading token is "palapi"
// (e.g. via rcon.py). The dispatcher thread is NOT guaranteed -- on the current build it is the GAME
// thread, so a slow/blocking handler would freeze the whole server. It must be quick, never block, and
// must NOT call any game-thread API (broadcast/call/write/players). Reading our own atomic counters is
// fine. Write a short reply; return 1 = handled (the reply reaches the RCON client).
static int OnRconStatus(const char* command, char* reply, int cap, void* user) {
    (void)command; (void)user;
    _snprintf_s(reply, cap, _TRUNCATE,
                "PalApi example: alive. ProcessEvent calls=%lld, timer ticks=%lld.",
                (long long)g_calls, (long long)g_ticks);
    return 1;
}

// RCON command handler (ABI v11 DEMO): shows host.panic. Sending "palpanic" over RCON makes THIS plugin
// deliberately shut the server down -- attributed (the log + palapi_panic_*.txt name this plugin). This
// is only a demonstration of the API; a real plugin would call panic only on an unrecoverable condition.
static int OnRconPanic(const char* command, char* reply, int cap, void* user) {
    (void)command; (void)reply; (void)cap; (void)user;
    if (g_api && g_api->abi_version >= 11) {
        g_api->host.panic(g_handle, "example plugin: 'palpanic' RCON command (intentional demo shutdown)");
    }
    return 0; // unreachable once panic fired; if ABI < 11, decline and let native RCON handle it
}

// Player visitor (ABI v7): called once per connected player during players.for_each. Reads one
// per-player field by reflection and logs it. Read-only. Returns 1 to keep enumerating. On an empty
// server this is never called (0 players); a real client exercises it (a live test).
static int ExamplePlayerVisitor(uint64_t player, void* user) {
    const PalApi* api = static_cast<const PalApi*>(user);
    if (!api) {
        return 1;
    }
    char name[128];
    int len = api->players.read_string(player, "PlayerName", name, sizeof(name));
    int camps = -1;
    api->players.read_int(player, "BaseCampBuildingNum", &camps);
    char buf[192];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "example: player obj=%llx name=\"%s\"(len=%d) camps=%d",
                (unsigned long long)player, len >= 0 ? name : "?", len, camps);
    api->log.info(buf);
    return 1; // continue enumeration
}

// Post-observer: counts every engine ProcessEvent call. Runs on the game thread; kept trivial.
static void ExamplePeObserver(void* self, void* function, void* parms, void* user) {
    (void)self; (void)function; (void)parms; (void)user;
    InterlockedIncrement64(&g_calls);
}

// Runs on the game thread. Engine calls (call_function_ints / call_getter_int) and certified reads
// MUST run here, not from Plugin_Init: dispatching ProcessEvent off the game thread races the live
// engine (the framework refuses it). This is the correct pattern for plugin authors to follow.
static void ExerciseApiOnGameThread(void* user) {
    const PalApi* api = static_cast<const PalApi*>(user);
    char buf[192];

    // (a) Call a function WITH parameters and verify the result.
    uint64_t mathCdo = api->reflect2.find_object_by_name("Default__KismetMathLibrary");
    if (mathCdo) {
        int args[2] = {20, 22};
        int result = 0;
        if (api->call.call_function_ints(mathCdo, "Add_IntInt", args, 2, &result)) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "example: Add_IntInt(20,22)=%d", result);
            api->log.info(buf);
        }
    }

    // (b) Read a certified scalar and cross-check it against the reflected getter.
    uint64_t inst = api->reflect2.find_live_instance("PalGameStateInGame");
    int viaRead = -1, viaGetter = -1;
    if (inst) {
        api->state.read_int32(inst, "MaxPlayerNum", &viaRead);
    }
    api->call.call_getter_int("PalGameStateInGame", "GetMaxPlayerNum", &viaGetter);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "example: on game thread (tid=%lu) MaxPlayerNum read=%d getter=%d",
                GetCurrentThreadId(), viaRead, viaGetter);
    api->log.info(buf);
}

extern "C" __declspec(dllexport) void Plugin_Init(const PalApi* api) {
    g_api = api;
    if (!api) {
        return;
    }
    g_handle = api->plugin_handle;

    char buf[192];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "example: loaded, framework=%s ABI=%u",
                api->framework_version, api->abi_version);
    api->log.info(buf);
    if (api->abi_version < 2) {
        api->log.warn("example: needs ABI v2; active features disabled");
        return;
    }

    // (1) Observe ProcessEvent to count engine calls -- via the shared framework hook, NOT our own
    // detour. Auto-removed on unload (tagged with our handle). Requires ABI v3.
    if (api->abi_version >= 3) {
        g_peObserver = api->events.register_processevent(nullptr, &ExamplePeObserver, nullptr, g_handle);
        if (g_peObserver > 0) {
            api->log.info("example: ProcessEvent observer registered (shared hook, no stacking)");
        }
    } else {
        api->log.warn("example: ABI < 3; ProcessEvent observing disabled");
    }

    // (2) Register a custom chat command: a player typing "!ping" gets a "pong" reply. Auto-removed on
    // unload (tagged with our handle). This is the incoming-command surface.
    //
    // From ABI v8 we register it WITH a help line, so the framework's built-in "!help" can tell players
    // the command exists -- describing your commands is the difference between a feature players use
    // and one only you know about. Older frameworks: fall back to the v4 call (same command, no help).
    if (api->abi_version >= 8) {
        g_pingCmd = api->commands2.register_command_ex("!ping", "reply pong", &OnPingCommand, nullptr,
                                                       g_handle);
        if (g_pingCmd > 0) {
            api->log.info("example: '!ping' chat command registered (listed in !help)");
        }
    } else if (api->abi_version >= 4) {
        g_pingCmd = api->commands.register_command("!ping", &OnPingCommand, nullptr, g_handle);
        if (g_pingCmd > 0) {
            api->log.info("example: '!ping' chat command registered");
        }
    } else {
        api->log.warn("example: ABI < 4; chat commands disabled");
    }

    // (2b) Register a custom RCON command (ABI v10): an operator typing "palapi" over RCON gets a
    // status reply. This runs on the NETWORK thread (unlike the chat "!ping", which is game-thread),
    // so the handler stays quick and touches no game-thread API. Registration always succeeds; it only
    // fires when the operator enabled RCON interception and the reply path certified on this build.
    if (api->abi_version >= 10) {
        g_rconCmd = api->rcon.register_command("palapi", &OnRconStatus, nullptr, g_handle);
        if (g_rconCmd > 0) {
            api->log.info("example: 'palapi' RCON command registered (answered when interception is on)");
        }
    } else {
        api->log.warn("example: ABI < 10; custom RCON commands disabled");
    }

    // (2c) DEMO of host.panic (ABI v11): register "palpanic" so an operator can trigger a deliberate,
    // attributed server shutdown over RCON. A real plugin would not expose this; it is here to show the
    // API and to let the crash-attribution artifact be proven end to end on a test server.
    if (api->abi_version >= 11) {
        api->rcon.register_command("palpanic", &OnRconPanic, nullptr, g_handle);
        api->log.info("example: 'palpanic' RCON command registered (DEMO: intentionally crashes the server)");
    }

    // (3) Schedule a recurring timer (every 30s) on the game thread. Requires ABI v5. Auto-removed on
    // unload (tagged with our handle). A real plugin might autosave or announce on this tick.
    if (api->abi_version >= 5) {
        g_tickTimer = api->timers.schedule_every(30000, &OnTick, nullptr, g_handle);
        if (g_tickTimer > 0) {
            api->log.info("example: 30s recurring timer scheduled");
        }
    } else {
        api->log.warn("example: ABI < 5; timers disabled");
    }

    // (4) Read our OWN settings from the "Config" block of our PluginInfo.json (ABI v6). This is
    // read-only in-memory data -- safe to read here on the loader thread, no game thread needed. A
    // real plugin would drive its behaviour from these (e.g. skip an announce when "announce" is
    // false). Missing keys fall back to the defaults passed in.
    if (api->abi_version >= 6) {
        char greeting[128];
        api->config.get_string(g_handle, "greeting", "(default greeting)", greeting, sizeof(greeting));
        int    maxKills  = api->config.get_int(g_handle, "max_kills", -1);
        int    announce  = api->config.get_bool(g_handle, "announce", 0);
        double spawnRate = api->config.get_double(g_handle, "spawn_rate", 0.0);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "example: config greeting=\"%s\" max_kills=%d announce=%d spawn_rate=%.2f",
                    greeting, maxKills, announce, spawnRate);
        api->log.info(buf);
    } else {
        api->log.warn("example: ABI < 6; plugin config disabled");
    }

    // (6) Enumerate connected players (ABI v7). Read-only reflection scan -- no engine, no game
    // thread -- so it is fine to run here. A real plugin might list names, check a per-player field, etc.
    //
    // This runs ONCE, at plugin load, which on a normal boot is before anyone has joined -- so 0 here
    // is the expected answer and says nothing about whether enumeration works with a player online.
    // (Not an impossibility, a timing fact: load happens after the loader's init delay, so a very fast
    // joiner could in principle already be counted.) Wording this honestly matters -- the older
    // "players connected = 0" line was read as a live measurement and sent a whole session chasing a
    // bug the evidence never showed (2026-07-15). To watch the count while a client is connected, poll
    // it OFF the game thread (see the framework's live_probe).
    if (api->abi_version >= 7) {
        int nStates = api->players.count("PalPlayerState");
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "example: players at plugin-load time (once, at boot; not a live measurement) "
                    "(PalPlayerState) = %d", nStates);
        api->log.info(buf);
        // Demonstrate for_each + a per-player read (0 iterations on an empty server, proves the path).
        api->players.for_each("PalPlayerState", &ExamplePlayerVisitor, const_cast<PalApi*>(api));
    } else {
        api->log.warn("example: ABI < 7; player utilities disabled");
    }

    // (7) Exercise the call/read surface -- but on the GAME THREAD, never from Plugin_Init (which
    // runs on the injected loader thread). Tag the job with our handle so it is dropped if we unload.
    api->thread.run_on_game_thread(&ExerciseApiOnGameThread, const_cast<PalApi*>(api), g_handle);
}

extern "C" __declspec(dllexport) void Plugin_Unload(void) {
    if (!g_api) {
        return;
    }
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "example: unloading, observed %lld ProcessEvent calls, %lld timer ticks",
                (long long)g_calls, (long long)g_ticks);
    g_api->log.info(buf);
    // Explicitly drop our observer and any hooks. The framework also auto-purges by owner on unload,
    // so this is good form rather than strictly required.
    if (g_peObserver > 0) {
        g_api->events.unregister_processevent(g_peObserver);
        g_peObserver = 0;
    }
    if (g_pingCmd > 0) {
        g_api->commands.unregister_command(g_pingCmd);
        g_pingCmd = 0;
    }
    if (g_rconCmd > 0) {
        g_api->rcon.unregister_command(g_rconCmd);
        g_rconCmd = 0;
    }
    if (g_tickTimer > 0) {
        g_api->timers.cancel_timer(g_tickTimer);
        g_tickTimer = 0;
    }
    g_api->hook.remove_my_hooks(g_handle);
}
