#define CORE_INTERNAL

#include "N02Traversal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET n02_socket_t;
#define N02_INVALID_SOCKET INVALID_SOCKET
#define N02_CLOSE_SOCKET(s) closesocket(s)
#define N02_LAST_ERROR     WSAGetLastError()
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int n02_socket_t;
#define N02_INVALID_SOCKET (-1)
#define N02_CLOSE_SOCKET(s) close(s)
#define N02_LAST_ERROR     errno
#endif

namespace
{

#ifdef _WIN32
std::once_flag g_winsock_once;
bool           g_winsock_ok = false;

bool ensure_winsock()
{
    std::call_once(g_winsock_once, []() {
        WSADATA wsa;
        g_winsock_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    });
    return g_winsock_ok;
}
#else
bool ensure_winsock() { return true; }
#endif

// Trim ASCII whitespace, strip ' ' chars, upper-case the rest.
std::string upper_trim_strip_spaces(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    size_t start = 0;
    size_t end   = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    for (size_t i = start; i < end; ++i)
    {
        char c = s[i];
        if (c == ' ') continue;
        out.push_back((char)std::toupper((unsigned char)c));
    }
    return out;
}

bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool is_digit(char c)  { return c >= '0' && c <= '9'; }

std::string strip_leading_zeros(const std::string& digits)
{
    size_t first_nonzero = 0;
    while (first_nonzero < digits.size() && digits[first_nonzero] == '0')
    {
        ++first_nonzero;
    }
    if (first_nonzero >= digits.size())
    {
        return "0";
    }
    return digits.substr(first_nonzero);
}

// Resolve hostname/IP to a sockaddr_in. Tries inet_pton first, falls
// back to getaddrinfo for hostnames.
bool resolve_server(const char* host, int port, sockaddr_in& out)
{
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &out.sin_addr) == 1)
    {
        return true;
    }

    addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return false;
    }

    bool ok = false;
    for (addrinfo* it = res; it != nullptr; it = it->ai_next)
    {
        if (it->ai_family == AF_INET && it->ai_addr != nullptr)
        {
            const auto* sa = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            out.sin_addr   = sa->sin_addr;
            ok = true;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

// '|'-split a string into UTF-8 fields. Empty trailing field
// preserved (matching QByteArray::split).
std::vector<std::string> split_pipe(const char* data, size_t len)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (data[i] == '|')
        {
            out.emplace_back(data + start, i - start);
            start = i + 1;
        }
    }
    out.emplace_back(data + start, len - start);
    return out;
}

} // anonymous namespace

bool CoreN02LooksLikeCode(const std::string& s)
{
    const std::string text = upper_trim_strip_spaces(s);
    if (text.empty()) return false;

    for (char ch : text)
    {
        if (ch == '.' || ch == ':' || ch == '/') return false;
    }
    if (text.size() < 4) return false;

    int prefix_len = 0;
    while (prefix_len < (int)text.size() && prefix_len < 4 && is_letter(text[prefix_len]))
    {
        ++prefix_len;
    }
    if (prefix_len < 3) return false;
    if (prefix_len < (int)text.size() && is_letter(text[prefix_len])) return false;

    std::string digits = text.substr(prefix_len);
    if (!digits.empty() && (digits[0] == '@' || digits[0] == '#' ||
                            digits[0] == '-' || digits[0] == '_'))
    {
        digits.erase(0, 1);
    }
    if (digits.empty()) return false;
    if ((int)digits.size() > CoreN02Traversal::kMaxDigits) return false;
    for (char ch : digits)
    {
        if (!is_digit(ch)) return false;
    }
    return true;
}

std::string CoreN02NormalizeCode(const std::string& s)
{
    const std::string text = upper_trim_strip_spaces(s);
    if (!CoreN02LooksLikeCode(text)) return std::string();

    int prefix_len = 0;
    while (prefix_len < (int)text.size() && prefix_len < 4 && is_letter(text[prefix_len]))
    {
        ++prefix_len;
    }

    std::string digits = text.substr(prefix_len);
    if (!digits.empty() && (digits[0] == '@' || digits[0] == '#' ||
                            digits[0] == '-' || digits[0] == '_'))
    {
        digits.erase(0, 1);
    }
    if ((int)digits.size() > CoreN02Traversal::kMaxDigits) return std::string();
    digits = strip_leading_zeros(digits);

    return text.substr(0, prefix_len) + "@" + digits;
}

std::string CoreN02NormalizeClaimTarget(const std::string& s)
{
    const std::string text = upper_trim_strip_spaces(s);
    if (text.empty()) return std::string();
    if (text == "AUTO") return text;

    for (char ch : text)
    {
        if (ch == '.' || ch == ':' || ch == '/') return std::string();
    }

    int prefix_len = 0;
    while (prefix_len < (int)text.size() && prefix_len < 4 && is_letter(text[prefix_len]))
    {
        ++prefix_len;
    }
    if (prefix_len < 3) return std::string();
    if (prefix_len < (int)text.size() && is_letter(text[prefix_len])) return std::string();

    const std::string prefix = text.substr(0, prefix_len);
    std::string suffix = text.substr(prefix_len);
    if (suffix.empty()) return prefix;
    if (suffix[0] == '@' || suffix[0] == '#' || suffix[0] == '-' || suffix[0] == '_')
    {
        suffix.erase(0, 1);
    }
    if (suffix.empty()) return prefix;
    if ((int)suffix.size() > CoreN02Traversal::kMaxDigits) return std::string();

    for (char ch : suffix)
    {
        if (!is_digit(ch)) return std::string();
    }
    suffix = strip_leading_zeros(suffix);

    return prefix + "@" + suffix;
}

CoreN02Traversal::Result CoreN02SendRequest(const std::string& payload, int timeout_ms)
{
    using namespace CoreN02Traversal;
    Result r;

    if (!ensure_winsock())
    {
        r.status = Status::SocketError;
        r.error  = "Failed to initialise networking.";
        return r;
    }

    sockaddr_in server = {};
    if (!resolve_server(kHost, kPort, server))
    {
        r.status = Status::ResolveError;
        r.error  = "Failed to resolve the NAT server address.";
        return r;
    }

    n02_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == N02_INVALID_SOCKET)
    {
        r.status = Status::SocketError;
        r.error  = "Failed to open a UDP socket for code configuration.";
        return r;
    }

    sockaddr_in bind_sa = {};
    bind_sa.sin_family      = AF_INET;
    bind_sa.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_sa.sin_port        = 0;
    if (bind(sock, (sockaddr*)&bind_sa, sizeof(bind_sa)) != 0)
    {
        N02_CLOSE_SOCKET(sock);
        r.status = Status::SocketError;
        r.error  = "Failed to open a UDP socket for code configuration.";
        return r;
    }

    int sent = sendto(sock, payload.data(), (int)payload.size(), 0,
                      (const sockaddr*)&server, sizeof(server));
    if (sent < 0)
    {
        N02_CLOSE_SOCKET(sock);
        r.status = Status::SendError;
        r.error  = "Failed to send the code request to the NAT server.";
        return r;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    int sel = select(0, &rfds, nullptr, nullptr, &tv);
#else
    int sel = select((int)sock + 1, &rfds, nullptr, nullptr, &tv);
#endif

    if (sel <= 0)
    {
        N02_CLOSE_SOCKET(sock);
        r.status = Status::Timeout;
        r.error  = "Timed out waiting for the NAT server.";
        return r;
    }

    char buf[2048];
    sockaddr_in from = {};
#ifdef _WIN32
    int from_len = (int)sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    int got = recvfrom(sock, buf, (int)sizeof(buf), 0,
                       (sockaddr*)&from, &from_len);
    N02_CLOSE_SOCKET(sock);

    if (got <= 0)
    {
        r.status = Status::Timeout;
        r.error  = "Timed out waiting for the NAT server.";
        return r;
    }

    // Some traversal server builds prepend a NUL byte to responses.
    const char* data = buf;
    size_t      len  = (size_t)got;
    if (len > 0 && data[0] == '\0')
    {
        data += 1;
        len  -= 1;
    }

    r.parts = split_pipe(data, len);
    if (r.parts.size() < 2 || r.parts[0] != CoreN02Traversal::kProtocol)
    {
        r.status = Status::InvalidResponse;
        r.error  = "Received an invalid response from the NAT server.";
        return r;
    }

    r.status = Status::Ok;
    return r;
}

CoreN02Traversal::Result CoreN02Check(const std::string& target,
                                      const std::string& owner_token)
{
    std::string payload = std::string(CoreN02Traversal::kProtocol) + "|CHECK|" + target;
    if (!owner_token.empty())
    {
        payload += "|";
        payload += owner_token;
    }
    return CoreN02SendRequest(payload);
}

CoreN02Traversal::Result CoreN02Claim(const std::string& target,
                                      const std::string& owner_token)
{
    std::string payload = std::string(CoreN02Traversal::kProtocol) + "|CLAIM|" + target;
    if (!owner_token.empty())
    {
        payload += "|";
        payload += owner_token;
    }
    return CoreN02SendRequest(payload);
}

CoreN02Traversal::Result CoreN02ClaimAuto(void)
{
    return CoreN02Claim("AUTO");
}

bool CoreN02ConfirmClaim(const std::string& code,
                         const std::string& owner_token,
                         std::string& out_error)
{
    if (code.empty() || owner_token.empty())
    {
        out_error = "Missing claim confirmation data.";
        return false;
    }

    std::string ack_payload = std::string(CoreN02Traversal::kProtocol) +
                              "|CLAIMACK|" + owner_token;
    auto ack = CoreN02SendRequest(ack_payload);
    if (ack.status == CoreN02Traversal::Status::Ok)
    {
        if (ack.parts.size() >= 2 && ack.parts[1] == "OK")
        {
            return true;
        }
        if (ack.parts.size() >= 3 && ack.parts[1] == "ERR")
        {
            out_error = "NAT server error: " + ack.parts[2];
        }
        else
        {
            out_error = "Unexpected response while confirming the claim.";
        }
    }
    else
    {
        out_error = ack.error;
    }

    // Fallback: CHECK with our token. If the server still considers us
    // the owner of this code, the response is CHECKOK with our code.
    std::string check_payload = std::string(CoreN02Traversal::kProtocol) +
                                "|CHECK|" + code + "|" + owner_token;
    auto check = CoreN02SendRequest(check_payload);
    if (check.status != CoreN02Traversal::Status::Ok)
    {
        out_error = check.error;
        return false;
    }

    if (check.parts.size() >= 3 && check.parts[1] == "CHECKOK")
    {
        const std::string returned = CoreN02NormalizeCode(check.parts[2]);
        const std::string expected = CoreN02NormalizeCode(code);
        if (!returned.empty() && returned == expected)
        {
            out_error.clear();
            return true;
        }
    }

    if (check.parts.size() >= 3 && check.parts[1] == "ERR")
    {
        out_error = "NAT server error: " + check.parts[2];
    }
    else if (out_error.empty())
    {
        out_error = "Unexpected response while confirming the claim.";
    }
    return false;
}

bool CoreN02Release(const std::string& owner_token, std::string& out_error)
{
    if (owner_token.empty())
    {
        out_error.clear();
        return true;
    }

    std::string payload = std::string(CoreN02Traversal::kProtocol) +
                          "|RELEASE|" + owner_token;
    auto r = CoreN02SendRequest(payload);
    if (r.status != CoreN02Traversal::Status::Ok)
    {
        out_error = r.error;
        return false;
    }

    if (r.parts.size() >= 2 && r.parts[1] == "OK")
    {
        return true;
    }

    if (r.parts.size() >= 3 && r.parts[1] == "ERR")
    {
        const std::string& reason = r.parts[2];
        if (reason == "NOAUTH")
        {
            // Already gone or never owned by us — caller's reservation
            // is effectively released either way.
            return true;
        }
        out_error = "NAT server error: " + reason;
        return false;
    }

    out_error = "Unexpected response while releasing the claim.";
    return false;
}
