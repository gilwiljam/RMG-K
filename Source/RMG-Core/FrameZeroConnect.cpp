#define CORE_INTERNAL

#include "FrameZeroConnect.hpp"

#include "Callback.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr const char* kHello = "FZ_HELLO\n";
constexpr const char* kAck   = "FZ_ACK\n";

std::atomic<CoreFrameZero::ConnectStatus> g_status{CoreFrameZero::ConnectStatus::Idle};
std::atomic<bool>                          g_should_stop{false};
std::thread                                g_worker;

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
                   int timeout_seconds)
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

    char log[192];
    std::snprintf(log, sizeof(log),
        "[FrameZero connect] listening on UDP %u, expecting %zu peer(s)",
        (unsigned)local_port, peers.size());
    CoreAddCallbackMessage(CoreDebugMessageType::Info, log);

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

    while (clock::now() < deadline && !g_should_stop.load())
    {
        const auto now = clock::now();

        /* Send HELLO (and ACK if we owe any) every 100 ms. */
        if (now >= next_send)
        {
            for (const auto& p : peers)
            {
                sendto(sock, kHello, (int)std::strlen(kHello), 0,
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
                if (std::strncmp(buf, kHello, std::strlen(kHello)) == 0)
                {
                    hello_received.insert(from_canon);
                }
                else if (std::strncmp(buf, kAck, std::strlen(kAck)) == 0)
                {
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
                                int timeout_seconds)
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

    g_should_stop.store(false);
    g_status.store(CoreFrameZero::ConnectStatus::Connecting);
    g_worker = std::thread(worker, local_port, std::move(parsed), timeout_seconds);
    return true;
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
