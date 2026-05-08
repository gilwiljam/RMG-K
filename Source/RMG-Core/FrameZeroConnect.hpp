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
 * port, exchange HELLO/ACK including a ROM identity tag, and only
 * after the handshake succeeds does the UI auto-load the ROM and
 * start emulation. This guarantees that both peers begin GekkoNet's
 * session at byte-identical emulator state — there's no settle period
 * to drift across, no race between manual ROM-load clicks, and a
 * mismatched ROM is rejected before any emulator state is built.
 *
 * After the handshake the socket is closed so GekkoNet's own ASIO
 * adapter can rebind the same port for the actual game traffic.
 *
 * Protocol (informal):
 *   - Each peer sends "FZ_HELLO <local_identity>\n" to every
 *     configured peer address once every ~100 ms.
 *   - When a peer receives a HELLO from a known address whose
 *     identity matches the local identity, it sends "FZ_ACK\n" back.
 *     Mismatched identities flip status to RomMismatch and exit.
 *   - When a peer has both received HELLO (with matching identity)
 *     from every configured peer AND received ACK from every peer,
 *     status flips to Connected and the worker thread exits.
 *   - If timeout_seconds elapses first, status flips to TimedOut.
 *
 * The identity string is opaque to this module — callers typically
 * pass the ROM MD5 (optionally suffixed with the cartridge internal
 * name) so two peers can't accidentally play different ROMs.
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
        RomMismatch,  // a peer's identity tag did not match the local one
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
// local_identity:  opaque ROM identity tag (typically the MD5; max
//                  64 chars). Empty string disables the check.
CORE_EXPORT bool CoreFrameZeroConnectStart(unsigned short local_port,
                                            const std::vector<std::string>& peer_addrs,
                                            int timeout_seconds,
                                            const std::string& local_identity);

// Returns the remote identity received from the first peer that
// matched/mismatched. Useful for surfacing the diff when status is
// RomMismatch. Empty if no HELLO has been received yet.
CORE_EXPORT std::string CoreFrameZeroConnectGetRemoteIdentity(void);

// Cheap to poll. Reads an atomic.
CORE_EXPORT CoreFrameZero::ConnectStatus CoreFrameZeroConnectGetStatus(void);

// Joins the worker thread (waits up to a few hundred ms). Idempotent.
CORE_EXPORT void CoreFrameZeroConnectStop(void);

// Median round-trip time across the handshake's HELLO/ACK exchange,
// in milliseconds. Returns -1 when no samples have been collected
// (handshake hasn't run, no ACKs received yet). Used to pick a
// suitable input-delay value for the GekkoNet session — see
// MainWindow::pollFrameZeroConnectStatus.
CORE_EXPORT int CoreFrameZeroConnectGetMedianRttMs(void);

#endif // CORE_FRAMEZEROCONNECT_HPP
