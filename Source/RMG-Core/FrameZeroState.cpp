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

using SaveToBufferFn   = int (*)(uint8_t**, size_t*);
using LoadFromBufferFn = int (*)(const uint8_t*, size_t);
using GetStateSizeFn   = size_t (*)(void);

struct Slot
{
    uint8_t* data = nullptr;
    size_t   size = 0;
    bool     valid = false;
};

struct State
{
    bool ready = false;
    SaveToBufferFn   saveToBuffer   = nullptr;
    LoadFromBufferFn loadFromBuffer = nullptr;
    GetStateSizeFn   getStateSize   = nullptr;
    size_t snapshotSize = 0;
    Slot slots[kNumSlots];
};

static State s_state;

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

    s_state.saveToBuffer   = (SaveToBufferFn)  resolveSym(handle, "savestates_save_to_buffer");
    s_state.loadFromBuffer = (LoadFromBufferFn)resolveSym(handle, "savestates_load_from_buffer");
    s_state.getStateSize   = (GetStateSizeFn)  resolveSym(handle, "savestates_get_state_size");

    if (s_state.saveToBuffer == nullptr ||
        s_state.loadFromBuffer == nullptr ||
        s_state.getStateSize == nullptr)
    {
        s_state.saveToBuffer = nullptr;
        s_state.loadFromBuffer = nullptr;
        s_state.getStateSize = nullptr;
        return false;
    }

    s_state.snapshotSize = s_state.getStateSize();
    s_state.ready = (s_state.snapshotSize > 0);
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
    s_state.saveToBuffer = nullptr;
    s_state.loadFromBuffer = nullptr;
    s_state.getStateSize = nullptr;
    s_state.snapshotSize = 0;
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

bool captureToBuffer(uint8_t* dst, size_t dst_len, size_t* out_len)
{
    if (!s_state.ready || dst == nullptr)
        return false;

    /* savestates_save_to_buffer always allocates a fresh buffer. We copy
     * into the caller's buffer and free. One extra ~16 MB memcpy per
     * SaveEvent — acceptable for Phase 3; revisit if it shows up in
     * profiling. */
    uint8_t* tmp = nullptr;
    size_t   tmp_len = 0;
    if (!s_state.saveToBuffer(&tmp, &tmp_len) || tmp == nullptr)
        return false;

    if (tmp_len > dst_len)
    {
        std::free(tmp);
        return false;
    }

    std::memcpy(dst, tmp, tmp_len);
    std::free(tmp);

    if (out_len != nullptr)
        *out_len = tmp_len;
    return true;
}

bool restoreFromBuffer(const uint8_t* src, size_t len)
{
    if (!s_state.ready || src == nullptr || len == 0)
        return false;
    return s_state.loadFromBuffer(src, len) != 0;
}

} // namespace FrameZero
