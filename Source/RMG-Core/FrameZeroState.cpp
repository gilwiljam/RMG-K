/*
 * Frame Zero — rollback state ring buffer (implementation).
 */
#define CORE_INTERNAL
#include "FrameZeroState.hpp"

#include "m64p/Api.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace FrameZero
{

namespace
{

using SaveToBufferFn        = int (*)(uint8_t**, size_t*);
using SaveToBufferInplaceFn = int (*)(uint8_t*, size_t, size_t*);
using LoadFromBufferFn      = int (*)(const uint8_t*, size_t);
using GetStateSizeFn        = size_t (*)(void);

struct Slot
{
    uint8_t* data = nullptr;
    size_t   size = 0;
    bool     valid = false;
};

struct State
{
    bool ready = false;
    SaveToBufferFn        saveToBuffer        = nullptr;
    SaveToBufferInplaceFn saveToBufferInplace = nullptr;  // optional, may be null on older cores
    LoadFromBufferFn      loadFromBuffer      = nullptr;
    GetStateSizeFn        getStateSize        = nullptr;
    size_t uncompressedSize = 0;  // raw m64p savestate size
    size_t snapshotSize = 0;      // size we report to GekkoNet (compressed worst-case)
    Slot slots[kNumSlots];

    /* Differential savestate state.
     *
     * Slot format is now 1-byte type prefix + compressed payload:
     *   type 0 = keyframe  (payload is the full state, skip-zero compressed)
     *   type 1 = delta     (payload is XOR of state vs the most recent keyframe,
     *                       skip-zero compressed — typically ~95% zero so the
     *                       existing compressor squeezes it down to a few KB)
     *
     * Save scheduling is deterministic — every kKeyframeInterval frames is a
     * keyframe, the others are deltas from the most recent keyframe. The
     * "shadow" buffer holds the most recent keyframe's full uncompressed
     * state so we can XOR against it on save and apply on load. */
    uint8_t* keyframeShadow      = nullptr;  // 16 MB, holds a keyframe state
    size_t   keyframeShadowSize  = 0;
    int      keyframeShadowFrame = -1;       // which frame is in shadow; -1 = none

    /* In-place save scratch. m64p writes the savestate directly into
     * this buffer (skipping its internal 16 MB malloc + memset + the
     * subsequent memcpy into our path). On keyframe saves we'd
     * normally point m64p at the shadow directly, but for deltas we
     * need BOTH the new state AND the previous keyframe in shadow
     * during the XOR pass — so this scratch is the new-state
     * destination on every save, then memcpy'd into shadow on
     * keyframes. Allocated lazily on first save; freed in
     * stateShutdown. */
    uint8_t* inplaceScratch     = nullptr;
    size_t   inplaceScratchSize = 0;

    /* Small ring of recent compressed keyframes so we can repopulate
     * the shadow when GekkoNet rolls back across a keyframe boundary.
     * Indexed by (keyframe_frame / kKeyframeInterval) % kKeyframeRingSize.
     * Each entry's compressed payload is owned by us — sized to fit the
     * worst-case keyframe (~ uncompressed_size bytes worst case, but in
     * practice ~4 MB for SSB64). */
    struct KeyframeRingEntry
    {
        int      frame = -1;
        uint8_t* data  = nullptr;
        size_t   size  = 0;
        size_t   cap   = 0;
    };
    static constexpr int kKeyframeRingSize = 16;
    KeyframeRingEntry keyframeRing[kKeyframeRingSize];
};

static State s_state;

/* Keyframe interval. Every Nth save event is a full snapshot; the rest
 * are XOR-deltas from the most recent keyframe. N=4 trades steady-state
 * save savings for shorter rollback walks: max 3 deltas to apply on
 * rollback, vs 7+ at N=8. See the design notes for the math. */
constexpr int kKeyframeInterval = 4;

/* Slot header. Five bytes at the front of every GekkoNet slot tells
 * the load path how to interpret the rest. The frame number is stored
 * explicitly so a delta load can locate its reference keyframe in the
 * keyframe ring without relying on shadow state matching the current
 * rollback target. */
enum SlotType : uint8_t
{
    SLOT_KEYFRAME = 0,
    SLOT_DELTA    = 1,
};

#pragma pack(push, 1)
struct SlotHeader
{
    uint8_t  type;       // SlotType
    int32_t  frame;      // frame number this slot represents
};
#pragma pack(pop)
static_assert(sizeof(SlotHeader) == 5, "slot header layout");

/* Skip-zero block compression for the GekkoNet rollback path.
 *
 * The mupen64plus savestate is ~16 MB and ~7 MB of that is zero-filled
 * RDRAM (chunks 8-14 in the 16-chunk view all hash to FNV-of-zeroes).
 * Compressing those zero regions out drops the per-frame save/load and
 * checksum work proportionally, freeing budget for the rollback re-sim.
 *
 * Format is dead-simple — one bit per fixed-size chunk, then concatenated
 * non-zero chunk payloads. Deterministic (same input → same output) so
 * cross-peer hash comparison still works on compressed bytes:
 *
 *   struct { u32 magic; u32 uncomp_len; u32 chunk_size; }
 *   bitmap[ceil(num_chunks/8)]   bit set ⇒ chunk has data, clear ⇒ all zero
 *   data[]                       concatenated non-zero chunks
 *
 * 4 KB chunk size: small enough to capture sparse non-zero data without
 * dragging in too much zero padding, large enough that the bitmap is
 * tiny (512 bytes for a 16 MB snapshot). Page-aligned by accident which
 * also maps onto how the OS already pages large allocations. */
constexpr uint32_t kCompressMagic    = 0x46535A43u;  // 'CZSF' little-endian
constexpr uint32_t kCompressVersion  = 1u;
constexpr uint32_t kCompressChunk    = 4096u;

struct CompressHeader
{
    uint32_t magic;
    uint32_t uncomp_len;
    uint32_t chunk_size;
};
static_assert(sizeof(CompressHeader) == 12, "compress header layout");

inline bool chunk_is_zero(const uint8_t* p, size_t len)
{
    /* Strided 8-byte zero check. Modern compilers auto-vectorise the
     * loop body to SSE2 movdqa/pcmpeqb at /O2, which scans memory at
     * close to memcpy bandwidth. */
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    const size_t words = len / 8;
    for (size_t i = 0; i < words; ++i)
    {
        if (w[i] != 0) return false;
    }
    for (size_t i = words * 8; i < len; ++i)
    {
        if (p[i] != 0) return false;
    }
    return true;
}

/* Returns the maximum compressed length for the given uncompressed
 * size — header + bitmap + worst-case-all-non-zero payload. Used to
 * size the destination buffer GekkoNet allocates per slot. */
inline size_t compressed_capacity(size_t uncomp_len)
{
    if (uncomp_len == 0) return sizeof(CompressHeader);
    const size_t num_chunks   = (uncomp_len + kCompressChunk - 1) / kCompressChunk;
    const size_t bitmap_bytes = (num_chunks + 7) / 8;
    return sizeof(CompressHeader) + bitmap_bytes + uncomp_len;
}

/* Compresses src → dst. Returns the bytes written, or 0 on failure
 * (dst too small or invalid input). */
size_t compress_snapshot(const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_capacity)
{
    if (src == nullptr || dst == nullptr) return 0;
    if (dst_capacity < compressed_capacity(src_len)) return 0;

    const size_t num_chunks   = (src_len + kCompressChunk - 1) / kCompressChunk;
    const size_t bitmap_bytes = (num_chunks + 7) / 8;

    CompressHeader hdr;
    hdr.magic      = kCompressMagic;
    hdr.uncomp_len = (uint32_t)src_len;
    hdr.chunk_size = kCompressChunk;
    std::memcpy(dst, &hdr, sizeof(hdr));

    uint8_t* bitmap = dst + sizeof(CompressHeader);
    std::memset(bitmap, 0, bitmap_bytes);

    uint8_t* data_out = bitmap + bitmap_bytes;
    size_t   data_pos = 0;

    for (size_t c = 0; c < num_chunks; ++c)
    {
        const size_t off  = c * kCompressChunk;
        const size_t span = (off + kCompressChunk <= src_len)
                          ? kCompressChunk
                          : (src_len - off);
        if (chunk_is_zero(src + off, span))
        {
            /* Bitmap bit stays 0 → decode side memsets back to zero. */
        }
        else
        {
            bitmap[c >> 3] |= (uint8_t)(1u << (c & 7));
            std::memcpy(data_out + data_pos, src + off, span);
            data_pos += span;
        }
    }

    return sizeof(CompressHeader) + bitmap_bytes + data_pos;
}

/* Decompresses src → dst. Returns the uncompressed bytes written, or 0
 * on failure (bad header / dst too small / truncated payload). */
size_t decompress_snapshot(const uint8_t* src, size_t src_len,
                           uint8_t* dst, size_t dst_capacity)
{
    if (src == nullptr || dst == nullptr) return 0;
    if (src_len < sizeof(CompressHeader)) return 0;

    CompressHeader hdr;
    std::memcpy(&hdr, src, sizeof(hdr));
    if (hdr.magic      != kCompressMagic) return 0;
    if (hdr.chunk_size != kCompressChunk) return 0;
    if (hdr.uncomp_len > dst_capacity)    return 0;

    const size_t num_chunks   = (hdr.uncomp_len + kCompressChunk - 1) / kCompressChunk;
    const size_t bitmap_bytes = (num_chunks + 7) / 8;
    if (src_len < sizeof(CompressHeader) + bitmap_bytes) return 0;

    const uint8_t* bitmap   = src + sizeof(CompressHeader);
    const uint8_t* data_in  = bitmap + bitmap_bytes;
    const size_t   data_cap = src_len - (sizeof(CompressHeader) + bitmap_bytes);
    size_t         data_pos = 0;

    for (size_t c = 0; c < num_chunks; ++c)
    {
        const size_t off  = c * kCompressChunk;
        const size_t span = (off + kCompressChunk <= hdr.uncomp_len)
                          ? (size_t)kCompressChunk
                          : (size_t)hdr.uncomp_len - off;
        const bool is_data = (bitmap[c >> 3] & (uint8_t)(1u << (c & 7))) != 0;
        if (is_data)
        {
            if (data_pos + span > data_cap) return 0;  // truncated
            std::memcpy(dst + off, data_in + data_pos, span);
            data_pos += span;
        }
        else
        {
            std::memset(dst + off, 0, span);
        }
    }

    return hdr.uncomp_len;
}

/* Persistent decompression scratch. Allocated lazily on first restore
 * and freed in stateShutdown. Sized to the uncompressed snapshot. */
uint8_t* g_decomp_scratch      = nullptr;
size_t   g_decomp_scratch_size = 0;

/* Compresses (state XOR reference) → dst in a single pass. Avoids the
 * intermediate XOR buffer the naive approach would need. The bitmap
 * captures which chunks have any non-zero XOR result; non-zero chunks'
 * XOR values get copied to the data area.
 *
 * Cheap because the per-byte XOR is fused into the zero-check loop:
 * we build the XOR'd word, test it for zero, and only store it if the
 * chunk has any non-zero word. */
size_t compress_xor_delta(const uint8_t* state, const uint8_t* reference,
                          size_t src_len,
                          uint8_t* dst, size_t dst_capacity)
{
    if (state == nullptr || reference == nullptr || dst == nullptr) return 0;
    if (dst_capacity < compressed_capacity(src_len)) return 0;

    const size_t num_chunks   = (src_len + kCompressChunk - 1) / kCompressChunk;
    const size_t bitmap_bytes = (num_chunks + 7) / 8;

    CompressHeader hdr;
    hdr.magic      = kCompressMagic;
    hdr.uncomp_len = (uint32_t)src_len;
    hdr.chunk_size = kCompressChunk;
    std::memcpy(dst, &hdr, sizeof(hdr));

    uint8_t* bitmap = dst + sizeof(CompressHeader);
    std::memset(bitmap, 0, bitmap_bytes);

    uint8_t* data_out = bitmap + bitmap_bytes;
    size_t   data_pos = 0;

    for (size_t c = 0; c < num_chunks; ++c)
    {
        const size_t off  = c * kCompressChunk;
        const size_t span = (off + kCompressChunk <= src_len)
                          ? kCompressChunk
                          : (src_len - off);

        /* Detect-and-emit pass: walk 8-byte words, build XOR result,
         * test for zero, write to data_out if any nonzero. */
        const uint64_t* sw = reinterpret_cast<const uint64_t*>(state + off);
        const uint64_t* rw = reinterpret_cast<const uint64_t*>(reference + off);
        uint64_t* dw = reinterpret_cast<uint64_t*>(data_out + data_pos);

        const size_t words = span / 8;
        bool any_nonzero = false;
        for (size_t i = 0; i < words; ++i)
        {
            const uint64_t v = sw[i] ^ rw[i];
            dw[i] = v;
            if (v != 0) any_nonzero = true;
        }
        const size_t tail_off = words * 8;
        for (size_t i = tail_off; i < span; ++i)
        {
            const uint8_t v = state[off + i] ^ reference[off + i];
            data_out[data_pos + i] = v;
            if (v != 0) any_nonzero = true;
        }

        if (any_nonzero)
        {
            bitmap[c >> 3] |= (uint8_t)(1u << (c & 7));
            data_pos += span;
        }
        /* If chunk was all-zero, we wrote zeros into data_out but advance
         * data_pos by 0 → those zeros get overwritten by the next nonzero
         * chunk's data. */
    }

    return sizeof(CompressHeader) + bitmap_bytes + data_pos;
}

/* Decompresses src (a compressed XOR delta) and applies it on top of
 * `reference`, writing the resulting state into `dst`. dst and reference
 * may NOT alias. */
size_t decompress_xor_apply(const uint8_t* src, size_t src_len,
                            const uint8_t* reference,
                            uint8_t* dst, size_t dst_capacity)
{
    if (src == nullptr || dst == nullptr || reference == nullptr) return 0;
    if (src_len < sizeof(CompressHeader)) return 0;

    CompressHeader hdr;
    std::memcpy(&hdr, src, sizeof(hdr));
    if (hdr.magic      != kCompressMagic) return 0;
    if (hdr.chunk_size != kCompressChunk) return 0;
    if (hdr.uncomp_len > dst_capacity)    return 0;

    const size_t num_chunks   = (hdr.uncomp_len + kCompressChunk - 1) / kCompressChunk;
    const size_t bitmap_bytes = (num_chunks + 7) / 8;
    if (src_len < sizeof(CompressHeader) + bitmap_bytes) return 0;

    const uint8_t* bitmap   = src + sizeof(CompressHeader);
    const uint8_t* data_in  = bitmap + bitmap_bytes;
    const size_t   data_cap = src_len - (sizeof(CompressHeader) + bitmap_bytes);
    size_t         data_pos = 0;

    for (size_t c = 0; c < num_chunks; ++c)
    {
        const size_t off  = c * kCompressChunk;
        const size_t span = (off + kCompressChunk <= hdr.uncomp_len)
                          ? (size_t)kCompressChunk
                          : (size_t)hdr.uncomp_len - off;
        const bool is_data = (bitmap[c >> 3] & (uint8_t)(1u << (c & 7))) != 0;

        if (is_data)
        {
            if (data_pos + span > data_cap) return 0;
            const uint64_t* dw = reinterpret_cast<const uint64_t*>(data_in + data_pos);
            const uint64_t* rw = reinterpret_cast<const uint64_t*>(reference + off);
            uint64_t* ow = reinterpret_cast<uint64_t*>(dst + off);
            const size_t words = span / 8;
            for (size_t i = 0; i < words; ++i)
                ow[i] = dw[i] ^ rw[i];
            for (size_t i = words * 8; i < span; ++i)
                dst[off + i] = data_in[data_pos + i] ^ reference[off + i];
            data_pos += span;
        }
        else
        {
            /* Zero delta chunk → output equals reference for that chunk. */
            std::memcpy(dst + off, reference + off, span);
        }
    }

    return hdr.uncomp_len;
}

static void* resolveSym(void* handle, const char* name)
{
    if (handle == nullptr)
        return nullptr;
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

} // namespace

bool stateInit()
{
    stateShutdown();

    void* handle = m64p::Core.GetHandle();
    if (handle == nullptr)
        return false;

    s_state.saveToBuffer        = (SaveToBufferFn)       resolveSym(handle, "savestates_save_to_buffer");
    s_state.saveToBufferInplace = (SaveToBufferInplaceFn)resolveSym(handle, "savestates_save_to_buffer_inplace");
    s_state.loadFromBuffer      = (LoadFromBufferFn)     resolveSym(handle, "savestates_load_from_buffer");
    s_state.getStateSize        = (GetStateSizeFn)       resolveSym(handle, "savestates_get_state_size");

    if (s_state.saveToBuffer == nullptr ||
        s_state.loadFromBuffer == nullptr ||
        s_state.getStateSize == nullptr)
    {
        s_state.saveToBuffer        = nullptr;
        s_state.saveToBufferInplace = nullptr;
        s_state.loadFromBuffer      = nullptr;
        s_state.getStateSize        = nullptr;
        return false;
    }
    /* saveToBufferInplace is optional — older mupen64plus-core builds
     * won't have the symbol. The save path falls back to the malloc'ing
     * variant when it's not available. */

    s_state.uncompressedSize = s_state.getStateSize();
    /* Report the worst-case compressed size to GekkoNet so its ring
     * buffer is always large enough even for pathological non-zero
     * snapshots. +sizeof(SlotHeader) is the type+frame slot prefix.
     * Typical SSB64 saves come in well under uncompressed size — see
     * the chunk-debug data showing 7+ MB of zero RDRAM, and delta
     * saves shrink an order of magnitude further. */
    s_state.snapshotSize = sizeof(SlotHeader)
                         + compressed_capacity(s_state.uncompressedSize);

    /* Allocate the keyframe shadow buffer. Lives for the session — one
     * persistent 16 MB allocation rather than per-save malloc. Cleared
     * to zero so an early-life delta save never reads uninitialised
     * memory if (somehow) the first save event manages to miss the
     * keyframe scheduling reset below. */
    if (s_state.uncompressedSize > 0)
    {
        s_state.keyframeShadow = (uint8_t*)std::malloc(s_state.uncompressedSize);
        if (s_state.keyframeShadow == nullptr)
        {
            s_state.uncompressedSize = 0;
            s_state.snapshotSize     = 0;
            return false;
        }
        std::memset(s_state.keyframeShadow, 0, s_state.uncompressedSize);
        s_state.keyframeShadowSize  = s_state.uncompressedSize;
        s_state.keyframeShadowFrame = -1;
    }

    s_state.ready = (s_state.uncompressedSize > 0);
    return s_state.ready;
}

void stateShutdown()
{
    for (int i = 0; i < kNumSlots; ++i)
    {
        if (s_state.slots[i].data != nullptr)
        {
            std::free(s_state.slots[i].data);
        }
        s_state.slots[i] = Slot{};
    }
    s_state.ready = false;
    s_state.saveToBuffer        = nullptr;
    s_state.saveToBufferInplace = nullptr;
    s_state.loadFromBuffer      = nullptr;
    s_state.getStateSize        = nullptr;
    s_state.snapshotSize        = 0;
    s_state.uncompressedSize    = 0;

    if (s_state.inplaceScratch != nullptr)
    {
        std::free(s_state.inplaceScratch);
        s_state.inplaceScratch     = nullptr;
        s_state.inplaceScratchSize = 0;
    }

    if (s_state.keyframeShadow != nullptr)
    {
        std::free(s_state.keyframeShadow);
        s_state.keyframeShadow      = nullptr;
        s_state.keyframeShadowSize  = 0;
        s_state.keyframeShadowFrame = -1;
    }

    /* Free the keyframe ring entries. */
    for (int i = 0; i < State::kKeyframeRingSize; ++i)
    {
        if (s_state.keyframeRing[i].data != nullptr)
        {
            std::free(s_state.keyframeRing[i].data);
        }
        s_state.keyframeRing[i] = State::KeyframeRingEntry{};
    }

    if (g_decomp_scratch != nullptr)
    {
        std::free(g_decomp_scratch);
        g_decomp_scratch      = nullptr;
        g_decomp_scratch_size = 0;
    }
}

bool stateReady()
{
    return s_state.ready;
}

size_t stateSnapshotSize()
{
    return s_state.snapshotSize;
}

bool slotIsValid(int slot)
{
    if (slot < 0 || slot >= kNumSlots)
        return false;
    return s_state.slots[slot].valid;
}

bool slotBytes(int slot, const uint8_t** out_data, size_t* out_len)
{
    if (slot < 0 || slot >= kNumSlots)
        return false;
    if (!s_state.slots[slot].valid || s_state.slots[slot].data == nullptr)
        return false;
    if (out_data) *out_data = s_state.slots[slot].data;
    if (out_len)  *out_len  = s_state.slots[slot].size;
    return true;
}

bool captureState(int slot)
{
    if (!s_state.ready || slot < 0 || slot >= kNumSlots)
        return false;

    /* Free any previous slot contents — savestates_save_to_buffer always
     * mallocs a fresh buffer, so we can't reuse the old allocation. */
    if (s_state.slots[slot].data != nullptr)
    {
        std::free(s_state.slots[slot].data);
        s_state.slots[slot] = Slot{};
    }

    uint8_t* buf = nullptr;
    size_t   len = 0;
    if (!s_state.saveToBuffer(&buf, &len) || buf == nullptr)
        return false;

    s_state.slots[slot].data = buf;
    s_state.slots[slot].size = len;
    s_state.slots[slot].valid = true;
    return true;
}

bool restoreState(int slot)
{
    if (!s_state.ready || slot < 0 || slot >= kNumSlots)
        return false;
    if (!s_state.slots[slot].valid || s_state.slots[slot].data == nullptr)
        return false;

    return s_state.loadFromBuffer(s_state.slots[slot].data,
                                  s_state.slots[slot].size) != 0;
}

/* Returns the keyframe ring entry for the given keyframe frame number,
 * or nullptr if not present. */
static State::KeyframeRingEntry* find_keyframe_ring_entry(int keyframe_frame)
{
    if (keyframe_frame < 0) return nullptr;
    const int slot = (keyframe_frame / kKeyframeInterval) % State::kKeyframeRingSize;
    State::KeyframeRingEntry* e = &s_state.keyframeRing[slot];
    return (e->frame == keyframe_frame) ? e : nullptr;
}

/* Stores a compressed keyframe payload in the ring, replacing whatever
 * was there. The ring is small and unsynchronized — only the emulation
 * thread should touch it. */
static bool stash_keyframe_in_ring(int frame, const uint8_t* payload, size_t payload_len)
{
    if (frame < 0) return false;
    const int slot = (frame / kKeyframeInterval) % State::kKeyframeRingSize;
    State::KeyframeRingEntry* e = &s_state.keyframeRing[slot];

    if (e->cap < payload_len)
    {
        std::free(e->data);
        e->data = (uint8_t*)std::malloc(payload_len);
        if (e->data == nullptr)
        {
            e->cap = 0;
            e->size = 0;
            e->frame = -1;
            return false;
        }
        e->cap = payload_len;
    }

    std::memcpy(e->data, payload, payload_len);
    e->size  = payload_len;
    e->frame = frame;
    return true;
}

/* Repopulates the shadow buffer with the keyframe at the given frame.
 * If the shadow already holds it, this is a no-op. */
static bool reload_shadow_from_ring(int keyframe_frame)
{
    if (s_state.keyframeShadowFrame == keyframe_frame) return true;

    State::KeyframeRingEntry* e = find_keyframe_ring_entry(keyframe_frame);
    if (e == nullptr || e->data == nullptr) return false;

    const size_t uncomp = decompress_snapshot(e->data, e->size,
                                              s_state.keyframeShadow,
                                              s_state.keyframeShadowSize);
    if (uncomp == 0) return false;

    s_state.keyframeShadowFrame = keyframe_frame;
    return true;
}

bool captureToBuffer(uint8_t* dst, size_t dst_len, size_t* out_len, int frame)
{
    if (!s_state.ready || dst == nullptr)
        return false;
    if (dst_len < sizeof(SlotHeader))
        return false;

    /* m64p writes the savestate into our persistent scratch buffer
     * (no per-save malloc/memset/free if the in-place export is
     * available — a ~1.4 ms win at this size). We then either
     * full-compress the scratch (keyframe) or XOR-compress it against
     * the shadow (delta) into the GekkoNet slot. */
    uint8_t* tmp     = nullptr;
    size_t   tmp_len = 0;
    bool     tmp_owned_by_m64p = false;

    if (s_state.saveToBufferInplace != nullptr)
    {
        /* Lazy-allocate the scratch buffer — same lifecycle as the
         * shadow but we keep it separate so deltas can XOR new-state
         * against shadow without a 16 MB intermediate. */
        if (s_state.inplaceScratch == nullptr ||
            s_state.inplaceScratchSize < s_state.uncompressedSize)
        {
            std::free(s_state.inplaceScratch);
            s_state.inplaceScratchSize = s_state.uncompressedSize;
            s_state.inplaceScratch     = (uint8_t*)std::malloc(s_state.inplaceScratchSize);
            if (s_state.inplaceScratch == nullptr)
            {
                s_state.inplaceScratchSize = 0;
                return false;
            }
        }
        if (!s_state.saveToBufferInplace(s_state.inplaceScratch,
                                         s_state.inplaceScratchSize,
                                         &tmp_len))
            return false;
        tmp = s_state.inplaceScratch;
    }
    else
    {
        /* Fallback path for older mupen64plus-core builds. m64p
         * mallocs a fresh buffer; we free it after compression. */
        if (!s_state.saveToBuffer(&tmp, &tmp_len) || tmp == nullptr)
            return false;
        tmp_owned_by_m64p = true;
    }

    /* Sanity — m64p must not write past the end of our buffer. The
     * actual written length (tmp_len) is permitted to be smaller than
     * uncompressedSize: m64p reports the body size while the buffer
     * is dimensioned to include 4096 bytes of trailing padding that
     * the savestate format reserves but doesn't write. m64p memset's
     * the full buffer to zero before writing, so those trailing bytes
     * are deterministic-zero and compress out via skip-zero — we hash
     * the full uncompressedSize either way for cross-peer parity. */
    if (tmp_len > s_state.uncompressedSize)
    {
        if (tmp_owned_by_m64p) std::free(tmp);
        return false;
    }

    /* Frame-derived scheduling: every kKeyframeInterval-th frame is a
     * keyframe. Deterministic so the load path can locate the right
     * reference keyframe by computing (frame - frame % N). Doesn't
     * matter what order GekkoNet calls save in (rollbacks may save the
     * same frame multiple times).
     *
     * Pre-session frame numbers (frame < 0, e.g. GekkoNet's frame=-1
     * initial-state probe) are always keyframes — they have nothing
     * to delta against, and the ring isn't indexed by them. */
    const bool is_keyframe = (frame < 0) || (frame % kKeyframeInterval) == 0;

    uint8_t* payload     = dst + sizeof(SlotHeader);
    size_t   payload_cap = dst_len - sizeof(SlotHeader);
    size_t   payload_len = 0;

    auto release_tmp = [&]() {
        if (tmp_owned_by_m64p) std::free(tmp);
    };

    /* Always compress the full uncompressedSize so both peers
     * deterministically encode the same byte count. The trailing
     * (uncompressedSize - tmp_len) bytes are zero (m64p memset before
     * writing), so they compress out via skip-zero and do not
     * contribute to the cross-peer hash. */
    const size_t encode_len = s_state.uncompressedSize;

    if (is_keyframe)
    {
        payload_len = compress_snapshot(tmp, encode_len, payload, payload_cap);
        if (payload_len == 0) { release_tmp(); return false; }

        /* Update shadow to this keyframe and stash the compressed
         * payload in the ring so we can repopulate the shadow if
         * GekkoNet later loads a delta whose reference keyframe is
         * older than what's currently in shadow. Pre-session frames
         * (frame < 0) skip the ring — they're never addressable as a
         * rollback target. */
        std::memcpy(s_state.keyframeShadow, tmp, encode_len);
        s_state.keyframeShadowFrame = frame;
        if (frame >= 0)
            stash_keyframe_in_ring(frame, payload, payload_len);

        SlotHeader hdr{ SLOT_KEYFRAME, frame };
        std::memcpy(dst, &hdr, sizeof(hdr));
    }
    else
    {
        /* Reference keyframe for this delta is the most recent
         * keyframe at or before this frame. */
        const int ref_kf_frame = frame - (frame % kKeyframeInterval);

        /* Make sure the shadow holds the reference keyframe. If we're
         * deep in a normal forward sequence, it already does. After a
         * rollback that crossed a keyframe boundary, we may need to
         * reload it from the ring. */
        if (s_state.keyframeShadowFrame != ref_kf_frame)
        {
            if (!reload_shadow_from_ring(ref_kf_frame))
            {
                /* Reference keyframe not in the ring — we can't
                 * compute a delta without it. Bail; GekkoNet will
                 * surface the failure. */
                release_tmp();
                return false;
            }
        }

        payload_len = compress_xor_delta(tmp, s_state.keyframeShadow,
                                         encode_len,
                                         payload, payload_cap);
        if (payload_len == 0) { release_tmp(); return false; }

        SlotHeader hdr{ SLOT_DELTA, frame };
        std::memcpy(dst, &hdr, sizeof(hdr));
    }

    release_tmp();

    if (out_len != nullptr)
        *out_len = sizeof(SlotHeader) + payload_len;
    return true;
}

bool restoreFromBuffer(const uint8_t* src, size_t len)
{
    if (!s_state.ready || src == nullptr || len < sizeof(SlotHeader))
        return false;

    /* Lazy-allocate (and grow) a single decompression scratch buffer
     * sized to the uncompressed snapshot. One alloc per session; freed
     * in stateShutdown. */
    if (g_decomp_scratch == nullptr || g_decomp_scratch_size < s_state.uncompressedSize)
    {
        std::free(g_decomp_scratch);
        g_decomp_scratch_size = s_state.uncompressedSize;
        g_decomp_scratch      = (uint8_t*)std::malloc(g_decomp_scratch_size);
        if (g_decomp_scratch == nullptr)
        {
            g_decomp_scratch_size = 0;
            return false;
        }
    }

    SlotHeader hdr;
    std::memcpy(&hdr, src, sizeof(hdr));

    const uint8_t* payload = src + sizeof(SlotHeader);
    const size_t   pay_len = len - sizeof(SlotHeader);

    if (hdr.type == SLOT_KEYFRAME)
    {
        const size_t uncomp = decompress_snapshot(payload, pay_len,
                                                  g_decomp_scratch,
                                                  g_decomp_scratch_size);
        if (uncomp == 0) return false;

        /* Update shadow to this keyframe and refresh the ring entry —
         * GekkoNet may not save this frame again, and we want the ring
         * to reflect the canonical state we just restored. Pre-session
         * frames don't go in the ring. */
        std::memcpy(s_state.keyframeShadow, g_decomp_scratch, uncomp);
        s_state.keyframeShadowFrame = hdr.frame;
        if (hdr.frame >= 0)
            stash_keyframe_in_ring(hdr.frame, payload, pay_len);

        return s_state.loadFromBuffer(g_decomp_scratch, uncomp) != 0;
    }

    if (hdr.type == SLOT_DELTA)
    {
        /* Locate the reference keyframe and ensure it's in the
         * shadow. If GekkoNet has rolled back to a delta whose
         * reference keyframe is older than what shadow currently
         * holds, repopulate from the ring. */
        const int ref_kf_frame = hdr.frame - (hdr.frame % kKeyframeInterval);
        if (s_state.keyframeShadowFrame != ref_kf_frame)
        {
            if (!reload_shadow_from_ring(ref_kf_frame))
                return false;
        }

        const size_t uncomp = decompress_xor_apply(payload, pay_len,
                                                   s_state.keyframeShadow,
                                                   g_decomp_scratch,
                                                   g_decomp_scratch_size);
        if (uncomp == 0) return false;

        /* Shadow stays anchored at the keyframe; only the emulator's
         * state moves to the delta-frame. */
        return s_state.loadFromBuffer(g_decomp_scratch, uncomp) != 0;
    }

    return false;
}

int stateKeyframeInterval()
{
    return kKeyframeInterval;
}

} // namespace FrameZero
