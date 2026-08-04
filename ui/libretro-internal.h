/*
 * xemu libretro core - internal glue declarations
 *
 * Copyright (c) 2026 Tharwidu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UI_LIBRETRO_INTERNAL_H
#define UI_LIBRETRO_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Declarations shared between ui/libretro.c and the stub translation units
 * that stand in for xemu's SDL/ImGui frontend. Not part of the libretro API;
 * ui/libretro.h is the vendored upstream libretro header.
 */

/* ui/libretro-settings-stub.c: point the settings layer at the directory the
 * frontend gave us, in place of xemu's own config discovery. */
void libretro_settings_set_base_path(const char *path);

/* ui/libretro-stubs.c: attach the emulated Xbox controllers. */
void libretro_input_create_xid_devices(void);

/* hw/xbox/mcpx/apu/monitor.c: pull decoded audio from the APU ring buffer.
 * Returns the number of frames written to out_buf. */
int libretro_audio_pull(int16_t *out_buf, int max_frames);

/* hw/xbox/mcpx/apu/monitor.c: frames currently queued in the APU ring. */
int libretro_audio_ring_frames(void);

/* hw/xbox/mcpx/apu/monitor.c: drop everything queued in the APU ring. Used
 * once at first active pull to discard stale boot audio. */
void libretro_audio_flush(void);

/* ui/libretro-eeprom.c: guest-persistent console settings.
 *
 * The EEPROM is the guest's own state, written by the Xbox Dashboard on real
 * hardware, so every field here is opt-in: leave it at the "unset" value and
 * the field is not touched. Silently rewriting on every launch would clobber
 * what the user chose in the Dashboard. */
typedef struct LibretroEepromSettings {
    int language;        /* Xbox LanguageID (1 = English); <0 leaves it */
    uint32_t video_standard; /* XC_VIDEO_STANDARD_*; 0 leaves it */
    int widescreen;      /* 1 on, 0 off, <0 leaves it */
} LibretroEepromSettings;

/* Apply the requested overrides to the EEPROM at `path`, recomputing whichever
 * checksums the change invalidates. Returns false and fills `err` on failure;
 * a no-op request succeeds without opening the file. */
/* Xbox TV standard identifiers as stored in the EEPROM. */
#define XC_VIDEO_STANDARD_NTSC_M 0x00400100u
#define XC_VIDEO_STANDARD_NTSC_J 0x00400200u
#define XC_VIDEO_STANDARD_PAL_I  0x00800300u

/* Read the console's configured TV standard; 0 if unknown (assume NTSC). */
uint32_t libretro_eeprom_read_video_standard(const char *path);

bool libretro_eeprom_apply(const char *path,
                           const LibretroEepromSettings *settings,
                           char *err, size_t err_size);

#endif /* UI_LIBRETRO_INTERNAL_H */
