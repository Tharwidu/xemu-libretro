/*
 * xemu libretro core - Core Options v2
 *
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef LIBRETRO_CORE_OPTIONS_H
#define LIBRETRO_CORE_OPTIONS_H

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

struct retro_core_option_v2_category option_cats_us[] = {
    {
        "system",
        "System",
        "Configure system-level settings such as BIOS paths, memory, and boot options."
    },
    {
        "video",
        "Video",
        "Configure video rendering settings."
    },
    {
        "audio",
        "Audio",
        "Configure audio emulation settings."
    },
    { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
    /* NOTE: File path options (bootrom, bios, hdd, eeprom) are not exposed
     * as core options because they require text input which core options
     * don't support (dropdown only). Paths are auto-detected from
     * RetroArch's system directory instead. */
    {
        "xemu_memory",
        "System Memory (MB)",
        NULL,
        "Amount of system RAM. 64 MB is standard; 128 MB is used by debug kits. Requires restart.",
        NULL,
        "system",
        {
            { "64",  "64 MB" },
            { "128", "128 MB" },
            { NULL, NULL },
        },
        "64"
    },
    {
        "xemu_network_backend",
        "Network Backend",
        NULL,
        "Enable network support for Xbox games with LAN/System Link features. NAT backend provides basic connectivity. Requires restart.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "nat",      "NAT" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_skip_boot_anim",
        "Skip Boot Animation",
        NULL,
        "Skip the Xbox boot animation. Requires restart.",
        NULL,
        "system",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_hard_fpu",
        "Hardware FPU Emulation",
        NULL,
        "Use host FPU for x87 emulation. Faster but may have minor inaccuracies. Requires restart.",
        NULL,
        "system",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "xemu_surface_scale",
        "Internal Resolution Scale",
        NULL,
        "Scales the internal rendering resolution. Higher values improve quality but reduce performance.",
        NULL,
        "video",
        {
            { "1", "1x (640x480)" },
            { "2", "2x (1280x960)" },
            { "3", "3x (1920x1440)" },
            { "4", "4x (2560x1920)" },
            { "5", "5x (3200x2400)" },
            { "6", "6x (3840x2880)" },
            { "7", "7x (4480x3360)" },
            { "8", "8x (5120x3840)" },
            { "9", "9x (5760x4320)" },
            { "10", "10x (6400x4800)" },
            { NULL, NULL },
        },
        "1"
    },
    {
        "xemu_avpack",
        "AV Pack",
        NULL,
        "Select the AV pack type. Affects available video modes. Requires restart.",
        NULL,
        "video",
        {
            { "hdtv",      "HDTV" },
            { "composite", "Composite" },
            { "svideo",    "S-Video" },
            { "scart",     "SCART" },
            { "vga",       "VGA" },
            { NULL, NULL },
        },
        "hdtv"
    },
    {
        "xemu_console_language",
        "Console Language (EEPROM)",
        NULL,
        "Language the emulated console reports to games, stored in the console's EEPROM. 'Auto' leaves whatever is already there, including a value you set in the Xbox Dashboard. 'Follow Frontend' uses RetroArch's language setting. Requires restart.",
        NULL,
        "system",
        {
            { "auto",       "Auto (leave unchanged)" },
            { "frontend",   "Follow Frontend" },
            { "english",    "English" },
            { "japanese",   "Japanese" },
            { "german",     "German" },
            { "french",     "French" },
            { "spanish",    "Spanish" },
            { "italian",    "Italian" },
            { "korean",     "Korean" },
            { "chinese",    "Chinese (Traditional)" },
            { "portuguese", "Portuguese" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "xemu_console_video_standard",
        "Console Video Standard (EEPROM)",
        NULL,
        "TV standard the emulated console is wired for, stored in the console's EEPROM. PAL-I runs PAL titles at their native 50 Hz. 'Auto' leaves whatever is already there. Requires restart.",
        NULL,
        "system",
        {
            { "auto",   "Auto (leave unchanged)" },
            { "ntsc-m", "NTSC-M (North America)" },
            { "ntsc-j", "NTSC-J (Japan)" },
            { "pal-i",  "PAL-I (Europe)" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "xemu_console_widescreen",
        "Console Widescreen (EEPROM)",
        NULL,
        "Whether the emulated console thinks it is connected to a widescreen TV, stored in the console's EEPROM. Games read this to choose a 16:9 or 4:3 presentation, and the core reports the matching aspect ratio to the frontend. 'Auto' leaves whatever is already there. Requires restart.",
        NULL,
        "video",
        {
            { "auto", "Auto (leave unchanged)" },
            { "on",   "16:9 Widescreen" },
            { "off",  "4:3 Standard" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "xemu_use_dsp",
        "APU DSP Emulation",
        NULL,
        "Enable DSP emulation for improved audio accuracy. May impact performance.",
        NULL,
        "audio",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL },
        },
        "disabled"
    },
    {
        "xemu_cache_shaders",
        "Shader Cache",
        NULL,
        "Cache compiled GPU shaders to disk. Reduces stutter on subsequent runs. OpenGL renderer only - the Vulkan renderer has no on-disk shader cache, so this setting is hidden and does nothing while Vulkan is selected.",
        NULL,
        "video",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "xemu_audio_volume",
        "Audio Volume Limit (%)",
        NULL,
        "Limit Xbox audio output volume independently from RetroArch master volume.",
        NULL,
        "audio",
        {
            { "100", "100%" },
            { "80",  "80%" },
            { "60",  "60%" },
            { "40",  "40%" },
            { "20",  "20%" },
            { "0",   "Muted" },
            { NULL, NULL },
        },
        "100"
    },
    {
        "xemu_renderer",
        "Renderer",
        NULL,
        "Which NV2A renderer the emulator uses internally, as in standalone xemu. 'Auto' selects Vulkan in software frame output mode, where it is measurably faster on demanding titles, and follows the frontend's hardware-render context otherwise. Both renderers work in either frame output mode; if a renderer cannot be used the core falls back to the other and says why in the log. Save states cannot be moved between renderers - loading one made with the other is refused. Requires restart.",
        NULL,
        "video",
        {
            { "auto",   "Auto" },
            { "opengl", "OpenGL" },
            { "vulkan", "Vulkan" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "xemu_frame_output",
        "Frame Output Mode",
        NULL,
        "How frames reach the frontend. 'Auto' uses software readback (memory frames) for maximum compatibility across RetroArch builds, GL drivers and OSes, and is what EmuVR's capture needs. 'Hardware (FBO)' is a faster direct-blit path, but only safe on a modern RetroArch/GL stack that keeps its shared GL context alive (older builds such as 1.7.5 can crash the render thread). Readback costs a little performance. Requires restart.",
        NULL,
        "video",
        {
            { "auto",     "Auto" },
            { "hardware", "Hardware (FBO)" },
            { "software", "Software (readback)" },
            { NULL, NULL },
        },
        "auto"
    },
    /* No "Display Filtering" option here on purpose. g_config.display.filtering
     * is read only by ui/xui/gl-helpers.cc, which is imgui UI code the libretro
     * build does not compile, so the setting did nothing at all. Scaling the
     * delivered frame is the frontend's job - RetroArch's own video smoothing
     * setting - and a core option that silently does nothing is worse than an
     * absent one. */
    { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

struct retro_core_options_v2 options_us = {
    option_cats_us,
    option_defs_us,
};

static void libretro_set_core_options(retro_environment_t environ_cb)
{
    unsigned version = 0;

    if (!environ_cb)
        return;

    if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
        version = 0;

    if (version >= 2) {
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
    } else if (version >= 1) {
        /* Fallback: convert v2 to v1 format */
        /* For simplicity, just set the v2 options and let the frontend handle it */
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
    } else {
        /* Legacy v0 fallback (RetroArch <= 1.7.7, incl. EmuVR's 1.7.5):
         * convert v2 definitions to SET_VARIABLES format
         * "Label; default_value|value1|value2|..." (default listed first). */
        #define XEMU_NUM_OPTS (sizeof(option_defs_us) / sizeof(option_defs_us[0]))
        static struct retro_variable vars[XEMU_NUM_OPTS];
        static char var_bufs[XEMU_NUM_OPTS][1024];
        size_t out = 0;

        for (size_t i = 0; option_defs_us[i].key; i++) {
            const struct retro_core_option_v2_definition *def = &option_defs_us[i];
            char *buf = var_bufs[out];
            size_t pos = (size_t)snprintf(buf, sizeof(var_bufs[out]), "%s; ",
                                          def->desc ? def->desc : def->key);

            /* default value first */
            if (def->default_value && pos < sizeof(var_bufs[out])) {
                pos += (size_t)snprintf(buf + pos, sizeof(var_bufs[out]) - pos,
                                        "%s", def->default_value);
            }
            for (size_t v = 0; def->values[v].value &&
                               v < RETRO_NUM_CORE_OPTION_VALUES_MAX; v++) {
                if (def->default_value &&
                    !strcmp(def->values[v].value, def->default_value)) {
                    continue;
                }
                if (pos < sizeof(var_bufs[out])) {
                    pos += (size_t)snprintf(buf + pos, sizeof(var_bufs[out]) - pos,
                                            "|%s", def->values[v].value);
                }
            }

            vars[out].key = def->key;
            vars[out].value = buf;
            out++;
        }
        vars[out].key = NULL;
        vars[out].value = NULL;
        #undef XEMU_NUM_OPTS

        environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_CORE_OPTIONS_H */
