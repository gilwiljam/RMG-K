#define CORE_INTERNAL

#include "FrameZeroConnect.hpp"

#include "Callback.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET fz_socket_t;
#define FZ_INVALID_SOCKET INVALID_SOCKET
#define FZ_CLOSE_SOCKET(s) closesocket(s)
#define FZ_LAST_ERROR     WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
typedef int fz_socket_t;
#define FZ_INVALID_SOCKET (-1)
#define FZ_CLOSE_SOCKET(s) close(s)
#define FZ_LAST_ERROR     errno
#endif

namespace
{

constexpr const char* kHelloPrefix = "FZ_HELLO ";
constexpr const char* kAck         = "FZ_ACK\n";
constexpr size_t      kMaxIdentity = 64;

std::atomic<CoreFrameZero::ConnectStatus> g_status{CoreFrameZero::ConnectStatus::Idle};
std::atomic<bool>                          g_should_stop{false};
std::thread                                g_worker;
std::mutex                                 g_remote_identity_mu;
std::string                                g_remote_identity;

/* RTT samples collected during the handshake. Updated whenever an
 * ACK arrives — RTT estimated as (now - last_hello_send). The
 * approximation error is bounded by the HELLO interval (100 ms) since
 * the peer's ACK might correspond to an earlier HELLO than the most
 * recent one we sent. Across multiple samples and given the median
 * read, the bias washes out for the "what input delay should we
 * pick?" decision. Samples are bounded — we only keep the last
 * kRttSampleMax to keep median compute cheap. */
constexpr size_t kRttSampleMax = 16;
std::mutex                                 g_rtt_mu;
std::vector<int>                           g_rtt_samples_ms;

struct ParsedAddr
{
    sockaddr_in sa;
    std::string canonical; /* "ip:port" — used as map key */
};

static bool parse_addr(const std::string& s, ParsedAddr& out)
{
    /* Split on last ':' to support "host:port". IPv6 not supported here
     * — Phase 4 minimum is IPv4 LAN/loopback. */
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = s.substr(0, colon);
    std::string port = s.substr(colon + 1);
    if (host.empty() || port.empty()) return false;

    int port_i = 0;
    try { port_i = std::stoi(port); } catch (...) { return false; }
    if (port_i <= 0 || port_i > 65535) return false;

    std::memset(&out.sa, 0, sizeof(out.sa));
    out.sa.sin_family = AF_INET;
    out.sa.sin_port   = htons((unsigned short)port_i);

    /* inet_pton: numeric IPv4 only. Hostname resolution is out of
     * scope for the env-var harness — direct LAN/loopback only. */
    if (inet_pton(AF_INET, host.c_str(), &out.sa.sin_addr) != 1)
    {
        return false;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s:%d", host.c_str(), port_i);
    out.canonical = buf;
    return true;
}

static std::string addr_to_string(const sockaddr_in& sa)
{
    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, ipbuf, sizeof(ipbuf));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s:%d", ipbuf, ntohs(sa.sin_port));
    return buf;
}

#ifdef _WIN32
static bool g_winsock_initialised = false;
static bool ensure_winsock()
{
    if (g_winsock_initialised) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_winsock_initialised = true;
    return true;
}
#else
static bool ensure_winsock() { return true; }
#endif

static void set_nonblocking(fz_socket_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void worker(unsigned short local_port,
                   std::vector<ParsedAddr> peers,
                   int timeout_seconds,
                   std::string local_identity)
{
    using clock = std::chrono::steady_clock;

    if (!ensure_winsock())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero connect] WSAStartup failed");
        g_status.store(CoreFrameZero::ConnectStatus::Error);
        return;
    }

    fz_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == FZ_INVALID_SOCKET)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero connect] socket() failed");
        g_status.store(CoreFrameZero::ConnectStatus::Error);
        return;
    }

    sockaddr_in bind_sa = {};
    bind_sa.sin_family      = AF_INET;
    bind_sa.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_sa.sin_port        = htons(local_port);
    if (bind(sock, (sockaddr*)&bind_sa, sizeof(bind_sa)) != 0)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "[FrameZero connect] bind() failed on port %u (err=%d)",
            (unsigned)local_port, FZ_LAST_ERROR);
        CoreAddCallbackMessage(CoreDebugMessageType::Error, buf);
        FZ_CLOSE_SOCKET(sock);
        g_status.store(CoreFrameZero::ConnectStatus::Error);
        return;
    }
    set_nonblocking(sock);

    char log[256];
    std::snprintf(log, sizeof(log),
        "[FrameZero connect] listening on UDP %u, expecting %zu peer(s), identity='%s'",
        (unsigned)local_port, peers.size(),
        local_identity.empty() ? "(none)" : local_identity.c_str());
    CoreAddCallbackMessage(CoreDebugMessageType::Info, log);

    /* Pre-format the HELLO once. */
    std::string hello_msg;
    hello_msg.reserve(std::strlen(kHelloPrefix) + local_identity.size() + 1);
    hello_msg.append(kHelloPrefix);
    hello_msg.append(local_identity);
    hello_msg.push_back('\n');

    /* Track per-peer handshake progress. Peers identified by their
     * canonical "ip:port" string. */
    std::set<std::string> hello_received;
    std::set<std::string> ack_received;
    const size_t expected = peers.size();
    if (expected == 0)
    {
        FZ_CLOSE_SOCKET(sock);
        g_status.store(CoreFrameZero::ConnectStatus::Error);
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero connect] no peer addresses configured");
        return;
    }

    const auto deadline = clock::now() + std::chrono::seconds(timeout_seconds);
    auto next_send = clock::now();

    /* Reset RTT samples — leftover values from a prior session would
     * skew the auto-delay decision for this one. */
    {
        std::lock_guard<std::mutex> lk(g_rtt_mu);
        g_rtt_samples_ms.clear();
    }

    auto last_hello_send = clock::now();

    while (clock::now() < deadline && !g_should_stop.load())
    {
        const auto now = clock::now();

        /* Send HELLO (and ACK if we owe any) every 100 ms. */
        if (now >= next_send)
        {
            for (const auto& p : peers)
            {
                sendto(sock, hello_msg.c_str(), (int)hello_msg.size(), 0,
                       (const sockaddr*)&p.sa, sizeof(p.sa));
                /* Keep ACKing peers we've heard from until they've
                 * acked us back — repetition makes the handshake
                 * robust to single packet loss. */
                if (hello_received.count(p.canonical))
                {
                    sendto(sock, kAck, (int)std::strlen(kAck), 0,
                           (const sockaddr*)&p.sa, sizeof(p.sa));
                }
            }
            last_hello_send = now;
            next_send = now + std::chrono::milliseconds(100);
        }

        /* Drain any pending packets. */
        char buf[256];
        sockaddr_in from = {};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        int r = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (sockaddr*)&from, &from_len);
        if (r > 0)
        {
            buf[r] = '\0';
            std::string from_canon = addr_to_string(from);

            /* Match against configured peers — ignore unknown senders. */
            bool known = false;
            for (const auto& p : peers)
            {
                if (p.canonical == from_canon) { known = true; break; }
            }
            if (known)
            {
                const size_t prefix_len = std::strlen(kHelloPrefix);
                if (std::strncmp(buf, kHelloPrefix, prefix_len) == 0)
                {
                    /* Extract identity: everything between prefix and \n
                     * (or end of buffer). Cap at kMaxIdentity so a
                     * malformed packet can't blow our string out. */
                    const char* id_start = buf + prefix_len;
                    const char* id_end = id_start;
                    while (*id_end != '\0' && *id_end != '\n' &&
                           (size_t)(id_end - id_start) < kMaxIdentity)
                    {
                        ++id_end;
                    }
                    std::string remote_id(id_start, id_end - id_start);

                    {
                        std::lock_guard<std::mutex> lk(g_remote_identity_mu);
                        if (g_remote_identity.empty())
                            g_remote_identity = remote_id;
                    }

                    if (remote_id != local_identity)
                    {
                        char err[320];
                        std::snprintf(err, sizeof(err),
                            "[FrameZero connect] ROM mismatch — local='%s' remote='%s' from %s. Both peers must load the same ROM.",
                            local_identity.empty() ? "(none)" : local_identity.c_str(),
                            remote_id.empty()      ? "(none)" : remote_id.c_str(),
                            from_canon.c_str());
                        CoreAddCallbackMessage(CoreDebugMessageType::Error, err);
                        FZ_CLOSE_SOCKET(sock);
                        g_status.store(CoreFrameZero::ConnectStatus::RomMismatch);
                        return;
                    }
                    /* Acknowledge immediately. The original code only
                     * sent ACKs in the 100 ms HELLO loop tick, which
                     * meant the peer's measured RTT was dominated by
                     * the up-to-100 ms ACK-batching delay rather than
                     * the actual network round-trip. Retrying in the
                     * 100 ms loop is still fine for redundancy under
                     * packet loss. */
                    sendto(sock, kAck, (int)std::strlen(kAck), 0,
                           (const sockaddr*)&from, from_len);
                    hello_received.insert(from_canon);
                }
                else if (std::strncmp(buf, kAck, std::strlen(kAck)) == 0)
                {
                    /* RTT estimate. Bias bounded by HELLO interval
                     * (100 ms): peer might be ACKing the HELLO before
                     * the most recent one, but the median across
                     * samples washes that out enough for the input-
                     * delay decision. */
                    const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock::now() - last_hello_send).count();
                    if (rtt >= 0 && rtt < 5000)
                    {
                        std::lock_guard<std::mutex> lk(g_rtt_mu);
                        g_rtt_samples_ms.push_back((int)rtt);
                        if (g_rtt_samples_ms.size() > kRttSampleMax)
                        {
                            g_rtt_samples_ms.erase(g_rtt_samples_ms.begin());
                        }
                    }
                    ack_received.insert(from_canon);
                }
            }
        }
        else
        {
            /* No data — sleep briefly to avoid busy-spin. */
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (hello_received.size() >= expected && ack_received.size() >= expected)
        {
            /* Send a few extra ACKs to give the peer a fighting chance
             * of receiving its final ACK before we close. UDP is
             * lossy; any ack the peer misses just causes them to
             * resend HELLO and we'll have closed the socket. Their
             * worst case is they timeout. We minimise that with a
             * short tail of redundant sends. */
            for (int i = 0; i < 5; ++i)
            {
                for (const auto& p : peers)
                {
                    sendto(sock, kAck, (int)std::strlen(kAck), 0,
                           (const sockaddr*)&p.sa, sizeof(p.sa));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            FZ_CLOSE_SOCKET(sock);
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "[FrameZero connect] handshake complete — peers ready.");
            g_status.store(CoreFrameZero::ConnectStatus::Connected);
            return;
        }
    }

    FZ_CLOSE_SOCKET(sock);
    if (g_should_stop.load())
    {
        g_status.store(CoreFrameZero::ConnectStatus::Idle);
    }
    else
    {
        std::snprintf(log, sizeof(log),
            "[FrameZero connect] timeout — got %zu/%zu HELLOs, %zu/%zu ACKs",
            hello_received.size(), expected, ack_received.size(), expected);
        CoreAddCallbackMessage(CoreDebugMessageType::Warning, log);
        g_status.store(CoreFrameZero::ConnectStatus::TimedOut);
    }
}

} // namespace

bool CoreFrameZeroConnectStart(unsigned short local_port,
                                const std::vector<std::string>& peer_addrs,
                                int timeout_seconds,
                                const std::string& local_identity)
{
    if (g_status.load() == CoreFrameZero::ConnectStatus::Connecting)
    {
        return false;
    }
    if (g_worker.joinable())
    {
        g_should_stop.store(true);
        g_worker.join();
    }

    std::vector<ParsedAddr> parsed;
    parsed.reserve(peer_addrs.size());
    for (const auto& s : peer_addrs)
    {
        if (s.empty() || s == "0") continue; /* placeholder for local slot */
        ParsedAddr a;
        if (parse_addr(s, a)) parsed.push_back(a);
    }

    if (parsed.empty())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero connect] no valid peer addresses parsed");
        g_status.store(CoreFrameZero::ConnectStatus::Error);
        return false;
    }

    /* Cap identity length defensively — protocol caps remote-side
     * parsing at kMaxIdentity, so anything longer locally would just
     * trigger spurious mismatches. */
    std::string id = local_identity;
    if (id.size() > kMaxIdentity) id.resize(kMaxIdentity);
    /* Strip newlines/spaces — those are framing. */
    for (char& c : id) { if (c == '\n' || c == '\r' || c == ' ') c = '_'; }

    {
        std::lock_guard<std::mutex> lk(g_remote_identity_mu);
        g_remote_identity.clear();
    }

    g_should_stop.store(false);
    g_status.store(CoreFrameZero::ConnectStatus::Connecting);
    g_worker = std::thread(worker, local_port, std::move(parsed),
                           timeout_seconds, std::move(id));
    return true;
}

std::string CoreFrameZeroConnectGetRemoteIdentity(void)
{
    std::lock_guard<std::mutex> lk(g_remote_identity_mu);
    return g_remote_identity;
}

CoreFrameZero::ConnectStatus CoreFrameZeroConnectGetStatus(void)
{
    return g_status.load();
}

void CoreFrameZeroConnectStop(void)
{
    if (g_worker.joinable())
    {
        g_should_stop.store(true);
        g_worker.join();
    }
    if (g_status.load() == CoreFrameZero::ConnectStatus::Connecting)
    {
        g_status.store(CoreFrameZero::ConnectStatus::Idle);
    }
}

int CoreFrameZeroConnectGetMedianRttMs(void)
{
    std::vector<int> samples;
    {
        std::lock_guard<std::mutex> lk(g_rtt_mu);
        samples = g_rtt_samples_ms;
    }
    if (samples.empty()) return -1;

    /* Median across the collected samples — robust against the
     * single-HELLO-interval bias that can spike individual readings. */
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
