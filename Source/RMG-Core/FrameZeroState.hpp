/*
 * Frame Zero — rollback state ring buffer.
 *
 * Wraps mupen64plus-core's buffer-based savestate API
 * (savestates_save_to_buffer / savestates_load_from_buffer) with a fixed-size
 * ring of preallocated slots. Used by the rollback path to capture and restore
 * emulator state at frame boundaries.
 *
 * Threading: not thread-safe. Intended to be called from the emulation thread
 * (PIF sync callback / frame callback context).
 */
#ifndef CORE_FRAMEZEROSTATE_HPP
#define CORE_FRAMEZEROSTATE_HPP

#ifdef CORE_INTERNAL

#include <cstddef>
#include <cstdint>

namespace FrameZero
{

constexpr int kNumSlots = 8;

/* Initialise the ring. Resolves the savestate API symbols from the loaded
 * mupen64plus-core. Returns true on success. Call once after Core is hooked
 * but before the first capture/restore. */
bool stateInit();

/* Free all slot buffers and reset state. Idempotent. */
void stateShutdown();

/* True if Init succeeded and the API is usable. */
bool stateReady();

/* Returns the size in bytes of a single snapshot. 0 if not ready. */
size_t stateSnapshotSize();

/* Capture current emulator state into the given slot.
 * Returns true on success. Replaces any previous content of that slot. */
bool captureState(int slot);

/* Restore emulator state from the given slot.
 * Returns true on success. The slot must have been previously captured. */
bool restoreState(int slot);

/* True if the slot has been captured and not freed. */
bool slotIsValid(int slot);

/* Read-only access to the captured bytes for a slot.
 * Returns false if slot is invalid or out of range. The returned pointer
 * is owned by FrameZero and remains valid until the slot is overwritten
 * via captureState() or freed via stateShutdown(). */
bool slotBytes(int slot, const uint8_t** out_data, size_t* out_len);

/* Capture current emulator state into a caller-provided buffer.
 * dst_len must be >= stateSnapshotSize(). On success out_len holds the
 * actual bytes written. The frame number is used by the differential
 * encoder to schedule keyframes deterministically (frame % N == 0 ⇒
 * keyframe) and to identify which keyframe a delta references. Used by
 * GekkoNet's SaveEvent handler. */
bool captureToBuffer(uint8_t* dst, size_t dst_len, size_t* out_len, int frame);

/* Restore emulator state from a caller-provided buffer. Used by
 * GekkoNet's LoadEvent handler — the buffer is owned by GekkoNet. */
bool restoreFromBuffer(const uint8_t* src, size_t len);

} // namespace FrameZero

#endif // CORE_INTERNAL

#endif // CORE_FRAMEZEROSTATE_HPP
