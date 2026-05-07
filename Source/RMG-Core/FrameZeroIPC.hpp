#ifndef CORE_FRAMEZEROIPC_HPP
#define CORE_FRAMEZEROIPC_HPP

#include "Library.hpp"

#include <cstdint>

/*
 * Frame Zero in-game ↔ emulator IPC channel.
 *
 * Lets a patched SSB64 ROM communicate with RMG-K by writing a 32-bit
 * "command word" at a known RDRAM address. Each VI the emulator polls
 * the address; if the word is non-zero a callback fires with the cmd
 * value and the emulator clears the word as an ack. The game side can
 * busy-wait on the word becoming zero to know the cmd was processed.
 *
 * Used by the Path-A in-game menu integration: patched mode-select
 * dispatch writes a command (e.g. CMD_OPEN_OVERLAY) instead of doing
 * the original scene transition. The emulator side then drives the
 * Frame Zero connect/lobby flow and writes a result word back into a
 * neighbouring address before clearing the cmd word.
 *
 * The magic addresses are in the upper 4 MB of an 8 MB RDRAM map,
 * which SSB64 NTSC-U does not use (no expansion pak required). Safe
 * for arbitrary cooperation with the game's heap.
 */

namespace CoreFrameZero
{
    /* N64 virtual address of the IPC command word. Game writes the
     * command here, emulator reads it, dispatches, then writes 0 to
     * acknowledge. */
    constexpr uint32_t kIPCCommandAddr = 0x807FFFE0;

    /* Result word, populated by the emulator before clearing the cmd
     * word. Game can read this to learn the outcome of a command. */
    constexpr uint32_t kIPCResultAddr  = 0x807FFFE4;

    /* Command codes. Keep values stable — they're an ABI between the
     * patched ROM and the emulator. */
    enum IPCCommand : uint32_t
    {
        IPC_NONE             = 0,
        IPC_OPEN_OVERLAY     = 1,  // game requests the connect overlay
        IPC_TRIGGER_HANDSHAKE = 2, // game requests immediate handshake
                                   //  (lobby-completed scenario)
        IPC_END_SESSION      = 3,  // game requests session teardown
        IPC_PING             = 0xFF, // dev: write a value, expect echo
    };

    /* Callback dispatched on the emulation thread when a non-zero
     * command word is observed. Implementations should marshal to the
     * UI thread before touching Qt state. */
    typedef void (*IPCHandler)(uint32_t cmd, uint32_t cookie);
}

/* Resolve mupen-core hooks. Idempotent. Returns true on success. */
CORE_EXPORT bool CoreFrameZeroIPCInit(void);

/* Register the dispatch callback. Pass nullptr to clear. The callback
 * runs on the emulation thread. */
CORE_EXPORT void CoreFrameZeroIPCSetHandler(CoreFrameZero::IPCHandler handler);

/* Poll the magic RDRAM word; if non-zero, fire the registered handler
 * and clear the word. Cheap to call (one RDRAM read + branch on zero).
 * Intended to be called once per VI from the FrameCallback path. */
CORE_EXPORT void CoreFrameZeroIPCPoll(void);

/* Direct RDRAM read/write helpers for callers that want to inspect
 * arbitrary RDRAM addresses (e.g. result word handling). */
CORE_EXPORT uint32_t CoreFrameZeroIPCReadWord(uint32_t address);
CORE_EXPORT void     CoreFrameZeroIPCWriteWord(uint32_t address, uint32_t value);

#endif // CORE_FRAMEZEROIPC_HPP
