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

#include "Error.hpp"
#include "FrameZeroState.hpp"

#ifdef FRAME_ZERO
#define GEKKONET_STATIC
#include <gekkonet.h>
#endif

namespace
{
#ifdef FRAME_ZERO

/* The Frame Zero session is a singleton — there is exactly one rollback
 * conversation at a time per emulator process. */
GekkoSession*                   g_session = nullptr;
CoreFrameZero::SessionMode      g_mode    = CoreFrameZero::SessionMode::None;
bool                            g_init    = false;

bool ensure_state_initialised()
{
    if (FrameZero::stateReady())
        return true;
    return FrameZero::stateInit();
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
    if (g_session != nullptr)
    {
        gekko_destroy(&g_session);
        g_session = nullptr;
    }
    g_mode = CoreFrameZero::SessionMode::None;
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

int CoreFrameZeroModifyPlayValues(void* values, int size, int num_players)
{
#ifdef FRAME_ZERO
    /* Phase 3 skeleton: not yet implemented.
     *
     * The full per-frame pump needs:
     *   1. Read local input from `values[0]`, call gekko_add_local_input.
     *   2. Drain gekko_session_events (connection / sync / desync events).
     *   3. Drain gekko_update_session — for each event:
     *        GekkoSaveEvent    → fill in event->data.save.state and friends
     *                            from a captureState() of the live emulator.
     *        GekkoLoadEvent    → restoreState() from event->data.load.state.
     *        GekkoAdvanceEvent → inject inputs into PIF, advance one frame.
     *   4. Write the synchronised inputs from GekkoNet back into `values`.
     *
     * The blocker for step (3) is that GekkoAdvanceEvent expects a
     * synchronous frame advance, and the existing `advance_one_frame_silent`
     * mupen64plus-core export is non-blocking (sets flags and returns).
     * A blocking variant — or restructuring the pump to run from
     * FrameCallback rather than the PIF sync path — is the next step
     * before this function can do real work.
     */
    (void)values;
    (void)size;
    (void)num_players;
    return -1;
#else
    (void)values;
    (void)size;
    (void)num_players;
    return -1;
#endif
}
