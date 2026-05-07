/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - main.h                                                  *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2002 Blight                                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef __MAIN_H__
#define __MAIN_H__

#include <stdint.h>

#include "api/m64p_types.h"
#include "main/cheat.h"
#include "device/device.h"
#include "osal/preproc.h"

#if defined(__GNUC__)
#define ATTR_FMT(fmtpos, attrpos) __attribute__ ((format (printf, fmtpos, attrpos)))
#else
#define ATTR_FMT(fmtpos, attrpos)
#endif

/* globals */
extern m64p_handle g_CoreConfig;

extern int g_RomWordsLittleEndian;
extern int g_EmulatorRunning;
extern int g_rom_pause;

/* Frame Zero rollback mode flag — see main.c. */
extern int g_RollbackMode;

extern struct cheat_ctx g_cheat_ctx;

extern void* g_mem_base;

extern struct device g_dev;

extern m64p_media_loader g_media_loader;

extern m64p_frame_callback g_FrameCallback;

extern int g_gs_vi_counter;

const char* get_savestatepath(void);
const char* get_savesrampath(void);
const char* get_savestatefilename(void);

void new_frame(void);
void new_vi(void);

void main_switch_next_pak(int control_id);
void main_switch_plugin_pak(int control_id);
void main_change_gb_cart(int control_id);

int  main_set_core_defaults(void);
void main_message(m64p_msg_level level, unsigned int osd_corner, const char *format, ...) ATTR_FMT(3, 4);

m64p_error main_run(void);
void main_stop(void);
void main_toggle_pause(void);
void main_advance_one(void);

/* Frame Zero: silent step + rollback flag accessors.
 * Exported so RMG-Core can dlsym them. */
EXPORT void CALL advance_one_frame_silent(void);
EXPORT void CALL core_set_rollback_mode(int mode);
EXPORT int  CALL core_get_rollback_mode(void);
EXPORT void CALL core_set_input_neutralize(int v);
EXPORT int  CALL core_get_input_neutralize(void);

/* Frame Zero: per-frame pump callback fired from new_frame() on the
 * emulation thread. RMG-Core sets this to drain GekkoNet events at
 * each frame boundary. NULL means "no Frame Zero session active".
 * Superseded by the pump-thread architecture (core_set_pump_driven)
 * but kept for forward-compat — set to NULL when not in use. */
typedef void (*m64p_frame_zero_pump)(unsigned int current_frame);
EXPORT void CALL core_set_frame_zero_pump(m64p_frame_zero_pump cb);

/* Frame Zero Phase 3.5 — pump-driven mode + park primitives.
 *
 * When pump-driven mode is enabled, new_frame() blocks at the frame
 * boundary on a condition variable; an external "pump" thread wakes
 * it for each frame. This lets the pump drive multi-frame inline
 * rollback re-simulation that GekkoNet's update_session expects.
 *
 * Lifecycle:
 *   1. RMG-Core spawns a pump thread, calls core_set_pump_driven(1)
 *   2. Pump thread waits on core_wait_for_park()
 *   3. Emulation thread reaches new_frame, parks
 *   4. Pump thread wakes, processes events synchronously
 *   5. For each Advance, pump calls core_resume_emulation_one_frame()
 *      then core_wait_for_park() to drive exactly one frame
 *   6. Shutdown: core_signal_pump_shutdown() unblocks both sides;
 *      RMG-Core joins pump thread; calls core_set_pump_driven(0). */
EXPORT void CALL core_set_pump_driven(int v);
EXPORT void CALL core_resume_emulation_one_frame(void);
EXPORT int  CALL core_wait_for_park(void);
EXPORT void CALL core_signal_pump_shutdown(void);
EXPORT void CALL core_clear_pump_shutdown(void);

/* Frame Zero IPC primitives — direct (release-safe) RDRAM access.
 * `address` is an N64 virtual address; only the low 24 bits index
 * into RDRAM. Used to read commands written by patched SSB64 code at
 * a magic RDRAM word and to ack them by clearing.
 * Unlike DebugMemRead32/Write32, these are not gated on `#ifdef DBG`
 * and work in release builds. */
EXPORT uint32_t CALL core_read_rdram_word(uint32_t address);
EXPORT void     CALL core_write_rdram_word(uint32_t address, uint32_t value);

/* Read by input_plugin_compat.c. */
extern int g_FrameZero_NeutralizeInput;

void main_speedup(int percent);
void main_speeddown(int percent);
void main_set_fastforward(int enable);
void main_speedlimiter_toggle(void);

void main_take_next_screenshot(void);

void main_state_set_slot(int slot);
void main_state_inc_slot(void);
void main_state_load(const char *filename);
void main_state_save(int format, const char *filename);

m64p_error main_core_state_query(m64p_core_param param, int *rval);
m64p_error main_core_state_set(m64p_core_param param, int val);

m64p_error main_get_screen_size(int *width, int *height);
m64p_error main_read_screen(void *pixels, int bFront);

m64p_error main_volume_up(void);
m64p_error main_volume_down(void);
m64p_error main_volume_get_level(int *level);
m64p_error main_volume_set_level(int level);
m64p_error main_volume_mute(void);
int        main_volume_get_muted(void);

m64p_error main_reset(int do_hard_reset);

m64p_error open_pif(const unsigned char* pifimage, unsigned int size);
m64p_error close_pif(void);

#endif /* __MAIN_H__ */

