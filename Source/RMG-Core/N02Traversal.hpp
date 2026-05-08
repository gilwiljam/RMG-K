#ifndef CORE_N02TRAVERSAL_HPP
#define CORE_N02TRAVERSAL_HPP

#include "Library.hpp"

#include <string>
#include <vector>

/*
 * N02 Traversal Server client — RMG-Core copy.
 *
 * The N02 traversal server (nat.smash64.net:6364) provides connect-code
 * based NAT punch-through for peer-to-peer netplay. A host claims a
 * short alphanumeric code (e.g. "PIKA@555") and registers its public
 * endpoint with the server; a joiner asks the server to look the code
 * up and the server matchmakes the two endpoints, sending each side
 * the other's IP/port and orchestrating the UDP hole punch.
 *
 * This file is a duplicate of the synchronous traversal helpers that
 * live alongside the existing Kaillera Qt dialogs
 * (KailleraNetplayDialog.cpp / KailleraP2PDialog.cpp). The duplicate
 * lives in RMG-Core so Frame Zero's Qt code can call it without
 * pulling in the Kaillera dialog module — and so the Kaillera copy
 * can eventually be deleted once Kaillera is retired without breaking
 * Frame Zero. The protocol on the wire is identical, so a single
 * traversal server installation serves both clients.
 *
 * Wire protocol (UDP, no framing — one request datagram, one response
 * datagram, all '|'-separated ASCII fields, leading NUL byte tolerated
 * on responses):
 *
 *   request                              response
 *   -----------------------------------  -----------------------------------
 *   N02TRAV1|CHECK|<target>[|<token>]    N02TRAV1|CHECKOK|<code>
 *                                        N02TRAV1|CHECKSUGGEST|<req>|<sugg>
 *                                        N02TRAV1|ERR|<reason>
 *   N02TRAV1|CLAIM|<target>[|<token>]    N02TRAV1|CLAIMOK|<code>|<token>
 *                                        N02TRAV1|CLAIMSUGGEST|<req>|<sugg>
 *                                        N02TRAV1|ERR|<reason>
 *   N02TRAV1|CLAIMACK|<token>            N02TRAV1|OK
 *                                        N02TRAV1|ERR|<reason>
 *   N02TRAV1|RELEASE|<token>             N02TRAV1|OK
 *                                        N02TRAV1|ERR|<reason>
 *
 * <target> is a normalized code (see N02NormalizeClaimTarget) or the
 * literal string "AUTO" to let the server pick a fresh code.
 *
 * This module only implements the synchronous dance — claim, check,
 * confirm, release. The host/joiner punch flow (HOSTOPEN, HOSTKEEP,
 * JOIN, HOST, PEER, PUNCH) requires an async UDP socket that's also
 * shared with the game traffic and is intentionally left to the
 * higher-level Frame Zero connect code.
 */

namespace CoreN02Traversal
{
    static constexpr const char* kHost     = "nat.smash64.net";
    static constexpr int         kPort     = 6364;
    static constexpr const char* kProtocol = "N02TRAV1";

    // Connect codes have a 3-or-4-letter prefix and a 1-3 digit suffix
    // (e.g. "ABC@1", "PIKA@555"). Used for both validation and
    // canonical formatting.
    static constexpr int kMaxDigits = 3;

    enum class Status
    {
        Ok,
        SocketError,    // socket()/bind() failure
        ResolveError,   // DNS lookup failed
        SendError,      // sendto() failed
        Timeout,        // no response within the timeout window
        InvalidResponse // response missing protocol marker / too short
    };

    struct Result
    {
        Status                          status = Status::Ok;
        std::vector<std::string>        parts;   // '|'-split response
        std::string                     error;   // human-readable description
    };
}

// ---- Code normalization ----

// Returns true if `s` parses as a connect code (3-4 letters + 1-3 digits,
// optional separator char @/#/-/_ between the two halves, case-insensitive,
// trimmed).
CORE_EXPORT bool CoreN02LooksLikeCode(const std::string& s);

// Returns the canonical form of a connect code (e.g. "PIKA@555") with
// leading zeros stripped from the digit run. Returns an empty string
// when `s` does not parse as a code.
CORE_EXPORT std::string CoreN02NormalizeCode(const std::string& s);

// Returns the canonical form of a CLAIM target — either a normalized
// code, the literal string "AUTO", or just the prefix (e.g. "PIKA")
// when the user wants the server to assign digits. Returns an empty
// string when `s` cannot be parsed.
CORE_EXPORT std::string CoreN02NormalizeClaimTarget(const std::string& s);

// ---- Synchronous request/response ----

// Sends a single datagram to the traversal server and waits up to
// `timeout_ms` for a response. The response is split on '|' into
// `out.parts`; out.parts[0] is verified to equal kProtocol. A leading
// NUL byte (some traversal server builds prepend one) is stripped
// before splitting.
CORE_EXPORT CoreN02Traversal::Result CoreN02SendRequest(const std::string& payload,
                                                        int timeout_ms = 2000);

// ---- High-level operations ----

// CHECK: ask whether a code is currently available. Pass an existing
// owner_token (saved from a prior CLAIMOK) to allow re-checking your
// own reservation. The reply is CHECKOK with the assigned code,
// CHECKSUGGEST with a fallback when the requested code is taken, or
// ERR.
CORE_EXPORT CoreN02Traversal::Result CoreN02Check(const std::string& target,
                                                  const std::string& owner_token = "");

// CLAIM: reserve a connect code for hosting. Pass target="AUTO" to let
// the server pick. Pass owner_token to refresh an existing reservation.
// The reply is CLAIMOK with the assigned code + owner token,
// CLAIMSUGGEST with a fallback, or ERR.
CORE_EXPORT CoreN02Traversal::Result CoreN02Claim(const std::string& target,
                                                  const std::string& owner_token = "");

// Convenience wrapper around CLAIM with target="AUTO".
CORE_EXPORT CoreN02Traversal::Result CoreN02ClaimAuto(void);

// CLAIMACK followed by a CHECK fallback, used to confirm the server
// accepted a claim. Returns true when the assigned code matches `code`.
// On false, `out_error` describes why.
CORE_EXPORT bool CoreN02ConfirmClaim(const std::string& code,
                                     const std::string& owner_token,
                                     std::string& out_error);

// RELEASE: drop a reservation. Returns true on OK / NOAUTH (already
// released or never owned by us). On false, `out_error` describes why.
CORE_EXPORT bool CoreN02Release(const std::string& owner_token,
                                std::string& out_error);

#endif // CORE_N02TRAVERSAL_HPP
