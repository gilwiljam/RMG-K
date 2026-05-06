/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - savestates.h                                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2009 Olejl Tillin9                                      *
 *   Copyright (C) 2008 Richard42 Tillin9                                  *
 *   Copyright (C) 2002 Hacktarux                                          *
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

#ifndef __SAVESTAVES_H__
#define __SAVESTAVES_H__

#include <stddef.h>
#include <stdint.h>

#include "api/m64p_types.h"

typedef enum _savestates_job
{
    savestates_job_nothing,
    savestates_job_load,
    savestates_job_save
} savestates_job;

typedef enum _savestates_type
{
    savestates_type_unknown,
    savestates_type_m64p,
    savestates_type_pj64_zip,
    savestates_type_pj64_unc
} savestates_type;

savestates_job savestates_get_job(void);
void savestates_set_job(savestates_job j, savestates_type t, const char *fn);
void savestates_init(void);
void savestates_deinit(void);

int savestates_load(void);
int savestates_save(void);

/* Frame Zero buffer-based savestate API.
 * Synchronous, no compression, no file I/O. Used for rollback snapshots.
 * Buffer layout matches what savestates_save_m64p() builds in memory. */

/* Returns the size in bytes that savestates_save_to_buffer will produce. */
EXPORT size_t CALL savestates_get_state_size(void);

/* Captures full m64p savestate to a freshly malloc'd buffer.
 * Caller takes ownership of *out_buf and must free() it.
 * Returns 1 on success, 0 on failure. */
EXPORT int CALL savestates_save_to_buffer(uint8_t** out_buf, size_t* out_len);

/* Applies a savestate previously produced by savestates_save_to_buffer.
 * Skips MD5 check (caller is responsible for ensuring same ROM).
 * Returns 1 on success, 0 on failure. */
EXPORT int CALL savestates_load_from_buffer(const uint8_t* buf, size_t len);

void savestates_select_slot(unsigned int s);
unsigned int savestates_get_slot(void);
void savestates_set_autoinc_slot(int b);
void savestates_inc_slot(void);

#endif /* __SAVESTAVES_H__ */

