/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_FRAMEZERO_HPP
#define CORE_FRAMEZERO_HPP

#include "Library.hpp"

#include <cstdint>
#include <string>

/*
 * Frame Zero session integration — wraps GekkoNet (rollback netcode SDK)
 * for SSB64-specific play. The Phase 1 byte-deterministic savestate
 * roundtrip is the foundation; this module is the runtime that consumes
 * it.
 *
 * Three session types in the long-term plan:
 *   - StressTest (local, no networking) — Phase 3, the determinism gate
 *   - Online      (P2P UDP via GekkoNet's built-in ASIO transport) — Phase 4
 *   - Spectator   — Phase 7
 *
 * Build: gated on the `FRAME_ZERO` CMake option; when off, all functions
 * here become no-ops returning false. Callers should not assume the
 * session ran just because Init succeeded.
 */

namespace CoreFrameZero
{
    enum class SessionMode
    {
        None,        // No active Frame Zero session
        StressTest,  // Local 2-player sim with both actors local — sync test
        Online,      // P2P session (Phase 4)
    };
}

//
// Exported Functions
//

// Initialise the Frame Zero subsystem. Idempotent.
// Returns true on success. When FRAME_ZERO is disabled at build time,
// returns false and CoreSetError() explains why.
CORE_EXPORT bool CoreInitFrameZero(void);

// Tear down any active session and free GekkoNet resources.
CORE_EXPORT bool CoreShutdownFrameZero(void);

// True if Init succeeded and GekkoNet is usable.
CORE_EXPORT bool CoreHasInitFrameZero(void);

// Start a local stress-test session. Both actors are GekkoLocalPlayer;
// no networking. GekkoNet drives save/load/advance events as if it were
// a real rollback session, which exercises the determinism gate.
//
// num_players: 1..4 (typically 2 for SSB64 testing)
// Returns true on session create + start success.
CORE_EXPORT bool CoreStartFrameZeroStressSession(int num_players);

// End the current session, regardless of mode. Idempotent.
CORE_EXPORT bool CoreEndFrameZeroSession(void);

// Returns the current session mode. None if no session is active.
CORE_EXPORT CoreFrameZero::SessionMode CoreGetFrameZeroSessionMode(void);

// Obsolete — kept for ABI compatibility with the Phase 3 skeleton. Always
// returns -1 in the active architecture; per-frame work happens via the
// PIF sync callback and the new_frame() pump hook registered when a
// Frame Zero session starts.
CORE_EXPORT int CoreFrameZeroModifyPlayValues(void* values, int size, int num_players);

#endif // CORE_FRAMEZERO_HPP
