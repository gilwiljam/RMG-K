#ifndef CORE_FRAMEZERO_HPP
#define CORE_FRAMEZERO_HPP

#include "Library.hpp"

#include <cstdint>
#include <functional>
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

// Pre-arm the Frame Zero subsystem before any frame runs. Registers
// the PIF sync callback in "neutralize" mode so every controller
// channel returns "controller present, no buttons" until a session
// opens. Critical for cross-instance determinism — without this, two
// peers' input plugins poll real hardware (USB adapters, etc.)
// differently and the savestate captured at session-open has
// host-dependent bytes, producing immediate desync at frame 0.
//
// Idempotent. Returns true on success. Call from emulation start when
// FRAME_ZERO_ONLINE is set, before M64CMD_EXECUTE.
CORE_EXPORT bool CoreFrameZeroPreArm(void);

// Start a local stress-test session. Both actors are GekkoLocalPlayer;
// no networking. GekkoNet drives save/load/advance events as if it were
// a real rollback session, which exercises the determinism gate.
//
// num_players: 1..4 (typically 2 for SSB64 testing)
// Returns true on session create + start success.
CORE_EXPORT bool CoreStartFrameZeroStressSession(int num_players);

// Start an online (P2P) session over GekkoNet's built-in UDP/ASIO
// transport. One slot is the local player; the rest are remote.
//
// num_players:        total slot count (2..4 supported)
// local_player_index: 0..num_players-1, identifies which slot is local
// local_port:         UDP port to bind on this machine
// remote_addrs:       num_players entries; the local slot's entry is
//                     ignored, the others must be "host:port" strings
//                     in any standard form (e.g., "192.168.1.42:7000").
// input_delay:        local delay frames (typical 1..3 for fightig
//                     games; 0 for no artificial delay)
//
// Returns true on session create + start. Caller must subsequently
// drive emulation; the pump thread handles per-frame I/O.
CORE_EXPORT bool CoreStartFrameZeroOnlineSession(int num_players,
                                                 int local_player_index,
                                                 unsigned short local_port,
                                                 const std::string* remote_addrs,
                                                 int input_delay);

// End the current session, regardless of mode. Idempotent.
CORE_EXPORT bool CoreEndFrameZeroSession(void);

// Returns the current session mode. None if no session is active.
CORE_EXPORT CoreFrameZero::SessionMode CoreGetFrameZeroSessionMode(void);

// Obsolete — kept for ABI compatibility with the Phase 3 skeleton. Always
// returns -1 in the active architecture; per-frame work happens via the
// PIF sync callback and the new_frame() pump hook registered when a
// Frame Zero session starts.
CORE_EXPORT int CoreFrameZeroModifyPlayValues(void* values, int size, int num_players);

// Frame-advantage exposure (GGPO-article concept #1).
//
// GekkoNet tracks the local clock skew vs each remote peer via the
// frame_advantage field exchanged on input ACK. gekko_frames_ahead()
// returns the average across remotes — positive means we're running
// ahead of the peer, negative means we're behind. The example
// integrations slow the local frame pacing by ~1.6 % when the value
// exceeds 0.5 to bring the two sides back into ±1 frame.
//
// Returns 0.0 when no session is active. Cheap to call; reads a single
// float maintained by GekkoNet.
CORE_EXPORT float CoreGetFrameZeroFramesAhead(void);

// OSD notification hook. RMG-Core can't reach the OnScreenDisplay
// directly (lives in RMG GUI), so the GUI registers a callback here and
// the pump invokes it when something user-visible happens (rollback
// activity, desync onset, etc.). The callback is invoked from the
// emulation thread; the registered handler is responsible for
// marshalling to the UI thread before touching OSD state.
//
// Pass an empty std::function to clear. Idempotent.
CORE_EXPORT void CoreSetFrameZeroOSDNotifier(std::function<void(std::string)> notifier);

#endif // CORE_FRAMEZERO_HPP
