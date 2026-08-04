/*
 * xemu libretro core - guest-persistent settings (EEPROM)
 *
 * The Xbox keeps its user settings - language, video standard, widescreen,
 * audio mode - in the console's EEPROM, not in xemu's own config. On real
 * hardware the Dashboard writes them. xemu generates an EEPROM on first run
 * with hardcoded North-America / NTSC-M / English values and never revisits
 * it, so those settings have been unreachable from the frontend.
 *
 * This applies core-option overrides to the EEPROM file before the machine
 * starts. Anything left on "auto" is not touched, so a value the user set in
 * the Dashboard survives - the EEPROM is the guest's own state, and silently
 * rewriting it every launch would clobber their choice.
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

#include "qemu/osdep.h"
#include "hw/xbox/eeprom_generation.h"
#include "ui/libretro-internal.h"

/*
 * Offsets within XboxEEPROM.user_section (which begins at file offset 0x64),
 * from the documented Xbox EEPROM layout. The generator's own language write
 * at +0x2C corroborates the alignment: it emits 44 bytes of timezone data and
 * then sets the next field, which the layout names LanguageID at 0x90.
 */
#define EEPROM_USER_LANGUAGE    0x2C
#define EEPROM_USER_VIDEO_FLAGS 0x30
#define EEPROM_USER_AUDIO_FLAGS 0x34

/* Bytes of each section covered by its checksum, matching the generator. */
#define EEPROM_IDENTITY_CRC_LEN 0x2C
#define EEPROM_USER_CRC_LEN     0x5C

#define XC_VIDEO_FLAGS_WIDESCREEN 0x00010000

static uint32_t eeprom_get_u32(const uint8_t *p)
{
    return le32_to_cpu(*(const uint32_t *)p);
}

static void eeprom_set_u32(uint8_t *p, uint32_t v)
{
    *(uint32_t *)p = cpu_to_le32(v);
}

/* Read the console's configured TV standard. Returns 0 if it cannot be
 * determined, in which case callers should assume NTSC. */
uint32_t libretro_eeprom_read_video_standard(const char *path)
{
    XboxEEPROM e;
    FILE *f;

    if (!path || !path[0]) {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t got = fread(&e, 1, sizeof(e), f);
    fclose(f);
    if (got != sizeof(e)) {
        return 0;
    }
    return le32_to_cpu(e.video_standard);
}

bool libretro_eeprom_apply(const char *path,
                           const LibretroEepromSettings *settings,
                           char *err, size_t err_size)
{
    XboxEEPROM e;
    FILE *f;

    if (!path || !path[0] || !settings) {
        return true;
    }

    /* Nothing requested: do not even open the file. */
    if (settings->language < 0 && settings->video_standard == 0 &&
        settings->widescreen < 0) {
        return true;
    }

    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "cannot open EEPROM");
        return false;
    }
    size_t got = fread(&e, 1, sizeof(e), f);
    fclose(f);
    if (got != sizeof(e)) {
        snprintf(err, err_size, "EEPROM is %zu bytes (expected %zu)",
                 got, sizeof(e));
        return false;
    }

    bool user_changed = false;
    bool identity_changed = false;

    if (settings->language > 0) {
        uint8_t *p = e.user_section + EEPROM_USER_LANGUAGE;
        if (eeprom_get_u32(p) != (uint32_t)settings->language) {
            eeprom_set_u32(p, (uint32_t)settings->language);
            user_changed = true;
        }
    }

    if (settings->widescreen >= 0) {
        uint8_t *p = e.user_section + EEPROM_USER_VIDEO_FLAGS;
        uint32_t flags = eeprom_get_u32(p);
        uint32_t want = settings->widescreen
                            ? (flags | XC_VIDEO_FLAGS_WIDESCREEN)
                            : (flags & ~(uint32_t)XC_VIDEO_FLAGS_WIDESCREEN);
        if (want != flags) {
            eeprom_set_u32(p, want);
            user_changed = true;
        }
    }

    /* video_standard lives outside user_section and is covered by the
     * identity checksum instead. It is not inside the RC4-encrypted block -
     * that covers confounder, hdd_key and region only - so it can be
     * rewritten without touching the security section. */
    if (settings->video_standard != 0) {
        if (le32_to_cpu(e.video_standard) != settings->video_standard) {
            e.video_standard = cpu_to_le32(settings->video_standard);
            identity_changed = true;
        }
    }

    if (!user_changed && !identity_changed) {
        return true;
    }

    if (user_changed) {
        e.user_checksum =
            cpu_to_le32(xbox_eeprom_crc(e.user_section, EEPROM_USER_CRC_LEN));
    }
    if (identity_changed) {
        e.checksum =
            cpu_to_le32(xbox_eeprom_crc(e.serial, EEPROM_IDENTITY_CRC_LEN));
    }

    f = fopen(path, "r+b");
    if (!f) {
        snprintf(err, err_size, "EEPROM is not writable");
        return false;
    }
    bool ok = fwrite(&e, sizeof(e), 1, f) == 1;
    fclose(f);
    if (!ok) {
        snprintf(err, err_size, "failed to write EEPROM");
        return false;
    }

    return true;
}
