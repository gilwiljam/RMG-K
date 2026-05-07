/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL

#include "FrameZero.hpp"

#include "Callback.hpp"
#include "Error.hpp"
#include "FrameZeroState.hpp"
#include "m64p/Api.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef FRAME_ZERO
#define GEKKONET_STATIC
#include <gekkonet.h>
#endif

/* PIF struct layout (private duplicate of mupen64plus-core's pif.h —
 * the core header isn't on RMG-Core's include path). Must stay in sync
 * with the matching declaration in Emulation.cpp; both must mirror
 * core/src/device/pif/pif.h. */
extern "C" {
    struct fz_pif_channel
    {
        void*    jbd;
        const void* ijbd;
        uint8_t* tx;
        uint8_t* tx_buf;
        uint8_t* rx;
        uint8_t* rx_buf;
    };
    struct fz_pif
    {
        uint8_t* base;
        uint8_t* ram;
        struct fz_pif_channel channels[6];
    };
    enum {
        FZ_JCMD_STATUS            = 0x00,
        FZ_JCMD_CONTROLLER_READ   = 0x01,
        FZ_JCMD_RESET             = 0xff,
    };
    typedef void (*fz_pif_sync_callback_t)(struct fz_pif*);
    typedef void (*fz_frame_zero_pump_t)(unsigned int);
}

namespace
{
#ifdef FRAME_ZERO

constexpr int kFZMaxPlayers = 4;

/* The Frame Zero session is a singleton — there is exactly one rollback
 * conversation at a time per emulator process. */
GekkoSession*                   g_session = nullptr;
CoreFrameZero::SessionMode      g_mode    = CoreFrameZero::SessionMode::None;
bool                            g_init    = false;
int                             g_num_players = 0;

/* Local-input staging: PIF callback writes here on each first
 * JCMD_CONTROLLER_READ of a frame; pump reads it and feeds GekkoNet. */
std::atomic<uint32_t>           g_local_input_staged{0};
std::atomic<bool>               g_local_input_valid{false};

/* Synced-input cache: pump writes here from the most recent
 * AdvanceEvent; PIF callback reads on every channel poll. */
uint32_t                        g_synced_inputs[kFZMaxPlayers] = {0};
int                             g_synced_count = 0;
bool                            g_synced_have_data = false;

/* Per-frame "have we synced this frame" gate — same role as the
 * Kaillera s_SyncedThisFrame flag. Cleared in the pump (which fires
 * once per VI), set in the PIF callback. */
bool                            g_pif_synced_this_frame = false;

/* Pump instrumentation (FRAME_ZERO_PUMP_DEBUG=1). Logs per-call event
 * breakdown for the first kPumpDebugFrames pump invocations so we can
 * see exactly what GekkoNet emits — diagnoses whether StressSession
 * desyncs are caused by multi-AdvanceEvent rollback that our
 * single-frame-per-pump architecture can't drive. */
constexpr int kPumpDebugFrames = 50;
bool                            g_pump_debug = false;
bool                            g_pump_debug_resolved = false;
int                             g_pump_call_count = 0;

/* Set to true by the AdvanceEvent handler in fz_pump_one_frame; read
 * by the thread loop to decide whether to back off CPU when no
 * advance fired. */
bool                            g_pump_iter_advanced = false;

/* Mupen64plus-core resolved hooks. */
typedef void (*set_pif_sync_callback_t)(fz_pif_sync_callback_t);
typedef void (*core_set_frame_zero_pump_t)(fz_frame_zero_pump_t);
typedef void (*core_set_rollback_mode_t)(int);
typedef void (*core_set_pump_driven_t)(int);
typedef void (*core_resume_emu_t)(void);
typedef int  (*core_wait_for_park_t)(void);
typedef void (*core_signal_shutdown_t)(void);
typedef void (*core_clear_shutdown_t)(void);

set_pif_sync_callback_t         g_set_pif_sync = nullptr;
core_set_frame_zero_pump_t      g_set_pump     = nullptr;
core_set_rollback_mode_t        g_set_rollback = nullptr;
core_set_pump_driven_t          g_set_pump_driven   = nullptr;
core_resume_emu_t               g_resume_emu        = nullptr;
core_wait_for_park_t            g_wait_for_park     = nullptr;
core_signal_shutdown_t          g_signal_shutdown   = nullptr;
core_clear_shutdown_t            g_clear_shutdown   = nullptr;
bool                            g_attached     = false;

/* Pump thread state. */
std::thread                     g_pump_thread;
std::atomic<bool>               g_pump_running{false};

bool ensure_state_initialised()
{
    if (FrameZero::stateReady())
        return true;
    return FrameZero::stateInit();
}

void* fz_resolve_sym(void* handle, const char* name)
{
    if (handle == nullptr)
        return nullptr;
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

bool fz_resolve_core_hooks()
{
    if (g_set_pif_sync != nullptr && g_set_pump != nullptr && g_set_rollback != nullptr &&
        g_set_pump_driven != nullptr && g_resume_emu != nullptr && g_wait_for_park != nullptr &&
        g_signal_shutdown != nullptr && g_clear_shutdown != nullptr)
        return true;

    void* handle = m64p::Core.GetHandle();
    if (handle == nullptr)
        return false;

    g_set_pif_sync = (set_pif_sync_callback_t)
        fz_resolve_sym(handle, "set_pif_sync_callback");
    g_set_pump = (core_set_frame_zero_pump_t)
        fz_resolve_sym(handle, "core_set_frame_zero_pump");
    g_set_rollback = (core_set_rollback_mode_t)
        fz_resolve_sym(handle, "core_set_rollback_mode");
    g_set_pump_driven = (core_set_pump_driven_t)
        fz_resolve_sym(handle, "core_set_pump_driven");
    g_resume_emu = (core_resume_emu_t)
        fz_resolve_sym(handle, "core_resume_emulation_one_frame");
    g_wait_for_park = (core_wait_for_park_t)
        fz_resolve_sym(handle, "core_wait_for_park");
    g_signal_shutdown = (core_signal_shutdown_t)
        fz_resolve_sym(handle, "core_signal_pump_shutdown");
    g_clear_shutdown = (core_clear_shutdown_t)
        fz_resolve_sym(handle, "core_clear_pump_shutdown");

    return (g_set_pif_sync != nullptr && g_set_pump != nullptr && g_set_rollback != nullptr &&
            g_set_pump_driven != nullptr && g_resume_emu != nullptr && g_wait_for_park != nullptr &&
            g_signal_shutdown != nullptr && g_clear_shutdown != nullptr);
}

void fz_set_rollback(int mode)
{
    if (g_set_rollback != nullptr)
        g_set_rollback(mode);
}

/* PIF sync callback. Runs on the emulation thread for every PIF channel
 * scan. Stages the local input from channel 0 once per frame and
 * writes the cached synced inputs back to all channels. */
void fz_pif_sync_callback(struct fz_pif* pif)
{
    if (g_session == nullptr || pif == nullptr)
        return;

    /* On the first JCMD_CONTROLLER_READ this frame, snapshot the local
     * input that the input plugin already wrote to channel 0's rx
     * buffer. Subsequent reads in the same frame use the cache. */
    bool is_ctrl_read = (pif->channels[0].tx != nullptr &&
                        pif->channels[0].tx_buf[0] == FZ_JCMD_CONTROLLER_READ &&
                        pif->channels[0].rx_buf  != nullptr);

    if (is_ctrl_read && !g_pif_synced_this_frame)
    {
        const uint8_t* rx = pif->channels[0].rx_buf;
        uint32_t local = ((uint32_t)rx[0] << 24) |
                         ((uint32_t)rx[1] << 16) |
                         ((uint32_t)rx[2] <<  8) |
                          (uint32_t)rx[3];
        g_local_input_staged.store(local, std::memory_order_relaxed);
        g_local_input_valid.store(true, std::memory_order_release);
        g_pif_synced_this_frame = true;
    }

    /* Write synced inputs to all channels for every poll, matching
     * Kaillera's pattern. JCMD_STATUS / JCMD_RESET force a stock
     * controller response so the game detects all slots even before
     * GekkoNet has produced any AdvanceEvents. */
    for (int i = 0; i < g_num_players && i < kFZMaxPlayers; ++i)
    {
        if (pif->channels[i].tx == nullptr || pif->channels[i].rx == nullptr)
            continue;

        *pif->channels[i].rx &= ~0xC0;

        const uint8_t cmd = pif->channels[i].tx_buf[0];
        if (cmd == FZ_JCMD_STATUS || cmd == FZ_JCMD_RESET)
        {
            if (pif->channels[i].rx_buf != nullptr)
            {
                const uint16_t type = 0x0500;
                pif->channels[i].rx_buf[0] = (uint8_t)(type & 0xFF);
                pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                pif->channels[i].rx_buf[2] = 0;
            }
        }
        else if (cmd == FZ_JCMD_CONTROLLER_READ &&
                 g_synced_have_data &&
                 i < g_synced_count &&
                 pif->channels[i].rx_buf != nullptr)
        {
            uint8_t* rx = pif->channels[i].rx_buf;
            const uint32_t v = g_synced_inputs[i];
            rx[0] = (uint8_t)((v >> 24) & 0xFF);
            rx[1] = (uint8_t)((v >> 16) & 0xFF);
            rx[2] = (uint8_t)((v >>  8) & 0xFF);
            rx[3] = (uint8_t)( v        & 0xFF);
        }
    }
}

/* Process a single frame's worth of pump work. Called by the pump
 * thread once per emulation-frame park. The emulation thread is
 * parked at frame boundary while this runs, so save/restore are
 * race-free and we can synchronously drive AdvanceEvents by resuming
 * + re-waiting for park. */
static void fz_pump_one_frame(unsigned int current_frame)
{
    if (g_session == nullptr)
        return;

    /* Reset per-frame PIF gate so the next frame's first
     * JCMD_CONTROLLER_READ stages a fresh local snapshot. */
    g_pif_synced_this_frame = false;

    /* One-shot resolution of the debug env var. Auto-enables under
     * the stress harness since that's the main diagnostic context;
     * explicit override still works elsewhere. */
    if (!g_pump_debug_resolved)
    {
        const char* env = std::getenv("FRAME_ZERO_PUMP_DEBUG");
        const char* stress_env = std::getenv("FRAME_ZERO_STRESS");
        const bool env_on   = (env        != nullptr && env[0]        != '\0' && env[0]        != '0');
        const bool stress_on = (stress_env != nullptr && stress_env[0] != '\0' && stress_env[0] != '0');
        g_pump_debug = env_on || stress_on;
        g_pump_debug_resolved = true;
    }
    const bool log_this_call = g_pump_debug && (g_pump_call_count < kPumpDebugFrames);
    ++g_pump_call_count;

    /* Push staged local input into all local actors EVERY iteration —
     * GekkoNet expects this whether we have a fresh poll or not.
     * Before the first PIF poll fires (chicken-and-egg: emulation is
     * parked, can't poll), g_local_input_staged is zero, which is a
     * fine neutral input. Once the dynarec advances one frame via
     * AdvanceEvent below, PIF callback runs and stages real input
     * for subsequent iterations. */
    {
        uint32_t local = g_local_input_staged.load(std::memory_order_relaxed);
        for (int i = 0; i < g_num_players && i < kFZMaxPlayers; ++i)
        {
            gekko_add_local_input(g_session, i, &local);
        }
    }

    /* Drain session events: connection state, sync progress, desync
     * detection. For Phase 3 we just log them. */
    int count = 0;
    GekkoSessionEvent** session_events = gekko_session_events(g_session, &count);
    for (int i = 0; i < count; ++i)
    {
        GekkoSessionEvent* ev = session_events[i];
        if (ev == nullptr)
            continue;
        if (ev->type == GekkoDesyncDetected)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "[FrameZero] DESYNC frame=%d remote_handle=%d local_chk=0x%08x remote_chk=0x%08x",
                ev->data.desynced.frame,
                ev->data.desynced.remote_handle,
                ev->data.desynced.local_checksum,
                ev->data.desynced.remote_checksum);
            CoreAddCallbackMessage(CoreDebugMessageType::Error, buf);
        }
    }

    /* Drain game events. SaveEvent and LoadEvent are processed inline.
     * AdvanceEvent caches inputs for the next dynarec frame to consume
     * via fz_pif_sync_callback. */
    bool saw_load = false;
    bool saw_advance = false;
    bool last_advance_was_rolling_back = false;

    /* Instrumentation tallies. Cheap to compute even when not logging;
     * keeps the event loop tight and hot-path-free of branches. */
    int n_save = 0, n_load = 0, n_advance = 0, n_advance_rb = 0;
    int first_save_frame = -1, first_load_frame = -1;
    int first_adv_frame = -1, last_adv_frame = -1;

    count = 0;
    GekkoGameEvent** updates = gekko_update_session(g_session, &count);
    for (int i = 0; i < count; ++i)
    {
        GekkoGameEvent* ev = updates[i];
        if (ev == nullptr)
            continue;

        switch (ev->type)
        {
            case GekkoSaveEvent:
            {
                if (n_save == 0) first_save_frame = ev->data.save.frame;
                ++n_save;
                size_t written = 0;
                const size_t snap = FrameZero::stateSnapshotSize();
                if (snap == 0 || ev->data.save.state == nullptr)
                    break;
                if (FrameZero::captureToBuffer(ev->data.save.state, snap, &written))
                {
                    if (ev->data.save.state_len != nullptr)
                        *ev->data.save.state_len = (unsigned int)written;
                    if (ev->data.save.checksum != nullptr)
                    {
                        /* FNV-1a over the snapshot bytes — cheap, good
                         * enough for desync detection. */
                        uint32_t h = 2166136261u;
                        const uint8_t* p = ev->data.save.state;
                        for (size_t b = 0; b < written; ++b)
                        {
                            h ^= p[b];
                            h *= 16777619u;
                        }
                        *ev->data.save.checksum = h;
                    }
                }
                break;
            }
            case GekkoLoadEvent:
            {
                if (n_load == 0) first_load_frame = ev->data.load.frame;
                ++n_load;
                if (ev->data.load.state == nullptr || ev->data.load.state_len == 0)
                    break;
                FrameZero::restoreFromBuffer(ev->data.load.state,
                                             ev->data.load.state_len);
                saw_load = true;
                break;
            }
            case GekkoAdvanceEvent:
            {
                if (n_advance == 0) first_adv_frame = ev->data.adv.frame;
                last_adv_frame = ev->data.adv.frame;
                ++n_advance;
                if (ev->data.adv.rolling_back) ++n_advance_rb;
                /* The advance payload concatenates per-player inputs:
                 * input_size * num_players bytes. We assume input_size
                 * matches our 4-byte controller word. */
                if (ev->data.adv.inputs != nullptr)
                {
                    const unsigned int byte_count = ev->data.adv.input_len;
                    int n = (int)(byte_count / 4u);
                    if (n > kFZMaxPlayers) n = kFZMaxPlayers;
                    for (int p = 0; p < n; ++p)
                    {
                        const uint8_t* src = ev->data.adv.inputs + p * 4;
                        uint32_t v = ((uint32_t)src[0]      ) |
                                     ((uint32_t)src[1] <<  8) |
                                     ((uint32_t)src[2] << 16) |
                                     ((uint32_t)src[3] << 24);
                        g_synced_inputs[p] = v;
                    }
                    g_synced_count = n;
                    g_synced_have_data = true;
                }
                saw_advance = true;
                last_advance_was_rolling_back = ev->data.adv.rolling_back;

                /* Drive ONE emulator frame synchronously. Set the
                 * rollback flag for this frame's run, resume, wait
                 * for park. After this, the dynarec has executed
                 * exactly one VI and is parked again at the new
                 * frame boundary. The PIF callback will have read
                 * g_synced_inputs and fed them into PIF channels
                 * during the run. */
                fz_set_rollback(ev->data.adv.rolling_back ? 1 : 0);
                if (g_resume_emu) g_resume_emu();
                if (g_wait_for_park)
                {
                    if (!g_wait_for_park())
                    {
                        /* Shutdown signalled mid-event. Bail to caller
                         * which will exit the thread loop. */
                        if (log_this_call)
                        {
                            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                                "[FrameZero pump] shutdown during AdvanceEvent — exiting");
                        }
                        return;
                    }
                }
                /* Reset PIF gate for the freshly-parked frame so its
                 * first JCMD_CONTROLLER_READ stages local input. */
                g_pif_synced_this_frame = false;
                g_pump_iter_advanced = true;
                break;
            }
            default:
                break;
        }
    }

    if (log_this_call)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[FrameZero pump] call=%d  events=%d  S=%d L=%d A=%d (rb=%d)  "
            "save_f=%d load_f=%d adv_f=[%d..%d]",
            g_pump_call_count,
            count, n_save, n_load, n_advance, n_advance_rb,
            first_save_frame, first_load_frame,
            first_adv_frame, last_adv_frame);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
    }
    (void)current_frame;

    /* Trailing rollback bookkeeping for the no-advance-event case.
     * AdvanceEvents already set the flag inline before resuming, so
     * this only matters for Load-only emissions (rare) or no-event
     * pump iterations (also rare). */
    if (saw_load && !saw_advance)
    {
        fz_set_rollback(1);
    }
}

/* Pump thread entry point. Drives the pump loop until shutdown is
 * signalled. Each iteration corresponds to one gekko_update_session
 * call. AdvanceEvent handlers inside fz_pump_one_frame run the
 * emulator forward — that's the only mechanism that should advance
 * the simulation, since advancing outside GekkoNet's control breaks
 * determinism (our state moves ahead of what GekkoNet expects). */
static void fz_pump_thread_entry()
{
    while (g_pump_running.load(std::memory_order_acquire))
    {
        if (g_wait_for_park == nullptr) break;
        if (!g_wait_for_park())
        {
            /* Shutdown signalled. */
            break;
        }

        g_pump_iter_advanced = false;
        fz_pump_one_frame(0);

        /* Safety: if no AdvanceEvent fired this iteration, GekkoNet
         * likely doesn't have enough confirmed input to advance yet.
         * Sleep briefly so we don't spin the CPU at full speed.
         * Should be a transient cold-start condition; if it persists
         * the pump-debug log will show events=0 indefinitely. */
        if (!g_pump_iter_advanced)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

#endif // FRAME_ZERO
} // namespace

bool CoreInitFrameZero(void)
{
#ifdef FRAME_ZERO
    if (g_init)
        return true;

    /* GekkoNet itself has no global init step — sessions are independent.
     * We initialise FrameZero's snapshot ring here so save/load are ready
     * the moment a session starts. */
    if (!ensure_state_initialised())
    {
        CoreSetError("CoreInitFrameZero: FrameZero::stateInit() failed (mupen64plus-core too old?)");
        return false;
    }

    g_init = true;
    return true;
#else
    CoreSetError("CoreInitFrameZero: build does not include FRAME_ZERO support");
    return false;
#endif
}

bool CoreShutdownFrameZero(void)
{
#ifdef FRAME_ZERO
    CoreEndFrameZeroSession();
    FrameZero::stateShutdown();
    g_init = false;
    return true;
#else
    return false;
#endif
}

bool CoreHasInitFrameZero(void)
{
#ifdef FRAME_ZERO
    return g_init;
#else
    return false;
#endif
}

bool CoreStartFrameZeroStressSession(int num_players)
{
#ifdef FRAME_ZERO
    if (!g_init)
    {
        CoreSetError("CoreStartFrameZeroStressSession: not initialised — call CoreInitFrameZero first");
        return false;
    }
    if (g_session != nullptr)
    {
        CoreSetError("CoreStartFrameZeroStressSession: a session is already active");
        return false;
    }
    if (num_players < 1 || num_players > 4)
    {
        CoreSetError("CoreStartFrameZeroStressSession: num_players must be 1..4");
        return false;
    }

    /* The state size is plugin-dependent (HLE adds an RSP1 trailer; LLE
     * does not). Query it after plugins are attached. Phase 1's roundtrip
     * test confirmed this is stable for the lifetime of an emulation. */
    const size_t state_size = FrameZero::stateSnapshotSize();
    if (state_size == 0)
    {
        CoreSetError("CoreStartFrameZeroStressSession: snapshot size is 0 — is emulation running?");
        return false;
    }

    if (!gekko_create(&g_session, GekkoStressSession))
    {
        CoreSetError("CoreStartFrameZeroStressSession: gekko_create failed");
        g_session = nullptr;
        return false;
    }

    GekkoConfig config = {};
    config.num_players              = (unsigned char)num_players;
    config.max_spectators           = 0;
    config.input_prediction_window  = 6;     // typical fighting-game window
    config.spectator_delay          = 0;
    config.input_size               = 4;     // N64 controller word
    config.state_size               = (unsigned int)state_size;
    config.limited_saving           = false;
    config.desync_detection         = true;  // Stress mode emits desync events
    config.check_distance           = 10;    // frames apart for desync compare

    gekko_start(g_session, &config);

    /* In stress mode every actor is GekkoLocalPlayer; GekkoNet handles
     * both "halves" of the conversation locally. One-frame input delay
     * matches the reference example. */
    for (int i = 0; i < num_players; ++i)
    {
        gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
        gekko_set_local_delay(g_session, i, 1);
    }

    g_mode = CoreFrameZero::SessionMode::StressTest;
    g_num_players = num_players;

    /* Reset shared staging state so a previous session's data can't
     * leak into the new one. */
    g_local_input_staged.store(0, std::memory_order_relaxed);
    g_local_input_valid.store(false, std::memory_order_relaxed);
    for (int i = 0; i < kFZMaxPlayers; ++i) g_synced_inputs[i] = 0;
    g_synced_count = 0;
    g_synced_have_data = false;
    g_pif_synced_this_frame = false;
    g_pump_call_count = 0;
    g_pump_debug_resolved = false;

    /* Register PIF callback. NULL out the legacy single-thread pump
     * callback — Phase 3.5 uses pump-driven mode + a dedicated thread
     * instead. Displaces any Kaillera PIF callback from a prior
     * session; callers should not start Kaillera and Frame Zero on
     * the same emulation run. */
    if (!fz_resolve_core_hooks())
    {
        CoreSetError("CoreStartFrameZeroStressSession: required core exports missing — rebuild mupen64plus-core");
        gekko_destroy(&g_session);
        g_session = nullptr;
        g_mode = CoreFrameZero::SessionMode::None;
        return false;
    }
    g_set_pif_sync(fz_pif_sync_callback);
    g_set_pump(nullptr);
    fz_set_rollback(0);
    g_attached = true;

    /* Spin up the pump thread + park-driven mode. Clear any stale
     * shutdown flag from a prior session. Set pump_driven AFTER
     * thread is launched so the emulation thread sees pump_driven=1
     * and the pump thread is already in wait_for_park. */
    g_clear_shutdown();
    g_pump_running.store(true, std::memory_order_release);
    g_pump_thread = std::thread(fz_pump_thread_entry);
    g_set_pump_driven(1);

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "[FrameZero] StressSession started — pump thread driving emulation.");

    return true;
#else
    (void)num_players;
    CoreSetError("CoreStartFrameZeroStressSession: build does not include FRAME_ZERO support");
    return false;
#endif
}

bool CoreEndFrameZeroSession(void)
{
#ifdef FRAME_ZERO
    /* 1. Stop pump-driven mode so future new_frames don't park. The
     *    emulation thread might be currently parked; the shutdown
     *    signal below wakes it. */
    if (g_set_pump_driven != nullptr) g_set_pump_driven(0);

    /* 2. Tell pump thread to exit, and unblock any current waits on
     *    both sides. */
    g_pump_running.store(false, std::memory_order_release);
    if (g_signal_shutdown != nullptr) g_signal_shutdown();

    /* 3. Join pump thread. Both wait_for_park (pump thread) and the
     *    park CV (emulation thread) wake on the shutdown signal, so
     *    this should return promptly. */
    if (g_pump_thread.joinable())
        g_pump_thread.join();

    /* 4. Clear shutdown flag so the next session starts clean. */
    if (g_clear_shutdown != nullptr) g_clear_shutdown();

    /* 5. Detach core hooks so a stale PIF poll doesn't reach a
     *    destroyed session. */
    if (g_attached)
    {
        if (g_set_pif_sync != nullptr) g_set_pif_sync(nullptr);
        if (g_set_pump     != nullptr) g_set_pump(nullptr);
        fz_set_rollback(0);
        g_attached = false;
    }

    /* 6. Destroy the GekkoNet session. Pump thread is gone, no race. */
    if (g_session != nullptr)
    {
        gekko_destroy(&g_session);
        g_session = nullptr;
    }
    g_mode = CoreFrameZero::SessionMode::None;
    g_num_players = 0;
    g_synced_have_data = false;
    g_synced_count = 0;
    g_local_input_valid.store(false, std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
}

CoreFrameZero::SessionMode CoreGetFrameZeroSessionMode(void)
{
#ifdef FRAME_ZERO
    return g_mode;
#else
    return CoreFrameZero::SessionMode::None;
#endif
}

int CoreFrameZeroModifyPlayValues(void* /*values*/, int /*size*/, int /*num_players*/)
{
    /* Obsolete in the Phase 3 architecture. GekkoNet integration runs
     * via two core hooks: fz_pif_sync_callback (registered through
     * set_pif_sync_callback) drains and publishes per-frame input
     * to/from PIF RAM, and fz_pump_callback (registered through
     * core_set_frame_zero_pump) handles GekkoNet's save/load/advance
     * events at the frame boundary. This function is kept only so
     * legacy call sites compile; it never executes during a Frame
     * Zero session. */
    return -1;
}
