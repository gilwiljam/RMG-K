/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "MediaLoader.hpp"
#include "RomSettings.hpp"
#include "Emulation.hpp"
#include "RomHeader.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Netplay.hpp"
#include "Kaillera.hpp"
#include "FrameZero.hpp"
#include "Plugins.hpp"
#include "Cheats.hpp"
#include "Callback.hpp"
#include "FrameZeroState.hpp"
#include "Error.hpp"
#include "File.hpp"
#include "Rom.hpp"

#include "m64p/Api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Windows/POSIX dynamic loading
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Forward declarations for PIF structures
extern "C" {
    struct pif;
    struct pif_channel {
        void* jbd;
        const void* ijbd;
        uint8_t* tx;
        uint8_t* tx_buf;
        uint8_t* rx;
        uint8_t* rx_buf;
    };
    struct pif {
        uint8_t* base;
        uint8_t* ram;
        struct pif_channel channels[6];  // PIF_CHANNELS_COUNT = 6
    };

    // Joybus command constants
    enum {
        JCMD_STATUS = 0x00,
        JCMD_CONTROLLER_READ = 0x01,
        JCMD_PAK_READ = 0x02,
        JCMD_PAK_WRITE = 0x03,
        JCMD_EEPROM_READ = 0x04,
        JCMD_EEPROM_WRITE = 0x05,
        JCMD_RESET = 0xff
    };

    typedef void (*pif_sync_callback_t)(struct pif*);
}

//
// Local Variables
//

// Frame counter for Kaillera sync (updated via frame callback)
static int s_CurrentFrame = 0;

//
// Frame Zero — Phase 0.5 silent-advance spike (THROWAWAY)
//
// Activate by setting environment variable FRAME_ZERO_SPIKE=1 before launch.
// State machine driven from FrameCallback. Measures wall-clock-per-frame
// across 300 normal frames + 300 silent frames, logs a one-shot summary,
// then disarms itself.
//
// Speed limiter is toggled OFF for the measurement window so we see real
// frame-advance time, not sleep.
//

namespace {

enum class SpikePhase
{
    Disabled,      // env var not set; do nothing
    Settle,        // wait N frames for the game to stabilise
    MeasureNormal, // collect timings with g_RollbackMode=0
    Switch,        // flip g_RollbackMode and zero out timing window
    MeasureSilent, // collect timings with g_RollbackMode=1
    Done           // report and disarm
};

constexpr int kSpikeSettleFrames = 600;
constexpr int kSpikeMeasureFrames = 300;

struct SpikeState
{
    SpikePhase phase = SpikePhase::Disabled;
    int frameInPhase = 0;
    int prevSpeedLimiter = 1;
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint lastFrameTime;
    std::vector<double> normalSamplesUs;
    std::vector<double> silentSamplesUs;
    void (*setRollbackMode)(int) = nullptr;
};

static SpikeState s_Spike;

static void SpikeReportSamples(const char* label, const std::vector<double>& samples)
{
    if (samples.empty())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            std::string("[FrameZero spike] ") + label + ": no samples");
        return;
    }

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (double v : sorted) sum += v;
    const double mean = sum / static_cast<double>(sorted.size());
    const double minV = sorted.front();
    const double maxV = sorted.back();
    const double p50  = sorted[sorted.size() / 2];
    const double p99  = sorted[(sorted.size() * 99) / 100];

    char buf[256];
    snprintf(buf, sizeof(buf),
        "[FrameZero spike] %s: n=%zu  min=%.2fms  p50=%.2fms  mean=%.2fms  p99=%.2fms  max=%.2fms",
        label, sorted.size(),
        minV / 1000.0, p50 / 1000.0, mean / 1000.0, p99 / 1000.0, maxV / 1000.0);

    CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
}

static void SpikeArm()
{
    const char* env = std::getenv("FRAME_ZERO_SPIKE");
    if (env == nullptr || env[0] == '\0' || env[0] == '0')
    {
        s_Spike.phase = SpikePhase::Disabled;
        return;
    }

    // Resolve core_set_rollback_mode dynamically.
    void* coreHandle = m64p::Core.GetHandle();
    s_Spike.setRollbackMode = nullptr;
    if (coreHandle)
    {
#ifdef _WIN32
        s_Spike.setRollbackMode =
            (void(*)(int))GetProcAddress((HMODULE)coreHandle, "core_set_rollback_mode");
#else
        s_Spike.setRollbackMode =
            (void(*)(int))dlsym(coreHandle, "core_set_rollback_mode");
#endif
    }

    if (s_Spike.setRollbackMode == nullptr)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "[FrameZero spike] core_set_rollback_mode not exported — rebuild mupen64plus-core. Spike disabled.");
        s_Spike.phase = SpikePhase::Disabled;
        return;
    }

    s_Spike.phase = SpikePhase::Settle;
    s_Spike.frameInPhase = 0;
    s_Spike.normalSamplesUs.clear();
    s_Spike.silentSamplesUs.clear();
    s_Spike.normalSamplesUs.reserve(kSpikeMeasureFrames);
    s_Spike.silentSamplesUs.reserve(kSpikeMeasureFrames);

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "[FrameZero spike] Armed. Will settle for "
        + std::to_string(kSpikeSettleFrames) + " frames, then measure "
        + std::to_string(kSpikeMeasureFrames) + " normal + "
        + std::to_string(kSpikeMeasureFrames) + " silent frames.");
}

static void SpikeTick()
{
    using clock = std::chrono::steady_clock;

    if (s_Spike.phase == SpikePhase::Disabled || s_Spike.phase == SpikePhase::Done)
        return;

    const auto now = clock::now();

    switch (s_Spike.phase)
    {
        case SpikePhase::Settle:
        {
            ++s_Spike.frameInPhase;
            if (s_Spike.frameInPhase >= kSpikeSettleFrames)
            {
                // Disable speed limiter so we measure true frame-advance time.
                int currentLimiter = 1;
                m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY,
                    M64CORE_SPEED_LIMITER, &currentLimiter);
                s_Spike.prevSpeedLimiter = currentLimiter;
                int off = 0;
                m64p::Core.DoCommand(M64CMD_CORE_STATE_SET,
                    M64CORE_SPEED_LIMITER, &off);

                s_Spike.phase = SpikePhase::MeasureNormal;
                s_Spike.frameInPhase = 0;
                s_Spike.lastFrameTime = now;
                CoreAddCallbackMessage(CoreDebugMessageType::Info,
                    "[FrameZero spike] Settled. Measuring normal frames…");
            }
            break;
        }
        case SpikePhase::MeasureNormal:
        {
            const double dtUs =
                std::chrono::duration<double, std::micro>(now - s_Spike.lastFrameTime).count();
            s_Spike.lastFrameTime = now;
            s_Spike.normalSamplesUs.push_back(dtUs);
            ++s_Spike.frameInPhase;
            if (s_Spike.frameInPhase >= kSpikeMeasureFrames)
            {
                s_Spike.phase = SpikePhase::Switch;
                s_Spike.frameInPhase = 0;
            }
            break;
        }
        case SpikePhase::Switch:
        {
            // Enter rollback mode for the silent measurement window.
            // We hold the flag for the entire window rather than calling
            // advance_one_frame_silent() per frame, because the latter requires
            // the emulator to be paused — here we want continuous running.
            if (s_Spike.setRollbackMode)
                s_Spike.setRollbackMode(1);
            s_Spike.phase = SpikePhase::MeasureSilent;
            s_Spike.frameInPhase = 0;
            s_Spike.lastFrameTime = now;
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "[FrameZero spike] Rollback mode ON. Measuring silent frames…");
            break;
        }
        case SpikePhase::MeasureSilent:
        {
            const double dtUs =
                std::chrono::duration<double, std::micro>(now - s_Spike.lastFrameTime).count();
            s_Spike.lastFrameTime = now;
            s_Spike.silentSamplesUs.push_back(dtUs);
            ++s_Spike.frameInPhase;
            if (s_Spike.frameInPhase >= kSpikeMeasureFrames)
            {
                if (s_Spike.setRollbackMode)
                    s_Spike.setRollbackMode(0);

                int prev = s_Spike.prevSpeedLimiter;
                m64p::Core.DoCommand(M64CMD_CORE_STATE_SET,
                    M64CORE_SPEED_LIMITER, &prev);

                SpikeReportSamples("normal", s_Spike.normalSamplesUs);
                SpikeReportSamples("silent", s_Spike.silentSamplesUs);

                // Compute and report the all-important ratio.
                if (!s_Spike.normalSamplesUs.empty() && !s_Spike.silentSamplesUs.empty())
                {
                    auto mean = [](const std::vector<double>& v) {
                        double sum = 0.0;
                        for (double x : v) sum += x;
                        return sum / static_cast<double>(v.size());
                    };
                    const double meanNormal = mean(s_Spike.normalSamplesUs);
                    const double meanSilent = mean(s_Spike.silentSamplesUs);
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[FrameZero spike] Silent vs normal: %.2fms vs %.2fms (silent is %.0f%% of normal)",
                        meanSilent / 1000.0, meanNormal / 1000.0,
                        meanNormal > 0.0 ? (100.0 * meanSilent / meanNormal) : 0.0);
                    CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
                }

                CoreAddCallbackMessage(CoreDebugMessageType::Info,
                    "[FrameZero spike] Done. Speed limiter restored. Spike disarmed.");
                s_Spike.phase = SpikePhase::Done;
            }
            break;
        }
        default:
            break;
    }
}

//
// Frame Zero — Phase 1 savestate roundtrip correctness test (THROWAWAY)
//
// Activate by setting FRAME_ZERO_ROUNDTRIP=1 before launch.
//
// Two modes (FRAME_ZERO_ROUNDTRIP_MODE):
//   0 (default) — full advance test:
//       capture(slot0) → advance N → capture(slot1) → restore(slot0)
//       → advance N → capture(slot2). slot1 and slot2 must be byte-identical.
//       Failure means EITHER the snapshot is missing state OR the emulator
//       advances non-deterministically given identical input state.
//   1 — reversibility test:
//       capture(slot0) → restore(slot0) → capture(slot3). slot0 and slot3
//       must be byte-identical. This isolates "is the snapshot complete"
//       from advance non-determinism. If reversibility passes but the
//       full test fails, the gap is in execution determinism, not state.
//
// IMPORTANT: do not press any buttons during the test, or the second pass
// will see different inputs and the test will report a false desync.
//

enum class RoundtripPhase
{
    Disabled,
    Settle,             // wait for game to stabilise
    Capture0,           // capture slot 0
    /* Reversibility mode (FRAME_ZERO_ROUNDTRIP_MODE=1): in a single tick,
     * captures slot 0, then immediately restores slot 0, then captures
     * slot 3 with NO emulation between. Tests whether save+restore is
     * byte-perfect — any byte that differs between slot 0 and slot 3 is
     * a field the snapshot reads but doesn't restore (or a field that
     * the load path mutates as a side-effect). */
    AdvanceFirst,       // advance N frames
    Capture1,           // capture slot 1 (post-advance state)
    Restore0,           // restore slot 0
    PostRestoreHold,    // hold rewound state on screen so user can see it
    AdvanceSecond,      // advance N frames again
    Capture2,           // capture slot 2 (post-redo state)
    Compare,            // diff slot 1 vs slot 2
    Done
};

/* Defaults overridable via env vars (read at arm time):
 *   FRAME_ZERO_ROUNDTRIP_SETTLE   = settle frames before test fires (min 60)
 *   FRAME_ZERO_ROUNDTRIP_ADVANCE  = advance frames per leg (min 1)
 *
 * For diagnosis: shrinking advance to e.g. 1 isolates whether divergence is
 * per-frame or only accumulates over many frames. */
constexpr int kRoundtripSettleFramesDefault = 300;
constexpr int kRoundtripAdvanceFramesDefault = 300;
constexpr int kRoundtripHoldFrames = 60;        // 1 sec hold on rewound state

struct RoundtripState
{
    RoundtripPhase phase = RoundtripPhase::Disabled;
    int frameInPhase = 0;
    int settleFrames = kRoundtripSettleFramesDefault;
    int advanceFrames = kRoundtripAdvanceFramesDefault;
    int holdFrames = kRoundtripHoldFrames;  // computed at arm time
    int mode = 0;  // 0 = full advance test, 1 = reversibility only
    unsigned int frameAtCapture0 = 0;
    unsigned int frameAtCapture1 = 0;
    unsigned int frameAtRestore = 0;
    int prevSpeedLimiter = 1;
    bool savedSpeedLimiter = false;
    int rollbackEnable = 0;  // 1 = engage g_RollbackMode during advance phases
    void (*setInputNeutralize)(int) = nullptr;
    void (*setRollbackMode)(int) = nullptr;
};

static RoundtripState s_Roundtrip;

static void RoundtripForceLimiterOn()
{
    int curr = 1;
    if (m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_LIMITER, &curr) == M64ERR_SUCCESS)
    {
        s_Roundtrip.prevSpeedLimiter = curr;
        s_Roundtrip.savedSpeedLimiter = true;
    }
    int on = 1;
    m64p::Core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &on);
}

static void RoundtripRestoreLimiter()
{
    if (!s_Roundtrip.savedSpeedLimiter)
        return;
    int prev = s_Roundtrip.prevSpeedLimiter;
    m64p::Core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &prev);
    s_Roundtrip.savedSpeedLimiter = false;
}

static void RoundtripArm()
{
    const char* env = std::getenv("FRAME_ZERO_ROUNDTRIP");
    if (env == nullptr || env[0] == '\0' || env[0] == '0')
    {
        s_Roundtrip.phase = RoundtripPhase::Disabled;
        return;
    }

    if (!FrameZero::stateInit())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "[FrameZero roundtrip] FrameZero::stateInit() failed — savestate API not exported. Test disabled.");
        s_Roundtrip.phase = RoundtripPhase::Disabled;
        return;
    }

    /* Settle override: longer settle lets the user navigate menus and start
     * a match before the test fires. Floors at 60 (1 second) so we always
     * have at least one VI tick to stabilise. */
    s_Roundtrip.settleFrames = kRoundtripSettleFramesDefault;
    if (const char* settle_env = std::getenv("FRAME_ZERO_ROUNDTRIP_SETTLE"))
    {
        int v = std::atoi(settle_env);
        if (v >= 60)
            s_Roundtrip.settleFrames = v;
    }

    /* Advance override: lets us shrink the leg to isolate per-frame vs
     * accumulated divergence. Min 1; if smaller than kRoundtripHoldFrames
     * we suppress the hold so the second leg's frame count still matches. */
    s_Roundtrip.advanceFrames = kRoundtripAdvanceFramesDefault;
    if (const char* adv_env = std::getenv("FRAME_ZERO_ROUNDTRIP_ADVANCE"))
    {
        int v = std::atoi(adv_env);
        if (v >= 1)
            s_Roundtrip.advanceFrames = v;
    }

    /* Mode select:
     *   0 (default) = full advance test (capture0→advance→capture1→restore0→advance→capture2→compare1vs2)
     *   1           = reversibility only (capture0→restore0→capture3→compare0vs3)
     * Mode 1 isolates "is the savestate complete" from "does the emulator
     * advance deterministically given identical state". */
    s_Roundtrip.mode = 0;
    if (const char* mode_env = std::getenv("FRAME_ZERO_ROUNDTRIP_MODE"))
    {
        int v = std::atoi(mode_env);
        if (v == 1)
            s_Roundtrip.mode = 1;
    }

    /* Rollback-mode opt-in: engages g_RollbackMode during advance phases so
     * the GFX updateScreen and audio push_samples side effects are suppressed
     * during the silent re-simulation legs. Doesn't change comparison input
     * (samples are still computed by HLE and written to N64 RDRAM), but it
     * exercises the production rollback wiring end-to-end and makes the
     * advance phases run at full headless speed. */
    s_Roundtrip.rollbackEnable = 0;
    if (const char* rb_env = std::getenv("FRAME_ZERO_ROUNDTRIP_ROLLBACK"))
    {
        if (rb_env[0] != '\0' && rb_env[0] != '0')
            s_Roundtrip.rollbackEnable = 1;
    }

    /* Resolve the input-neutralize hook. Optional — if mupen64plus-core is
     * older and doesn't export it, the test still runs but the user must
     * not press buttons during the advance phases. */
    s_Roundtrip.setInputNeutralize = nullptr;
    s_Roundtrip.setRollbackMode = nullptr;
    if (void* coreHandle = m64p::Core.GetHandle())
    {
#ifdef _WIN32
        s_Roundtrip.setInputNeutralize =
            (void(*)(int))GetProcAddress((HMODULE)coreHandle, "core_set_input_neutralize");
        s_Roundtrip.setRollbackMode =
            (void(*)(int))GetProcAddress((HMODULE)coreHandle, "core_set_rollback_mode");
#else
        s_Roundtrip.setInputNeutralize =
            (void(*)(int))dlsym(coreHandle, "core_set_input_neutralize");
        s_Roundtrip.setRollbackMode =
            (void(*)(int))dlsym(coreHandle, "core_set_rollback_mode");
#endif
    }

    s_Roundtrip.phase = RoundtripPhase::Settle;
    s_Roundtrip.frameInPhase = 0;
    s_Roundtrip.savedSpeedLimiter = false;

    /* If advance < hold, skip the hold (second leg would otherwise overshoot). */
    s_Roundtrip.holdFrames = (s_Roundtrip.advanceFrames < kRoundtripHoldFrames)
        ? 0 : kRoundtripHoldFrames;

    const char* rollbackStatus = s_Roundtrip.rollbackEnable
        ? (s_Roundtrip.setRollbackMode
              ? "ENABLED (silent re-simulation, no video/audio output)"
              : "REQUESTED but UNAVAILABLE (core_set_rollback_mode not exported)")
        : "off (default)";

    char buf[640];
    if (s_Roundtrip.mode == 1)
    {
        snprintf(buf, sizeof(buf),
            "[FrameZero roundtrip] Armed (REVERSIBILITY mode). Snapshot=%zu bytes. Plan: settle %d (%.1fs — play normally) -> capture0 -> RESTORE0 -> capture3 -> compare 0 vs 3. Input neutralization: %s. Rollback mode: %s.",
            FrameZero::stateSnapshotSize(),
            s_Roundtrip.settleFrames, s_Roundtrip.settleFrames / 60.0,
            s_Roundtrip.setInputNeutralize ? "enabled" : "UNAVAILABLE",
            rollbackStatus);
    }
    else
    {
        snprintf(buf, sizeof(buf),
            "[FrameZero roundtrip] Armed. Snapshot=%zu bytes. Plan: settle %d (%.1fs — play normally) -> capture0 -> advance %d -> capture1 -> RESTORE0 -> hold %d -> advance %d -> capture2 -> compare. Input neutralization: %s. Rollback mode: %s.",
            FrameZero::stateSnapshotSize(),
            s_Roundtrip.settleFrames, s_Roundtrip.settleFrames / 60.0,
            s_Roundtrip.advanceFrames, s_Roundtrip.holdFrames, s_Roundtrip.advanceFrames,
            s_Roundtrip.setInputNeutralize ? "enabled (your inputs ignored during advance phases)"
                                           : "UNAVAILABLE — rebuild mupen64plus-core, or hold still during advance phases",
            rollbackStatus);
    }
    CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
}

static void RoundtripDisarm()
{
    RoundtripRestoreLimiter();
    if (s_Roundtrip.setInputNeutralize)
        s_Roundtrip.setInputNeutralize(0);
    if (s_Roundtrip.setRollbackMode)
        s_Roundtrip.setRollbackMode(0);
    FrameZero::stateShutdown();
    s_Roundtrip.phase = RoundtripPhase::Done;
}

/* Diff two slots and log a structured report. Returns number of differing bytes. */
static size_t RoundtripDiffSlots(int slotA, int slotB, const char* label)
{
    if (!FrameZero::slotIsValid(slotA) || !FrameZero::slotIsValid(slotB))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero roundtrip] internal error: slots not valid for compare");
        return SIZE_MAX;
    }

    const uint8_t* buf_a = nullptr; size_t len_a = 0;
    const uint8_t* buf_b = nullptr; size_t len_b = 0;
    if (!FrameZero::slotBytes(slotA, &buf_a, &len_a) ||
        !FrameZero::slotBytes(slotB, &buf_b, &len_b))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
            "[FrameZero roundtrip] slotBytes() failed for compare");
        return SIZE_MAX;
    }

    char buf[256];

    if (len_a != len_b)
    {
        snprintf(buf, sizeof(buf),
            "[FrameZero roundtrip] FAIL (%s): state size differs — slot%d=%zu  slot%d=%zu",
            label, slotA, len_a, slotB, len_b);
        CoreAddCallbackMessage(CoreDebugMessageType::Error, std::string(buf));
        return SIZE_MAX;
    }

    size_t numDiff = 0;
    struct Range { size_t start; size_t end; };
    std::vector<Range> ranges;
    for (size_t i = 0; i < len_a; ++i)
    {
        if (buf_a[i] != buf_b[i])
        {
            ++numDiff;
            if (!ranges.empty() && ranges.back().end == i)
                ranges.back().end = i + 1;
            else
                ranges.push_back({i, i + 1});
        }
    }

    if (numDiff == 0)
    {
        snprintf(buf, sizeof(buf),
            "[FrameZero roundtrip] PASS (%s): %zu-byte snapshots identical (slot%d == slot%d)",
            label, len_a, slotA, slotB);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
        return 0;
    }

    snprintf(buf, sizeof(buf),
        "[FrameZero roundtrip] FAIL (%s): %zu/%zu bytes differ across %zu range(s) (slot%d vs slot%d). Listing up to 16:",
        label, numDiff, len_a, ranges.size(), slotA, slotB);
    CoreAddCallbackMessage(CoreDebugMessageType::Error, std::string(buf));

    const size_t maxList = std::min<size_t>(ranges.size(), 16);
    for (size_t r = 0; r < maxList; ++r)
    {
        const size_t s = ranges[r].start;
        const size_t e = ranges[r].end;
        const size_t n = e - s;
        char a_hex[40] = {0}, b_hex[40] = {0};
        const size_t showN = std::min<size_t>(n, 8);
        char* ap = a_hex; char* bp = b_hex;
        for (size_t k = 0; k < showN; ++k)
        {
            ap += snprintf(ap, sizeof(a_hex) - (ap - a_hex), "%02x ", buf_a[s + k]);
            bp += snprintf(bp, sizeof(b_hex) - (bp - b_hex), "%02x ", buf_b[s + k]);
        }
        snprintf(buf, sizeof(buf),
            "  [%zu] offset=0x%zx (%zu) len=%zu  slot%d: %s  slot%d: %s",
            r, s, s, n, slotA, a_hex, slotB, b_hex);
        CoreAddCallbackMessage(CoreDebugMessageType::Error, std::string(buf));
    }
    return numDiff;
}

static void RoundtripCompare()
{
    char label[64];
    snprintf(label, sizeof(label), "ADVANCE %d frames per leg",
             s_Roundtrip.advanceFrames);
    RoundtripDiffSlots(1, 2, label);
}

static void RoundtripTick()
{
    if (s_Roundtrip.phase == RoundtripPhase::Disabled ||
        s_Roundtrip.phase == RoundtripPhase::Done)
        return;

    char logbuf[256];

    switch (s_Roundtrip.phase)
    {
        case RoundtripPhase::Settle:
            if (++s_Roundtrip.frameInPhase >= s_Roundtrip.settleFrames) {
                /* Take ownership of the speed limiter for the rest of the test
                 * so the visual rewind isn't fast-forwarded out of existence. */
                RoundtripForceLimiterOn();
                /* Engage input neutralization at end of Settle, before capture.
                 * Both legs of the test will then see all-zero controller input
                 * regardless of what the user is pressing. */
                if (s_Roundtrip.setInputNeutralize)
                    s_Roundtrip.setInputNeutralize(1);
                /* Optionally engage rollback-mode (suppresses GFX updateScreen
                 * and audio push_samples side effects). Both legs see the
                 * same suppression so the comparison is unaffected; this
                 * exercises the production rollback wiring end-to-end. */
                bool rollbackOn = false;
                if (s_Roundtrip.rollbackEnable && s_Roundtrip.setRollbackMode) {
                    s_Roundtrip.setRollbackMode(1);
                    rollbackOn = true;
                }
                CoreAddCallbackMessage(CoreDebugMessageType::Info,
                    s_Roundtrip.setInputNeutralize
                        ? (rollbackOn
                              ? "[FrameZero roundtrip] Settle done. Speed limiter forced ON. INPUT NEUTRALIZED. ROLLBACK MODE ENGAGED (no video/audio output during test)."
                              : "[FrameZero roundtrip] Settle done. Speed limiter forced ON. INPUT NEUTRALIZED — your buttons are ignored until the test completes.")
                        : "[FrameZero roundtrip] Settle done. Speed limiter forced ON. (input neutralization unavailable — DO NOT press any buttons.)");
                s_Roundtrip.phase = RoundtripPhase::Capture0;
                s_Roundtrip.frameInPhase = 0;
            }
            break;

        case RoundtripPhase::Capture0:
            if (s_Roundtrip.mode == 1) {
                /* Reversibility test: capture slot 0, restore slot 0, capture
                 * slot 3 — ALL in this single tick so no emulation runs
                 * between them. If slot 0 != slot 3, the load path doesn't
                 * fully restore something the save path reads (or load has
                 * a side-effect that mutates state the next save reads). */
                if (!FrameZero::captureState(0)) {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero roundtrip] reversibility: capture slot 0 failed");
                    RoundtripDisarm();
                    break;
                }
                s_Roundtrip.frameAtCapture0 = s_CurrentFrame;

                if (!FrameZero::restoreState(0)) {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero roundtrip] reversibility: restore slot 0 failed");
                    RoundtripDisarm();
                    break;
                }

                if (!FrameZero::captureState(3)) {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero roundtrip] reversibility: capture slot 3 failed");
                    RoundtripDisarm();
                    break;
                }

                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] REVERSIBILITY done in one tick at emulator frame %u (no emulation between save→load→save). Comparing slot 0 vs slot 3…",
                    s_Roundtrip.frameAtCapture0);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));

                size_t diff = RoundtripDiffSlots(0, 3, "REVERSIBILITY save+restore (no emulation between)");
                if (diff == 0) {
                    CoreAddCallbackMessage(CoreDebugMessageType::Info,
                        "[FrameZero roundtrip] Reversibility passed — every byte the save path reads is also written by the load path. Any advance-test failure is therefore from non-deterministic emulator execution, not missing state.");
                } else if (diff != SIZE_MAX) {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero roundtrip] Reversibility FAILED — listed offsets show fields read by save but not restored by load (or mutated by load as a side-effect). These are the next things to fix.");
                }
                CoreAddCallbackMessage(CoreDebugMessageType::Info,
                    "[FrameZero roundtrip] Done. Speed limiter restored.");
                RoundtripDisarm();
            } else if (FrameZero::captureState(0)) {
                s_Roundtrip.frameAtCapture0 = s_CurrentFrame;
                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] Captured slot 0 at emulator frame %u. Advancing %d frames (~%.2fs game time)…",
                    s_Roundtrip.frameAtCapture0, s_Roundtrip.advanceFrames, s_Roundtrip.advanceFrames / 60.0);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));
                s_Roundtrip.phase = RoundtripPhase::AdvanceFirst;
                s_Roundtrip.frameInPhase = 0;
            } else {
                CoreAddCallbackMessage(CoreDebugMessageType::Error,
                    "[FrameZero roundtrip] capture slot 0 failed");
                RoundtripDisarm();
            }
            break;

        case RoundtripPhase::AdvanceFirst:
            if (++s_Roundtrip.frameInPhase >= s_Roundtrip.advanceFrames) {
                s_Roundtrip.phase = RoundtripPhase::Capture1;
                s_Roundtrip.frameInPhase = 0;
            }
            break;

        case RoundtripPhase::Capture1:
            if (FrameZero::captureState(1)) {
                s_Roundtrip.frameAtCapture1 = s_CurrentFrame;
                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] Captured slot 1 at emulator frame %u (advanced %u frames). Restoring slot 0 — WATCH THE SCREEN…",
                    s_Roundtrip.frameAtCapture1,
                    s_Roundtrip.frameAtCapture1 - s_Roundtrip.frameAtCapture0);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));
                s_Roundtrip.phase = RoundtripPhase::Restore0;
                s_Roundtrip.frameInPhase = 0;
            } else {
                CoreAddCallbackMessage(CoreDebugMessageType::Error,
                    "[FrameZero roundtrip] capture slot 1 failed");
                RoundtripDisarm();
            }
            break;

        case RoundtripPhase::Restore0:
        {
            unsigned int beforeRestore = s_CurrentFrame;
            if (FrameZero::restoreState(0)) {
                s_Roundtrip.frameAtRestore = beforeRestore;
                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] Restored slot 0. Was at emulator frame %u. Holding %d frames so you can see the rewound state…",
                    beforeRestore, s_Roundtrip.holdFrames);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));
                /* If holdFrames==0, skip PostRestoreHold entirely — otherwise
                 * the increment-then-check pattern in PostRestoreHold burns 1
                 * frame even at holdFrames=0, making leg 2 advance 1 more frame
                 * than leg 1 and breaking the byte-identical comparison. */
                s_Roundtrip.phase = (s_Roundtrip.holdFrames > 0)
                    ? RoundtripPhase::PostRestoreHold
                    : RoundtripPhase::AdvanceSecond;
                s_Roundtrip.frameInPhase = 0;
            } else {
                CoreAddCallbackMessage(CoreDebugMessageType::Error,
                    "[FrameZero roundtrip] restore slot 0 failed");
                RoundtripDisarm();
            }
            break;
        }

        case RoundtripPhase::PostRestoreHold:
            if (s_Roundtrip.frameInPhase == 0) {
                /* First frame after the restore — emulator frame counter should
                 * have jumped back to ~Capture0 if the savestate carries it. */
                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] First frame post-restore: emulator frame=%u (was %u just before restore — Δ=%d). Now re-advancing…",
                    s_CurrentFrame, s_Roundtrip.frameAtRestore,
                    (int)s_CurrentFrame - (int)s_Roundtrip.frameAtRestore);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));
            }
            if (++s_Roundtrip.frameInPhase >= s_Roundtrip.holdFrames) {
                s_Roundtrip.phase = RoundtripPhase::AdvanceSecond;
                s_Roundtrip.frameInPhase = 0;
            }
            break;

        case RoundtripPhase::AdvanceSecond:
            /* PostRestoreHold also advances emulation (holdFrames), so this leg
             * only needs (advanceFrames - holdFrames) more frames to match the
             * first leg's total advance from Capture0. */
            if (++s_Roundtrip.frameInPhase >= (s_Roundtrip.advanceFrames - s_Roundtrip.holdFrames)) {
                s_Roundtrip.phase = RoundtripPhase::Capture2;
                s_Roundtrip.frameInPhase = 0;
            }
            break;

        case RoundtripPhase::Capture2:
            if (FrameZero::captureState(2)) {
                snprintf(logbuf, sizeof(logbuf),
                    "[FrameZero roundtrip] Captured slot 2 at emulator frame %u. Comparing slot 1 vs slot 2…",
                    s_CurrentFrame);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(logbuf));
                s_Roundtrip.phase = RoundtripPhase::Compare;
            } else {
                CoreAddCallbackMessage(CoreDebugMessageType::Error,
                    "[FrameZero roundtrip] capture slot 2 failed");
                RoundtripDisarm();
            }
            break;

        case RoundtripPhase::Compare:
            RoundtripCompare();
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "[FrameZero roundtrip] Done. Speed limiter restored.");
            RoundtripDisarm();
            break;

        default:
            break;
    }
}

} // namespace

namespace {

//
// Phase 3 stress-test harness — local sync test for the GekkoNet
// integration. Activated by FRAME_ZERO_STRESS=<frames> (or =1 to use
// the default 10000). After the standard settle period we open a
// StressTest session for the configured player count and let it run
// for `frames` frames. Any DESYNC events are logged via the existing
// pump path. At target the session is torn down and a one-shot
// summary line is logged.
//
// Use FRAME_ZERO_STRESS_PLAYERS to override player count (default 2).
// Use FRAME_ZERO_STRESS_SETTLE to override the pre-arm settle window
// (default 600 frames, same as the spike).
//

enum class StressPhase
{
    Disabled,
    Settle,
    Running,
    Done,
};

constexpr int kStressSettleFramesDefault = 600;
constexpr int kStressFramesDefault       = 10000;
constexpr int kStressPlayersDefault      = 2;

struct StressState
{
    StressPhase phase    = StressPhase::Disabled;
    int settleFrames     = kStressSettleFramesDefault;
    int targetFrames     = kStressFramesDefault;
    int players          = kStressPlayersDefault;
    int frameInPhase     = 0;
};

static StressState s_Stress;

static int parse_env_int(const char* name, int default_v, int min_v)
{
    const char* env = std::getenv(name);
    if (env == nullptr || env[0] == '\0')
        return default_v;
    int v = std::atoi(env);
    if (v < min_v)
        return default_v;
    return v;
}

static void StressArm()
{
    const char* env = std::getenv("FRAME_ZERO_STRESS");
    if (env == nullptr || env[0] == '\0' || env[0] == '0')
    {
        s_Stress.phase = StressPhase::Disabled;
        return;
    }

#ifndef FRAME_ZERO
    CoreAddCallbackMessage(CoreDebugMessageType::Warning,
        "[FrameZero stress] FRAME_ZERO_STRESS=1 but build does not include FRAME_ZERO support. Test disabled.");
    s_Stress.phase = StressPhase::Disabled;
    return;
#else
    s_Stress.targetFrames = std::atoi(env);
    if (s_Stress.targetFrames < 60)
        s_Stress.targetFrames = kStressFramesDefault;

    s_Stress.settleFrames = parse_env_int("FRAME_ZERO_STRESS_SETTLE",
                                          kStressSettleFramesDefault, 60);
    s_Stress.players      = parse_env_int("FRAME_ZERO_STRESS_PLAYERS",
                                          kStressPlayersDefault, 1);
    if (s_Stress.players > 4) s_Stress.players = 4;

    s_Stress.phase        = StressPhase::Settle;
    s_Stress.frameInPhase = 0;

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "[FrameZero stress] Armed. settle=%d frames, run=%d frames, players=%d.",
        s_Stress.settleFrames, s_Stress.targetFrames, s_Stress.players);
    CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
#endif
}

static void StressTick()
{
#ifndef FRAME_ZERO
    return;
#else
    if (s_Stress.phase == StressPhase::Disabled || s_Stress.phase == StressPhase::Done)
        return;

    switch (s_Stress.phase)
    {
        case StressPhase::Settle:
        {
            ++s_Stress.frameInPhase;
            if (s_Stress.frameInPhase >= s_Stress.settleFrames)
            {
                if (!CoreInitFrameZero())
                {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero stress] CoreInitFrameZero failed — disabling test.");
                    s_Stress.phase = StressPhase::Done;
                    break;
                }
                /* Clean up any session leaked from a previous emulation
                 * run (CoreEndFrameZeroSession is idempotent). */
                CoreEndFrameZeroSession();
                if (!CoreStartFrameZeroStressSession(s_Stress.players))
                {
                    CoreAddCallbackMessage(CoreDebugMessageType::Error,
                        "[FrameZero stress] CoreStartFrameZeroStressSession failed — disabling test.");
                    s_Stress.phase = StressPhase::Done;
                    break;
                }
                s_Stress.phase = StressPhase::Running;
                s_Stress.frameInPhase = 0;
                CoreAddCallbackMessage(CoreDebugMessageType::Info,
                    "[FrameZero stress] Session started. Running…");
            }
            break;
        }
        case StressPhase::Running:
        {
            ++s_Stress.frameInPhase;
            if (s_Stress.frameInPhase >= s_Stress.targetFrames)
            {
                CoreEndFrameZeroSession();
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "[FrameZero stress] Done after %d frames. Inspect log for any DESYNC lines.",
                    s_Stress.targetFrames);
                CoreAddCallbackMessage(CoreDebugMessageType::Info, std::string(buf));
                s_Stress.phase = StressPhase::Done;
            }
            break;
        }
        default:
            break;
    }
#endif
}

} // namespace


#ifdef NETPLAY
// Maximum players supported by Kaillera
#define MAX_PLAYERS 8

// Cache for preventing duplicate syncs within same frame
static int s_LastSyncFrame = -1;
static uint32_t s_CachedSyncBuffer[MAX_PLAYERS] = {0};
static int s_CachedNumReceived = 0;

// Track whether we've already synced since the last frame advance
// This is more reliable than comparing frame numbers due to callback timing
static bool s_SyncedThisFrame = false;
#endif
// Frame callback function
static void FrameCallback(unsigned int frameIndex)
{
    s_CurrentFrame = frameIndex;
#ifdef NETPLAY
    // Reset sync flag at the start of each new frame
    // This ensures we sync exactly once per frame regardless of PIF polling timing
    s_SyncedThisFrame = false;
#endif

    // Frame Zero Phase 0.5 spike (no-op unless FRAME_ZERO_SPIKE=1)
    SpikeTick();

    // Frame Zero Phase 1 roundtrip test (no-op unless FRAME_ZERO_ROUNDTRIP=1)
    RoundtripTick();

    // Frame Zero Phase 3 stress test (no-op unless FRAME_ZERO_STRESS=N)
    StressTick();
}

// Kaillera PIF sync callback (called from mupen64plus-core after netplay sync)
static void KailleraPifSyncCallback(struct pif* pif)
{
#ifdef NETPLAY
    if (!CoreHasInitKaillera()) {
        return;
    }

    int player_num = CoreGetKailleraPlayerNumber();
    int num_players = CoreGetKailleraNumPlayers();

    if (player_num < 1 || player_num > MAX_PLAYERS) {
        return; // Invalid player number
    }

    // Check if this is a controller read command for channel 0 (local player)
    // We only want to sync on actual input reads, not status queries or other commands
    bool isControllerRead = (pif->channels[0].tx &&
                             pif->channels[0].tx_buf[0] == JCMD_CONTROLLER_READ &&
                             pif->channels[0].rx_buf != NULL);

    // Only sync with Kaillera on controller read commands, and only once per frame
    // This prevents syncing on JCMD_STATUS which would send zero input
    if (isControllerRead && !s_SyncedThisFrame) {
        // First controller read this frame - read local input and sync with Kaillera
        s_SyncedThisFrame = true;  // Mark as synced BEFORE calling Kaillera

        // Read 4-byte controller response from local controller
        // N64 controller format: [buttons_hi][buttons_lo][x_axis][y_axis]
        uint8_t* rx = pif->channels[0].rx_buf;
        uint32_t local_input = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];

        uint32_t sync_buffer[MAX_PLAYERS] = {0};
        sync_buffer[0] = local_input;

        // Synchronize with Kaillera - this must be called exactly ONCE per emulator frame
        int ret = CoreModifyKailleraPlayValues(sync_buffer, sizeof(uint32_t));

        if (ret < 0) {
            // Game ended or network error - cache zeros and continue
            // Don't stop emulation - let user manually stop
            // Mark game as inactive so UI buttons are re-enabled
            CoreMarkKailleraGameInactive();
            s_CachedNumReceived = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                s_CachedSyncBuffer[i] = 0;
            }
            return;
        }

        if (ret == 0) {
            // Frame delay period - n02 returns 0 while buffering initial frames
            // Use cached input from previous sync (or zeros if none yet)
            return;
        }

        int num_received = ret / sizeof(uint32_t);

        // Cache synced results for subsequent polls this frame and for writing to PIF
        s_CachedNumReceived = num_received;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            s_CachedSyncBuffer[i] = sync_buffer[i];
        }
    }

    // Write synchronized inputs to PIF RAM for all netplay players.
    // JCMD_STATUS/JCMD_RESET are handled unconditionally so games detect
    // controllers even before the first Kaillera sync (needed for playback
    // without a physical controller connected).
    // JCMD_CONTROLLER_READ data injection is gated on cache availability.
    int numPlayers = CoreGetKailleraNumPlayers();
    for (int i = 0; i < numPlayers && i < MAX_PLAYERS; i++) {
        if (pif->channels[i].tx && pif->channels[i].rx != NULL) {
            // Always clear error bits to show controller as connected
            *pif->channels[i].rx &= ~0xC0;

            uint8_t cmd = pif->channels[i].tx_buf[0];

            if (cmd == JCMD_STATUS || cmd == JCMD_RESET) {
                // Controller detection - force standard controller type response
                if (pif->channels[i].rx_buf != NULL) {
                    uint16_t type = 0x0500; // JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0; // No pak status
                }
            }
            else if (cmd == JCMD_CONTROLLER_READ) {
                // Write synced controller input from cache (only when populated)
                if (s_CachedNumReceived > 0 && i < s_CachedNumReceived && pif->channels[i].rx_buf != NULL) {
                    uint8_t* rx = pif->channels[i].rx_buf;
                    rx[0] = (s_CachedSyncBuffer[i] >> 24) & 0xFF;
                    rx[1] = (s_CachedSyncBuffer[i] >> 16) & 0xFF;
                    rx[2] = (s_CachedSyncBuffer[i] >> 8) & 0xFF;
                    rx[3] = s_CachedSyncBuffer[i] & 0xFF;
                }
            }
            else if (cmd == JCMD_PAK_READ && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[32] = 255;
            }
            else if (cmd == JCMD_PAK_WRITE && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[0] = 255;
            }
        }
    }
#endif // NETPLAY
}

//
// Local Functions
//

static bool get_emulation_state(m64p_emu_state& state)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &state);
    if (ret != M64ERR_SUCCESS)
    {
        error = "get_emulation_state m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

static void apply_coresettings_overlay(void)
{
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CPU_Emulator));
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem));
    CoreSettingsSetValue(SettingsID::Core_EnableDebugger, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_EnableDebugger));
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOpDenomPot));
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration));
    CoreSettingsSetValue(SettingsID::Core_SaveFileNameFormat, CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat));
    CoreSettingsSetValue(SettingsID::Core_GbCameraVideoCaptureBackend1, CoreSettingsGetStringValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1));
    // Reset DisableSaveFileLoading to default (false) - Kaillera will override this later if needed
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, false);
}

static void apply_game_coresettings_overlay(void)
{
    std::string section;
    CoreRomSettings romSettings;
    bool overrideCoreSettings;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentDefaultRomSettings(romSettings))
    {
        return;
    }

    section = romSettings.MD5;

    // when we don't need to override the core settings, return
    overrideCoreSettings = CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, section);
    if (!overrideCoreSettings)
    {
        return;
    }

    // apply settings overlay
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::Game_RandomizeInterrupt, section));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::Game_CPU_Emulator, section));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, section));
}

#ifdef NETPLAY
// Force HLE RSP plugin for Kaillera netplay - must be called BEFORE ROM open
// so the setting takes effect when plugins are loaded
static void apply_kaillera_rsp_override(void)
{
#ifdef _WIN32
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.dll"));
#else
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.so"));
#endif
}

// Force deterministic settings for Kaillera netplay to prevent desync
// These settings MUST be identical across all clients
// Called AFTER overlays so user/game settings don't override these
static void apply_kaillera_deterministic_settings(void)
{
    // Disable RandomizeInterrupt - critical for deterministic emulation
    // When enabled, interrupt timing varies randomly which causes desync
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, false);

    // Use dynamic recompiler for best performance
    // Value 0 = Pure Interpreter, 1 = Cached Interpreter, 2 = Dynamic Recompiler
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);

    // Set consistent CountPerOp values for deterministic timing
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, 0);
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, 0);

    // Set consistent SI DMA duration
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, -1);

    // Force extra memory enabled (8MB expansion pak) for consistent memory layout
    // Different memory configurations between players causes desync
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, false);

    // Disable save file loading so all players start with fresh/empty saves
    // This prevents desync from players having different in-game settings saved
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, true);
}

// Force deterministic settings for a Frame Zero (rollback) session.
// Rollback's correctness gate is stricter than Kaillera's: lockstep only
// requires "same inputs from frame 0 → same state", whereas rollback
// requires "any saved state, replayed from any point, → same result."
// In practice the baseline is identical to Kaillera's — randomness off,
// dynarec, no save-file influence — and we share it. If Frame Zero ever
// needs *additional* gates beyond what Kaillera tolerates, add them here.
static void apply_frame_zero_deterministic_settings(void)
{
    apply_kaillera_deterministic_settings();
}
#endif

static void apply_pif_rom_settings(void)
{
    CoreRomHeader romHeader;
    std::string error;
    m64p_error ret;
    int cpuEmulator;
    bool usePifROM;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentRomHeader(romHeader))
    {
        return;
    }

    // when we're using the dynarec, return
    cpuEmulator = CoreSettingsGetIntValue(SettingsID::Core_CPU_Emulator);
    if (cpuEmulator >= 2)
    {
        return;
    }

    usePifROM = CoreSettingsGetBoolValue(SettingsID::Core_PIF_Use);
    if (!usePifROM)
    {
        return;
    }

    const SettingsID settingsIds[] =
    {
        SettingsID::Core_PIF_NTSC,
        SettingsID::Core_PIF_PAL,
    };

    std::string rom = CoreSettingsGetStringValue(settingsIds[static_cast<int>(romHeader.SystemType)]);
    if (!std::filesystem::is_regular_file(rom))
    {
        return;
    }

    std::vector<char> buffer;
    if (!CoreReadFile(rom, buffer))
    {
        return;
    }

    ret = m64p::Core.DoCommand(M64CMD_PIF_OPEN, buffer.size(), buffer.data());
    if (ret != M64ERR_SUCCESS)
    {
        error = "open_pif_rom m64p::Core.DoCommand(M64CMD_PIF_OPEN) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }
}

//
// Exported Functions
//

CORE_EXPORT bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom,
    std::string address, int port, int player)
{
    std::string error;
    m64p_error  m64p_ret;
    bool        netplay_ret = false;
    CoreRomType type;
    bool        netplay = !address.empty();

#ifdef NETPLAY
    // Apply RSP plugin override and reload plugins BEFORE ROM open
    if (netplay && address == "KAILLERA")
    {
        apply_kaillera_rsp_override();
        CoreApplyPluginSettings();  // Force reload with HLE RSP
    }
#endif

    if (!CoreOpenRom(n64rom))
    {
        return false;
    }

    if (!CoreApplyRomPluginSettings())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreArePluginsReady())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreAttachPlugins())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (netplay)
    { // netplay cheats
        if (!CoreApplyNetplayCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }
    else
    { // local cheats
        if (!CoreApplyCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }

    if (!CoreGetRomType(type))
    {
        CoreClearCheats();
        CoreDetachPlugins();
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    // set disk file in media loader when ROM is a cartridge
    if (type == CoreRomType::Cartridge)
    {
        CoreMediaLoaderSetDiskFile(n64ddrom);
    }

    // apply core settings overlay
    apply_coresettings_overlay();

    // apply game core settings overrides
    apply_game_coresettings_overlay();

    // apply pif rom settings
    apply_pif_rom_settings();

#ifdef NETPLAY
    // Apply deterministic settings AFTER all overlays for Kaillera netplay
    // This ensures user/game-specific settings don't override critical sync settings
    if (netplay && address == "KAILLERA")
    {
        apply_kaillera_deterministic_settings();
    }
#ifdef FRAME_ZERO
    // Same gates apply to Frame Zero rollback. Active session => apply.
    if (CoreGetFrameZeroSessionMode() != CoreFrameZero::SessionMode::None)
    {
        apply_frame_zero_deterministic_settings();
    }
#endif

    // Kaillera connection happens BEFORE emulation via kailleraSelectServerDialog
    // Just verify it's initialized if netplay was requested
    if (netplay)
    {
        // Check if address is "KAILLERA" marker (set by UI when using Kaillera)
        if (address == "KAILLERA")
        {
            if (!CoreHasInitKaillera())
            {
                CoreSetError("CoreStartEmulation: Kaillera not initialized");
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                // Store player number for input plugin to use
                CoreSetKailleraPlayerNumber(player);
                netplay_ret = true;
            }
        }
        else
        {
            // Legacy netplay (Mupen64Plus built-in)
            netplay_ret = CoreInitNetplay(address, port, player);
            if (!netplay_ret)
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
            }
        }
    }
#endif // NETPLAY

    // only start emulation when initializing netplay
    // is successful or if there's no netplay requested
    if (!netplay || netplay_ret)
    {
        // Register frame callback for frame counter (used by Kaillera)
        s_CurrentFrame = 0;
        m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)FrameCallback);

        // Frame Zero Phase 0.5 spike (no-op unless FRAME_ZERO_SPIKE=1).
        // Must run after Core handle is valid and before M64CMD_EXECUTE.
        SpikeArm();

        // Frame Zero Phase 1 roundtrip test (no-op unless FRAME_ZERO_ROUNDTRIP=1).
        RoundtripArm();

        // Frame Zero Phase 3 stress test (no-op unless FRAME_ZERO_STRESS=N).
        StressArm();

#ifdef NETPLAY
        // Reset Kaillera sync state to prevent stale cache from previous sessions
        s_LastSyncFrame = -1;
        s_SyncedThisFrame = false;
        s_CachedNumReceived = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            s_CachedSyncBuffer[i] = 0;
        }
#endif

#ifdef NETPLAY
        // Register Kaillera PIF sync callback (works with any input plugin).
        // Frame Zero installs its own callback when CoreStartFrameZeroStressSession
        // is called; if a Frame Zero session is already active we leave that
        // callback in place rather than overriding it with Kaillera's.
        bool fz_owns_pif = false;
#ifdef FRAME_ZERO
        fz_owns_pif = (CoreGetFrameZeroSessionMode() != CoreFrameZero::SessionMode::None);
#endif
        if (!fz_owns_pif)
        {
            // Get function pointer dynamically since mupen64plus is loaded at runtime
            typedef void (*set_pif_sync_callback_t)(pif_sync_callback_t);
            void* coreHandle = m64p::Core.GetHandle();
            if (coreHandle)
            {
#ifdef _WIN32
                set_pif_sync_callback_t set_callback =
                    (set_pif_sync_callback_t)GetProcAddress((HMODULE)coreHandle, "set_pif_sync_callback");
#else
                set_pif_sync_callback_t set_callback =
                    (set_pif_sync_callback_t)dlsym(coreHandle, "set_pif_sync_callback");
#endif
                if (set_callback)
                {
                    set_callback(KailleraPifSyncCallback);
                }
            }
        }
#endif

        m64p_ret = m64p::Core.DoCommand(M64CMD_EXECUTE, 0, nullptr);
        if (m64p_ret != M64ERR_SUCCESS)
        {
            error = "CoreStartEmulation m64p::Core.DoCommand(M64CMD_EXECUTE) Failed: ";
            error += m64p::Core.ErrorMessage(m64p_ret);
        }
    }

#ifdef NETPLAY
    if (netplay && netplay_ret)
    {
        // Check if we used Kaillera or legacy netplay
        if (address == "KAILLERA")
        {
            // Don't shutdown Kaillera here - keep connection alive for restart
            // Kaillera will be shutdown when user leaves the server dialog
        }
        else
        {
            CoreShutdownNetplay();
        }
    }
#endif // NETPLAY

    CoreClearCheats();
    CoreDetachPlugins();
    CoreCloseRom();

    // restore plugin settings
    CoreApplyPluginSettings();

    // reset media loader state
    CoreResetMediaLoader();

    if (!netplay || netplay_ret)
    {
        // we need to set the emulation error last,
        // to prevent the other functions from
        // overriding the emulation error
        CoreSetError(error);
    }

    return m64p_ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STOP, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreStopEmulation m64p::Core.DoCommand(M64CMD_STOP) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

#ifdef NETPLAY
    // Clear Kaillera player number when stopping
    CoreSetKailleraPlayerNumber(0);
#endif

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CorePauseEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreHasInitNetplay() || (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode()))
    {
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CorePauseEmulation Failed: ";
        error += "cannot pause emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_PAUSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CorePauseEmulation m64p::Core.DoCommand(M64CMD_PAUSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResumeEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreHasInitNetplay() || (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode()))
    {
        return false;
    }

    if (!CoreIsEmulationPaused())
    {
        error = "CoreIsEmulationPaused Failed: ";
        error += "cannot resume emulation when emulation isn't paused!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESUME, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResumeEmulation m64p::Core.DoCommand(M64CMD_RESUME) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreAdvanceFrame(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreHasInitNetplay() || (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode()))
    {
        return false;
    }

    if (!CoreIsEmulationRunning() && !CoreIsEmulationPaused())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_ADVANCE_FRAME, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreAdvanceFrame DoCommand(M64CMD_ADVANCE_FRAME) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResetEmulation(bool hard)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsEmulationPaused())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when paused!";
        CoreSetError(error);
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESET, hard, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResetEmulation m64p::Core.DoCommand(M64CMD_RESET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreIsEmulationRunning(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_RUNNING;
}

CORE_EXPORT bool CoreIsEmulationPaused(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_PAUSED;
}

CORE_EXPORT int CoreGetCurrentFrameCount(void)
{
    // Return frame counter updated via frame callback
    return s_CurrentFrame;
}
