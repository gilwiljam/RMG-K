#define CORE_INTERNAL

#include "FrameZeroIPC.hpp"

#include "Callback.hpp"
#include "m64p/Api.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

typedef uint32_t (*read_rdram_word_t)(uint32_t address);
typedef void     (*write_rdram_word_t)(uint32_t address, uint32_t value);

read_rdram_word_t   g_read_rdram  = nullptr;
write_rdram_word_t  g_write_rdram = nullptr;
bool                g_resolved    = false;

std::atomic<CoreFrameZero::IPCHandler> g_handler{nullptr};

void* fz_ipc_resolve_sym(void* handle, const char* name)
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

bool CoreFrameZeroIPCInit(void)
{
    if (g_resolved)
        return true;

    void* handle = m64p::Core.GetHandle();
    if (handle == nullptr)
        return false;

    g_read_rdram  = (read_rdram_word_t)
        fz_ipc_resolve_sym(handle, "core_read_rdram_word");
    g_write_rdram = (write_rdram_word_t)
        fz_ipc_resolve_sym(handle, "core_write_rdram_word");

    if (g_read_rdram == nullptr || g_write_rdram == nullptr)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "[FrameZero IPC] mupen64plus-core does not export core_read_rdram_word / core_write_rdram_word — rebuild core.");
        return false;
    }

    g_resolved = true;
    return true;
}

void CoreFrameZeroIPCSetHandler(CoreFrameZero::IPCHandler handler)
{
    g_handler.store(handler, std::memory_order_release);
}

void CoreFrameZeroIPCPoll(void)
{
    if (!g_resolved || g_read_rdram == nullptr)
        return;

    const uint32_t word = g_read_rdram(CoreFrameZero::kIPCCommandAddr);
    if (word == 0)
        return;

    /* Lower 24 bits are the command, upper 8 bits are an opaque cookie
     * the game can use to disambiguate (e.g. ABI version, sequence
     * number). For this initial wedge we just pass both through. */
    const uint32_t cmd    = word & 0x00FFFFFF;
    const uint32_t cookie = (word >> 24) & 0xFF;

    /* Clear the word as an ack BEFORE dispatching the handler. If the
     * handler ends up triggering further game-state changes that
     * include the game waiting on the word being zero, we don't want
     * to deadlock by leaving it set. */
    g_write_rdram(CoreFrameZero::kIPCCommandAddr, 0);

    auto handler = g_handler.load(std::memory_order_acquire);
    if (handler != nullptr)
    {
        handler(cmd, cookie);
    }
    else
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "[FrameZero IPC] cmd=0x%06X cookie=0x%02X (no handler registered)",
            (unsigned)cmd, (unsigned)cookie);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
    }
}

uint32_t CoreFrameZeroIPCReadWord(uint32_t address)
{
    if (!g_resolved || g_read_rdram == nullptr) return 0;
    return g_read_rdram(address);
}

void CoreFrameZeroIPCWriteWord(uint32_t address, uint32_t value)
{
    if (!g_resolved || g_write_rdram == nullptr) return;
    g_write_rdram(address, value);
}
