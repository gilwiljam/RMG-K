#ifndef CORE_FRAMEZERONETSHIM_HPP
#define CORE_FRAMEZERONETSHIM_HPP

#ifdef CORE_INTERNAL

#ifdef FRAME_ZERO
#define GEKKONET_STATIC
#include <gekkonet.h>
#endif

namespace FrameZeroNetShim
{

#ifdef FRAME_ZERO
/*
 * Wrap a GekkoNetAdapter with simulated latency / jitter / packet loss.
 *
 * Reads three env vars (any may be unset):
 *   FRAME_ZERO_NETSIM_LATENCY_MS  base outbound delay, default 0
 *   FRAME_ZERO_NETSIM_JITTER_MS   uniform random extra 0..jitter, default 0
 *   FRAME_ZERO_NETSIM_LOSS_PCT    drop probability 0..100, default 0
 *
 * Returns the original `underlying` adapter unchanged when all three
 * are zero/unset (so production paths stay zero-overhead). Otherwise
 * returns a static GekkoNetAdapter whose function pointers route
 * through an internal queue. Designed to be called from
 * CoreStartFrameZeroOnlineSession just before gekko_net_adapter_set.
 *
 * The shim only delays *outbound* sends — applying delay symmetrically
 * on both peers naturally gives a 2*latency RTT, no need to also
 * delay inbound. Packets are released by piggy-backing on the next
 * receive_data call (called every pump tick), so timing accuracy is
 * limited to the pump cadence — fine for ms-scale rollback testing.
 *
 * Idempotent: calling Wrap twice in the same process re-reads env
 * vars and reconfigures the queue. Pending packets are dropped.
 */
GekkoNetAdapter* Wrap(GekkoNetAdapter* underlying);

/*
 * Drain and free any queued packets and disable the shim. Called from
 * CoreEndFrameZeroSession so a stale queue doesn't bleed into the next
 * session.
 */
void Reset();
#endif // FRAME_ZERO

} // namespace FrameZeroNetShim

#endif // CORE_INTERNAL

#endif // CORE_FRAMEZERONETSHIM_HPP
