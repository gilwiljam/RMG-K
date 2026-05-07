#ifndef CORE_FRAMEZEROCONNECT_HPP
#define CORE_FRAMEZEROCONNECT_HPP

#include "Library.hpp"

#include <string>
#include <vector>

/*
 * Frame Zero pre-emulation peer handshake.
 *
 * Used to coordinate two (or more) RMG-K instances BEFORE either of
 * them starts emulation. Both peers open a UDP socket on a known
 * port, exchange HELLO/ACK, and only after the handshake succeeds
 * does the UI auto-load the SSB64 ROM and start emulation. This
 * guarantees that both peers begin GekkoNet's session at byte-
 * identical emulator state — there's no settle period to drift
 * across, no race between manual ROM-load clicks.
 *
 * After the handshake the socket is closed so GekkoNet's own ASIO
 * adapter can rebind the same port for the actual game traffic.
 *
 * Protocol (informal):
 *   - Each peer sends "FZ_HELLO\n" to every configured peer address
 *     once every ~100 ms.
 *   - When a peer receives a HELLO from a new address, it sends
 *     "FZ_ACK\n" back.
 *   - When a peer has both received HELLO from every configured peer
 *     AND received ACK from every configured peer, status flips to
 *     Connected and the worker thread exits.
 *   - If timeout_seconds elapses first, status flips to TimedOut.
 *
 * Threading: Start() spawns a worker thread; status is read via an
 * atomic so the UI can poll cheaply. Stop() joins the thread.
 */

namespace CoreFrameZero
{
    enum class ConnectStatus
    {
        Idle,         // not started
        Connecting,   // worker running, waiting for peers
        Connected,    // all peers handshook successfully
        TimedOut,     // worker exited after timeout_seconds with no full handshake
        Error,        // socket bind failure or other I/O error
    };
}

// Start the handshake. Returns true if the worker thread was spawned
// (does not mean handshake succeeded — poll status). If a previous
// run is still running, this fails; caller should Stop() first.
//
// local_port:      UDP port to bind locally
// peer_addrs:      "host:port" strings for each peer; entries that
//                  don't parse are skipped
// timeout_seconds: how long to wait for the full handshake before
//                  flipping to TimedOut
CORE_EXPORT bool CoreFrameZeroConnectStart(unsigned short local_port,
                                            const std::vector<std::string>& peer_addrs,
                                            int timeout_seconds);

// Cheap to poll. Reads an atomic.
CORE_EXPORT CoreFrameZero::ConnectStatus CoreFrameZeroConnectGetStatus(void);

// Joins the worker thread (waits up to a few hundred ms). Idempotent.
CORE_EXPORT void CoreFrameZeroConnectStop(void);

#endif // CORE_FRAMEZEROCONNECT_HPP
