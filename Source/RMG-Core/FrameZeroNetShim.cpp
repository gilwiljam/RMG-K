#define CORE_INTERNAL

#include "FrameZeroNetShim.hpp"

#include "Callback.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <vector>

#ifdef FRAME_ZERO

namespace
{

struct PendingPacket
{
    std::vector<uint8_t>                            payload;
    std::vector<uint8_t>                            addr_data;  // owned copy of addr->data
    GekkoNetAddress                                 addr;       // points into addr_data
    std::chrono::steady_clock::time_point           release_at;
};

GekkoNetAdapter*                              g_underlying = nullptr;
GekkoNetAdapter                               g_shim       = {};
bool                                          g_active     = false;

int                                           g_latency_ms = 0;
int                                           g_jitter_ms  = 0;
float                                         g_loss_frac  = 0.0f;
unsigned long long                            g_dropped    = 0;
unsigned long long                            g_delayed    = 0;

std::mutex                                    g_outbound_mu;
std::deque<PendingPacket>                     g_outbound;

std::mt19937&                                 thread_rng()
{
    /* One generator per thread so we don't lock on the rng path. */
    static thread_local std::mt19937 rng{
        (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()
    };
    return rng;
}

bool roll_drop()
{
    if (g_loss_frac <= 0.0f) return false;
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    return d(thread_rng()) < g_loss_frac;
}

int roll_jitter()
{
    if (g_jitter_ms <= 0) return 0;
    std::uniform_int_distribution<int> d(0, g_jitter_ms);
    return d(thread_rng());
}

void flush_matured(std::chrono::steady_clock::time_point now)
{
    if (g_underlying == nullptr) return;
    std::deque<PendingPacket> ready;
    {
        std::lock_guard<std::mutex> lk(g_outbound_mu);
        while (!g_outbound.empty() && g_outbound.front().release_at <= now)
        {
            ready.emplace_back(std::move(g_outbound.front()));
            g_outbound.pop_front();
        }
    }
    for (auto& p : ready)
    {
        g_underlying->send_data(&p.addr, (const char*)p.payload.data(),
                                (int)p.payload.size());
    }
}

void shim_send_data(GekkoNetAddress* addr, const char* data, int length)
{
    if (g_underlying == nullptr) return;

    if (roll_drop())
    {
        ++g_dropped;
        return;
    }

    const int delay_ms = g_latency_ms + roll_jitter();
    if (delay_ms <= 0)
    {
        g_underlying->send_data(addr, data, length);
        return;
    }

    PendingPacket p;
    p.payload.assign((const uint8_t*)data, (const uint8_t*)data + length);
    if (addr != nullptr && addr->data != nullptr && addr->size > 0)
    {
        p.addr_data.assign((const uint8_t*)addr->data,
                           (const uint8_t*)addr->data + addr->size);
        p.addr.data = p.addr_data.data();
        p.addr.size = addr->size;
    }
    else
    {
        p.addr.data = nullptr;
        p.addr.size = 0;
    }
    p.release_at = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(delay_ms);

    {
        std::lock_guard<std::mutex> lk(g_outbound_mu);
        g_outbound.emplace_back(std::move(p));
    }
    ++g_delayed;
}

GekkoNetResult** shim_receive_data(int* length)
{
    /* Piggy-back on receive cadence to ship matured outbound. */
    flush_matured(std::chrono::steady_clock::now());
    if (g_underlying == nullptr || g_underlying->receive_data == nullptr)
    {
        if (length != nullptr) *length = 0;
        return nullptr;
    }
    return g_underlying->receive_data(length);
}

void shim_free_data(void* data_ptr)
{
    if (g_underlying != nullptr && g_underlying->free_data != nullptr)
        g_underlying->free_data(data_ptr);
}

int env_int(const char* name, int dflt, int min_v, int max_v)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') return dflt;
    int parsed = std::atoi(v);
    if (parsed < min_v) return min_v;
    if (parsed > max_v) return max_v;
    return parsed;
}

float env_float(const char* name, float dflt, float min_v, float max_v)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') return dflt;
    float parsed = (float)std::atof(v);
    if (parsed < min_v) return min_v;
    if (parsed > max_v) return max_v;
    return parsed;
}

} // namespace

namespace FrameZeroNetShim
{

GekkoNetAdapter* Wrap(GekkoNetAdapter* underlying)
{
    if (underlying == nullptr) return nullptr;

    /* Re-read env vars on every Wrap call so a session restart picks
     * up changes without an app restart. */
    g_latency_ms = env_int("FRAME_ZERO_NETSIM_LATENCY_MS", 0, 0, 5000);
    g_jitter_ms  = env_int("FRAME_ZERO_NETSIM_JITTER_MS",  0, 0, 5000);
    const float loss_pct = env_float("FRAME_ZERO_NETSIM_LOSS_PCT", 0.0f, 0.0f, 100.0f);
    g_loss_frac = loss_pct / 100.0f;

    if (g_latency_ms == 0 && g_jitter_ms == 0 && g_loss_frac == 0.0f)
    {
        /* Nothing to simulate — return underlying unchanged so the
         * production code path is zero-overhead. */
        Reset();
        return underlying;
    }

    /* Drop any pending packets from a previous session. */
    {
        std::lock_guard<std::mutex> lk(g_outbound_mu);
        g_outbound.clear();
    }
    g_dropped = 0;
    g_delayed = 0;
    g_underlying = underlying;
    g_shim.send_data    = &shim_send_data;
    g_shim.receive_data = &shim_receive_data;
    g_shim.free_data    = &shim_free_data;
    g_active = true;

    char msg[200];
    std::snprintf(msg, sizeof(msg),
        "[FrameZero netsim] active: latency=%d ms jitter=±%d ms loss=%.2f%%",
        g_latency_ms, g_jitter_ms, loss_pct);
    CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(msg));

    return &g_shim;
}

void Reset()
{
    if (!g_active && g_outbound.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_outbound_mu);
        g_outbound.clear();
    }
    if (g_active)
    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "[FrameZero netsim] reset: delayed=%llu dropped=%llu",
            g_delayed, g_dropped);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(msg));
    }
    g_underlying = nullptr;
    g_active     = false;
    g_dropped    = 0;
    g_delayed    = 0;
}

} // namespace FrameZeroNetShim

#endif // FRAME_ZERO
