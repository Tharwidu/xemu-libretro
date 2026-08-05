/*
 * xemu libretro core wrapper
 *
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "migration/snapshot.h"
#include "hw/xbox/eeprom_generation.h"
#include "hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/gloffscreen_libretro.h"
#include "hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/gloffscreen.h"
#include "ui/libretro-internal.h"
#include "ui/xemu-widescreen.h"
#include "crypto/init.h"
#include "ui/console.h"
#include "hw/xbox/nv2a/nv2a.h"

#include <epoxy/gl.h>
#if defined(_WIN32)
#include <epoxy/wgl.h>
#endif

#include "libretro.h"
#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "libretro_vulkan.h"
#include "libretro_core_options.h"
#include "xemu-settings.h"
#include "xemu-version.h"

/* ========================================================================= */
/* Libretro callback storage                                                 */
/* ========================================================================= */

static retro_environment_t        environ_cb   = NULL;
static retro_video_refresh_t      video_cb     = NULL;
static retro_audio_sample_t       audio_cb     = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t         input_poll_cb  = NULL;
retro_input_state_t               input_state_cb = NULL; /* non-static: used by libretro-stubs.c */
static retro_log_printf_t         log_cb       = NULL;

/* ========================================================================= */
/* Libretro log wrapper - use this instead of fprintf(stderr, ...)           */
/* ========================================================================= */

#define LRLOG_INFO(...)  do { if (log_cb) log_cb(RETRO_LOG_INFO,  __VA_ARGS__); } while(0)
#define LRLOG_WARN(...)  do { if (log_cb) log_cb(RETRO_LOG_WARN,  __VA_ARGS__); } while(0)
#define LRLOG_ERROR(...) do { if (log_cb) log_cb(RETRO_LOG_ERROR, __VA_ARGS__); } while(0)
#define LRLOG_DEBUG(...) do { if (log_cb) log_cb(RETRO_LOG_DEBUG, __VA_ARGS__); } while(0)

/* Log bridge for the gloffscreen backends: their stderr output never
 * reaches RetroArch's --log-file, so route their diagnostics through the
 * frontend log callback (thread-safe: RA's log callback is). */
void libretro_glo_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LRLOG_INFO("%s\n", buf);
}

/* ========================================================================= */
/* VFS interface (optional)                                                  */
/* ========================================================================= */

static struct retro_vfs_interface *vfs_interface = NULL;

/* ========================================================================= */
/* Hardware rendering state                                                  */
/* ========================================================================= */

static struct retro_hw_render_callback hw_render;
static bool use_vulkan = false;
static bool context_ready = false;
static bool game_loaded = false;

/* GL blit shader state */
static GLuint blit_program = 0;
static GLuint blit_vao = 0;
static GLint  blit_tex_loc = -1;

/* Vulkan HW render interface from RetroArch */
static struct retro_hw_render_interface_vulkan *vulkan_if = NULL;

/* RA-side VK resources: xemu's display image imported into RA's VkDevice */
static VkImage ra_vk_image = VK_NULL_HANDLE;
static VkImageView ra_vk_image_view = VK_NULL_HANDLE;
static VkDeviceMemory ra_vk_memory = VK_NULL_HANDLE;
static uint32_t ra_vk_width = 0, ra_vk_height = 0;
static void *ra_last_handle = NULL;
static struct retro_vulkan_image retro_vk_image;
static VkImageViewCreateInfo ra_vk_view_ci;

/* VK function pointers from RA's device (not xemu's) */
static PFN_vkCreateImage ra_vkCreateImage;
static PFN_vkDestroyImage ra_vkDestroyImage;
static PFN_vkCreateImageView ra_vkCreateImageView;
static PFN_vkDestroyImageView ra_vkDestroyImageView;
static PFN_vkAllocateMemory ra_vkAllocateMemory;
static PFN_vkFreeMemory ra_vkFreeMemory;
static PFN_vkBindImageMemory ra_vkBindImageMemory;
static PFN_vkGetImageMemoryRequirements ra_vkGetImageMemoryRequirements;
static PFN_vkGetPhysicalDeviceMemoryProperties ra_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateCommandPool ra_vkCreateCommandPool;
static PFN_vkDestroyCommandPool ra_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers ra_vkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers ra_vkFreeCommandBuffers;
static PFN_vkBeginCommandBuffer ra_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer ra_vkEndCommandBuffer;
static PFN_vkQueueSubmit ra_vkQueueSubmit;
static PFN_vkQueueWaitIdle ra_vkQueueWaitIdle;
static PFN_vkCmdPipelineBarrier ra_vkCmdPipelineBarrier;
static PFN_vkCreateFence ra_vkCreateFence;
static PFN_vkDestroyFence ra_vkDestroyFence;
static PFN_vkWaitForFences ra_vkWaitForFences;
static PFN_vkResetFences ra_vkResetFences;
static PFN_vkResetCommandBuffer ra_vkResetCommandBuffer;
static VkCommandPool ra_vk_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer ra_vk_cmds[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
static VkFence ra_vk_fences[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
static uint32_t ra_vk_frame_idx = 0;
static bool ra_vk_funcs_resolved = false;

/* Standalone GL for VK renderer's internal GL interop (PFIFO thread) */
typedef struct _GloContext GloContext;

/* ========================================================================= */
/* Emulation state                                                           */
/* ========================================================================= */

static bool emu_initialized = false;
static QemuThread emu_thread;
static volatile bool emu_thread_running = false;

/* Snapshot dispatch: RetroArch thread requests, QEMU thread executes */
enum {
    SNAPSHOT_NONE = 0,
    SNAPSHOT_SAVE,
    SNAPSHOT_LOAD,
};
static volatile int snapshot_request = SNAPSHOT_NONE;
static volatile bool snapshot_done = false;
static volatile bool snapshot_result = false;
static QemuSemaphore snapshot_done_sem;       /* signal RA thread */
static bool snapshot_sem_initialized = false;
static char snapshot_name[32] = "libretro_save";

/* Pause watchdog: monotonic time of the last retro_run call. The emulator
 * otherwise free-runs in real time while the frontend menu is open. */
static volatile int64_t last_retro_run_us;
static bool watchdog_paused;

/* ========================================================================= */
/* Core option values                                                        */
/* ========================================================================= */

static char opt_bootrom_path[4096] = "";
static char opt_bios_path[4096] = "";
static char opt_hdd_path[4096] = "";
static char opt_eeprom_path[4096] = "";
static char opt_dvd_path[4096] = "";
static char system_dir[4096] = "";
static int  opt_memory_mb = 64;
static bool opt_skip_boot_anim = false;
static bool opt_hard_fpu = true;
static bool opt_use_dsp = false;
static int  opt_surface_scale = 1;
static int  opt_avpack = CONFIG_SYS_AVPACK_HDTV;
static bool opt_cache_shaders = true;
static int  opt_filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
static int  opt_audio_volume = 100;
static int  opt_network_backend = 0; /* 0=disabled, 1=nat */
static int  opt_frame_output = 0; /* 0=auto, 1=hardware, 2=software */
/* Which NV2A renderer to use: 0=auto (follow the frontend's context type),
 * 1=force OpenGL, 2=force Vulkan. */
#define RENDERER_AUTO   0
#define RENDERER_OPENGL 1
#define RENDERER_VULKAN 2
static int  opt_renderer = RENDERER_AUTO;
/* EEPROM (guest-persistent) settings; "leave unchanged" by default. */
static int      opt_eeprom_language = -1;
static uint32_t opt_eeprom_video_standard = 0;
static int      opt_eeprom_widescreen = -1;

/* TV standard the console is actually wired for, read back from the EEPROM
 * once the overrides above have been applied. Drives the frame rate we
 * advertise, the region we report, and our own pacer. */
static uint32_t console_video_standard = XC_VIDEO_STANDARD_NTSC_M;

static bool console_is_pal(void)
{
    return console_video_standard == XC_VIDEO_STANDARD_PAL_I;
}

static double console_refresh_hz(void)
{
    return console_is_pal() ? 50.0 : 59.94;
}


/* Build "<system dir>/xemu/<name>" into dst. Returns false (and empties dst)
 * if the result would not fit, so an over-long system directory fails loudly
 * instead of silently yielding a truncated path that later opens the wrong
 * file or none at all. Pass "" for the directory itself. */
static bool build_system_path(char *dst, size_t dst_size, const char *name)
{
    int n = snprintf(dst, dst_size, "%s/xemu/%s", system_dir, name);
    if (n < 0 || (size_t)n >= dst_size) {
        LRLOG_ERROR("[xemu] System path too long: %s/xemu/%s\n",
                    system_dir, name);
        dst[0] = '\0';
        return false;
    }
    return true;
}

/* Optional software frame output (delivers memory frames instead of the
 * hardware FBO; available via the xemu_frame_output core option). */
static bool frame_readback = false;
/* Frontend capabilities, probed once in retro_set_environment. */
/* The Xbox's native output mode. */
#define XBOX_NATIVE_WIDTH  640
#define XBOX_NATIVE_HEIGHT 480

static bool frontend_can_dupe = false;
/* Defined here, read by the input bridge in libretro-stubs.c. */
bool libretro_input_bitmasks = false;
struct retro_rumble_interface libretro_rumble;
/* Size of the last frame actually delivered, so duplicate frames can be
 * announced at the dimensions the frontend already has. */
static unsigned last_frame_width  = XBOX_NATIVE_WIDTH;
static unsigned last_frame_height = XBOX_NATIVE_HEIGHT;
#define READBACK_MAX_W 1920
#define READBACK_MAX_H 1080
static uint32_t readback_frame[READBACK_MAX_W * READBACK_MAX_H];

/* ------------------------------------------------------------------ *
 * Periodic runtime stats.
 *
 * Always on, roughly every ten seconds. The point is that a user's log is
 * useful without them having to reproduce anything under a debug flag -
 * every performance question this core has produced so far ("it felt
 * slow", "it stuttered") has been unattributable because the log said
 * nothing about what the core was doing.
 * ------------------------------------------------------------------ */
#define STATS_INTERVAL_US (10 * 1000 * 1000) /* 10 s */

static struct {
    int64_t window_start_us;   /* when this window opened */
    unsigned frames;           /* retro_run calls */
    unsigned delivered;        /* frames handed to video_cb with pixels */
    unsigned duped;            /* frames skipped via can-dupe */
    unsigned black;            /* no display frame available */
    unsigned src_pgraph, src_vga, src_vk;
    uint64_t capture_us;       /* time inside the frame fetch */
    uint64_t pace_sleep_us;    /* time spent in the pacer */
    unsigned ff_frames;        /* frames while fast-forwarding */
    unsigned audio_frames;     /* audio frames pushed to the frontend */
} stats;

static void stats_report_if_due(void)
{
    int64_t now = g_get_monotonic_time();
    if (stats.window_start_us == 0) {
        /* First call opens the window. Clear the counters too: they have
         * been accumulating since load, and reporting them against a
         * window that starts here would double the rates. */
        memset(&stats, 0, sizeof(stats));
        stats.window_start_us = now;
        return;
    }

    if (now - stats.window_start_us < STATS_INTERVAL_US) {
        return;
    }

    double secs = (double)(now - stats.window_start_us) / 1e6;
    if (secs <= 0.0) {
        secs = 1.0;
    }

    uint64_t vkcopy_us = 0;
    unsigned vkcopy_n = 0;
    if (use_vulkan) {
        nv2a_vk_get_capture_stats(&vkcopy_us, &vkcopy_n);
    }

    LRLOG_INFO("[xemu] stats: %.1f fps (%u frames/%.1fs) | delivered %u "
               "dup %u black %u | src pgraph %u vga %u vk %u | capture "
               "%.2f ms/f | gpucopy %.2f ms x%u | pace %.2f ms/f | ff %u "
               "| audio %u frames (%.0f/s)\n",
               stats.frames / secs, stats.frames, secs,
               stats.delivered, stats.duped, stats.black,
               stats.src_pgraph, stats.src_vga, stats.src_vk,
               stats.frames ? (double)stats.capture_us / stats.frames / 1000.0
                            : 0.0,
               vkcopy_n ? (double)vkcopy_us / vkcopy_n / 1000.0 : 0.0,
               vkcopy_n,
               stats.frames ? (double)stats.pace_sleep_us / stats.frames / 1000.0
                            : 0.0,
               stats.ff_frames, stats.audio_frames,
               stats.audio_frames / secs);

    memset(&stats, 0, sizeof(stats));
    stats.window_start_us = now;
}

/* Tell the frontend the guest's display geometry whenever it changes.
 *
 * The Xbox scales its output in hardware, so the framebuffer's pixel
 * dimensions are NOT its display aspect: a 16:9 title still renders a
 * 640x480 surface. Deriving the ratio from width/height would report
 * 1.333 for every title. The guest instead announces the TV it thinks it
 * is driving by writing the aspect-ratio PM GPIO (hw/xbox/acpi_xbox.c),
 * which xemu tracks in xemu_get_widescreen() - the same source upstream's
 * own "auto" aspect setting uses. It can change at runtime, when the user
 * changes the console's video setting in the dashboard.
 *
 * SET_GEOMETRY is the cheap call: geometry only, no AV re-init, so it is
 * safe to attempt every frame and skip when nothing moved. */
static void update_display_geometry(unsigned width, unsigned height)
{
    static unsigned last_width;
    static unsigned last_height;
    static float last_aspect;

    if (!environ_cb || width == 0 || height == 0) {
        return;
    }

    float aspect = xemu_get_widescreen() ? (16.0f / 9.0f) : (4.0f / 3.0f);

    if (width == last_width && height == last_height &&
        aspect == last_aspect) {
        return;
    }

    struct retro_game_geometry geom;
    memset(&geom, 0, sizeof(geom));
    geom.base_width   = width;
    geom.base_height  = height;
    geom.max_width    = READBACK_MAX_W;
    geom.max_height   = READBACK_MAX_H;
    geom.aspect_ratio = aspect;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom)) {
        /* Frontend predates the call (RA < 1.3). Remember anyway so we do
         * not retry on every frame for the rest of the session. */
        LRLOG_INFO("[xemu] SET_GEOMETRY unsupported by this frontend\n");
    } else {
        LRLOG_INFO("[xemu] Geometry: %ux%u, aspect %.4f (%s)\n",
                   width, height, (double)aspect,
                   xemu_get_widescreen() ? "16:9" : "4:3");
    }

    last_width  = width;
    last_height = height;
    last_aspect = aspect;
}

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */

static void context_reset(void);
static void context_destroy(void);
static void libretro_drain_audio(void);
static void update_variables(void);
static void create_blit_resources(void);
static void destroy_blit_resources(void);
static void blit_nv2a_texture(GLuint tex, unsigned width, unsigned height, uintptr_t fbo);

/* ========================================================================= */
/* GL Blit Shader (fullscreen triangle, Y-flip for libretro top-left origin) */
/* ========================================================================= */

static const char *blit_vs_src =
    "#version 330 core\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "    vTexCoord = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);\n"
    "}\n";

static const char *blit_fs_src =
    "#version 330 core\n"
    "in vec2 vTexCoord;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = texture(uTex, vTexCoord);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char info[512];
        glGetShaderInfoLog(shader, sizeof(info), NULL, info);
        LRLOG_ERROR("[xemu] Shader compile error: %s\n", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void create_blit_resources(void)
{
    LRLOG_INFO("[xemu] Creating GL blit resources\n");

    GLuint vs = compile_shader(GL_VERTEX_SHADER, blit_vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, blit_fs_src);
    if (!vs || !fs) {
        LRLOG_ERROR("[xemu] Failed to compile blit shaders\n");
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    blit_program = glCreateProgram();
    glAttachShader(blit_program, vs);
    glAttachShader(blit_program, fs);
    glLinkProgram(blit_program);

    GLint status = 0;
    glGetProgramiv(blit_program, GL_LINK_STATUS, &status);
    if (!status) {
        char info[512];
        glGetProgramInfoLog(blit_program, sizeof(info), NULL, info);
        LRLOG_ERROR("[xemu] Shader link error: %s\n", info);
        glDeleteProgram(blit_program);
        blit_program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    blit_tex_loc = glGetUniformLocation(blit_program, "uTex");

    glGenVertexArrays(1, &blit_vao);

    LRLOG_INFO("[xemu] GL blit resources created (program=%u, vao=%u)\n",
               blit_program, blit_vao);
}

static void destroy_blit_resources(void)
{
    if (blit_program) {
        glDeleteProgram(blit_program);
        blit_program = 0;
    }
    if (blit_vao) {
        glDeleteVertexArrays(1, &blit_vao);
        blit_vao = 0;
    }
}

static void blit_nv2a_texture(GLuint tex, unsigned width, unsigned height, uintptr_t fbo)
{
    if (!blit_program || !blit_vao || !tex) return;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(blit_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(blit_tex_loc, 0);

    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Clean up state per libretro docs */
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ========================================================================= */
/* Vulkan HW render helpers                                                  */
/* ========================================================================= */

static void ra_vk_resolve_functions(void)
{
    if (!vulkan_if || ra_vk_funcs_resolved) return;

    PFN_vkGetDeviceProcAddr gdpa = vulkan_if->get_device_proc_addr;
    PFN_vkGetInstanceProcAddr gipa = vulkan_if->get_instance_proc_addr;
    VkDevice dev = vulkan_if->device;
    VkInstance inst = vulkan_if->instance;

    ra_vkCreateImage = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
    ra_vkDestroyImage = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
    ra_vkCreateImageView = (PFN_vkCreateImageView)gdpa(dev, "vkCreateImageView");
    ra_vkDestroyImageView = (PFN_vkDestroyImageView)gdpa(dev, "vkDestroyImageView");
    ra_vkAllocateMemory = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
    ra_vkFreeMemory = (PFN_vkFreeMemory)gdpa(dev, "vkFreeMemory");
    ra_vkBindImageMemory = (PFN_vkBindImageMemory)gdpa(dev, "vkBindImageMemory");
    ra_vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)gdpa(dev, "vkGetImageMemoryRequirements");
    ra_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    ra_vkCreateCommandPool = (PFN_vkCreateCommandPool)gdpa(dev, "vkCreateCommandPool");
    ra_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(dev, "vkDestroyCommandPool");
    ra_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(dev, "vkAllocateCommandBuffers");
    ra_vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(dev, "vkFreeCommandBuffers");
    ra_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(dev, "vkBeginCommandBuffer");
    ra_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(dev, "vkEndCommandBuffer");
    ra_vkQueueSubmit = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
    ra_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)gdpa(dev, "vkQueueWaitIdle");
    ra_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(dev, "vkCmdPipelineBarrier");
    ra_vkCreateFence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
    ra_vkDestroyFence = (PFN_vkDestroyFence)gdpa(dev, "vkDestroyFence");
    ra_vkWaitForFences = (PFN_vkWaitForFences)gdpa(dev, "vkWaitForFences");
    ra_vkResetFences = (PFN_vkResetFences)gdpa(dev, "vkResetFences");
    ra_vkResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(dev, "vkResetCommandBuffer");

    ra_vk_funcs_resolved = (ra_vkCreateImage && ra_vkDestroyImage &&
                            ra_vkCreateImageView && ra_vkDestroyImageView &&
                            ra_vkAllocateMemory && ra_vkFreeMemory &&
                            ra_vkBindImageMemory && ra_vkGetImageMemoryRequirements &&
                            ra_vkGetPhysicalDeviceMemoryProperties &&
                            ra_vkCreateCommandPool && ra_vkAllocateCommandBuffers &&
                            ra_vkBeginCommandBuffer && ra_vkEndCommandBuffer &&
                            ra_vkQueueSubmit && ra_vkCmdPipelineBarrier &&
                            ra_vkCreateFence && ra_vkWaitForFences &&
                            ra_vkResetFences && ra_vkResetCommandBuffer);

    if (ra_vk_funcs_resolved && ra_vk_cmd_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool_ci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = vulkan_if->queue_index,
        };
        ra_vkCreateCommandPool(dev, &pool_ci, NULL, &ra_vk_cmd_pool);

        VkCommandBufferAllocateInfo cmd_ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ra_vk_cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        ra_vkAllocateCommandBuffers(dev, &cmd_ai, ra_vk_cmds);

        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        ra_vkCreateFence(dev, &fence_ci, NULL, &ra_vk_fences[0]);
        ra_vkCreateFence(dev, &fence_ci, NULL, &ra_vk_fences[1]);
    }
}

static uint32_t ra_vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    ra_vkGetPhysicalDeviceMemoryProperties(vulkan_if->gpu, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

static void ra_vk_transition_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout)
{
    if (!vulkan_if || !ra_vk_funcs_resolved) return;

    uint32_t idx = ra_vk_frame_idx;
    VkCommandBuffer cmd = ra_vk_cmds[idx];
    VkFence fence = ra_vk_fences[idx];
    if (!cmd || !fence) return;

    VkDevice dev = vulkan_if->device;

    /* Wait for THIS slot's fence (from 2 frames ago — should be instant) */
    ra_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    ra_vkResetFences(dev, 1, &fence);
    ra_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    ra_vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    ra_vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    ra_vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    vulkan_if->lock_queue(vulkan_if->handle);
    ra_vkQueueSubmit(vulkan_if->queue, 1, &submit, fence);
    vulkan_if->unlock_queue(vulkan_if->handle);

    /* Flip to other slot for next frame */
    ra_vk_frame_idx = 1 - idx;
}

static void ra_vk_cleanup_display(void)
{
    if (!vulkan_if) return;
    VkDevice dev = vulkan_if->device;

    if (ra_vk_image_view != VK_NULL_HANDLE) {
        ra_vkDestroyImageView(dev, ra_vk_image_view, NULL);
        ra_vk_image_view = VK_NULL_HANDLE;
    }
    if (ra_vk_image != VK_NULL_HANDLE) {
        ra_vkDestroyImage(dev, ra_vk_image, NULL);
        ra_vk_image = VK_NULL_HANDLE;
    }
    if (ra_vk_memory != VK_NULL_HANDLE) {
        ra_vkFreeMemory(dev, ra_vk_memory, NULL);
        ra_vk_memory = VK_NULL_HANDLE;
    }
    ra_vk_width = 0;
    ra_vk_height = 0;
    ra_last_handle = NULL;
}

static bool ra_vk_import_display(void *ext_handle, int width, int height)
{
#ifndef _WIN32
    /* POSIX: the emulator exports its display allocation as an opaque fd.
     * Unlike a Win32 handle, importing an fd CONSUMES it - the driver takes
     * ownership - so a fresh one is exported per import and never reused. */
    (void)ext_handle;

    if (!vulkan_if || !ra_vk_funcs_resolved || !width || !height) {
        return false;
    }

    VkDevice dev = vulkan_if->device;

    if (ra_vk_image != VK_NULL_HANDLE &&
        (uint32_t)width == ra_vk_width && (uint32_t)height == ra_vk_height) {
        return true; /* already imported at this size */
    }

    ra_vk_cleanup_display();

    int fd = nv2a_vk_export_display_fd();
    if (fd < 0) {
        LRLOG_ERROR("[xemu] Could not export the display memory fd\n");
        return false;
    }

    VkExternalMemoryImageCreateInfo ext_img_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img_ci,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = nv2a_vk_display_uses_optimal_tiling()
                      ? VK_IMAGE_TILING_OPTIMAL
                      : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (ra_vkCreateImage(dev, &img_ci, NULL, &ra_vk_image) != VK_SUCCESS) {
        close(fd);
        LRLOG_ERROR("[xemu] vkCreateImage failed for the imported display\n");
        return false;
    }

    VkMemoryRequirements mem_reqs;
    ra_vkGetImageMemoryRequirements(dev, ra_vk_image, &mem_reqs);

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = fd,
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex =
            ra_vk_find_memory_type(mem_reqs.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VkResult res = ra_vkAllocateMemory(dev, &alloc_info, NULL, &ra_vk_memory);
    if (res != VK_SUCCESS) {
        /* The import did not happen, so the descriptor is still ours. */
        close(fd);
        ra_vkDestroyImage(dev, ra_vk_image, NULL);
        ra_vk_image = VK_NULL_HANDLE;
        LRLOG_ERROR("[xemu] Importing the display fd failed (VkResult %d). "
                    "The frontend's Vulkan device most likely lacks "
                    "VK_KHR_external_memory_fd\n", (int)res);
        return false;
    }
    /* From here the driver owns the fd; do not close it. */

    if (ra_vkBindImageMemory(dev, ra_vk_image, ra_vk_memory, 0) != VK_SUCCESS) {
        ra_vk_cleanup_display();
        LRLOG_ERROR("[xemu] vkBindImageMemory failed for the imported "
                    "display\n");
        return false;
    }

    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ra_vk_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if (ra_vkCreateImageView(dev, &view_ci, NULL, &ra_vk_image_view) !=
        VK_SUCCESS) {
        ra_vk_cleanup_display();
        LRLOG_ERROR("[xemu] vkCreateImageView failed for the imported "
                    "display\n");
        return false;
    }

    ra_vk_view_ci = view_ci;
    ra_vk_width = (uint32_t)width;
    ra_vk_height = (uint32_t)height;
    LRLOG_INFO("[xemu] Imported the display image over an opaque fd "
               "(%dx%d)\n", width, height);
    return true;
#else
    if (!vulkan_if || !ra_vk_funcs_resolved || !ext_handle || !width || !height)
        return false;

    VkDevice dev = vulkan_if->device;

    /* Cleanup old resources if size changed */
    if (ra_vk_image != VK_NULL_HANDLE &&
        ((uint32_t)width != ra_vk_width || (uint32_t)height != ra_vk_height)) {
        ra_vk_cleanup_display();
    }

    /* Already imported this handle at this size */
    if (ra_vk_image != VK_NULL_HANDLE && ext_handle == ra_last_handle)
        return true;

    ra_vk_cleanup_display();

    /* Create VkImage on RA's device with external memory import */
    VkExternalMemoryImageCreateInfo ext_img_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };

    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img_ci,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = nv2a_vk_display_uses_optimal_tiling()
                      ? VK_IMAGE_TILING_OPTIMAL
                      : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = ra_vkCreateImage(dev, &img_ci, NULL, &ra_vk_image);
    if (res != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    ra_vkGetImageMemoryRequirements(dev, ra_vk_image, &mem_reqs);

    /* Import the external memory handle */
    VkImportMemoryWin32HandleInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        .handle = (HANDLE)ext_handle,
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = ra_vk_find_memory_type(mem_reqs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    res = ra_vkAllocateMemory(dev, &alloc_info, NULL, &ra_vk_memory);
    if (res != VK_SUCCESS) {
        ra_vkDestroyImage(dev, ra_vk_image, NULL);
        ra_vk_image = VK_NULL_HANDLE;
        return false;
    }

    res = ra_vkBindImageMemory(dev, ra_vk_image, ra_vk_memory, 0);
    if (res != VK_SUCCESS) {
        ra_vk_cleanup_display();
        return false;
    }

    /* Create VkImageView */
    ra_vk_view_ci = (VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ra_vk_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    res = ra_vkCreateImageView(dev, &ra_vk_view_ci, NULL, &ra_vk_image_view);
    if (res != VK_SUCCESS) {
        ra_vk_cleanup_display();
        return false;
    }

    ra_vk_width = width;
    ra_vk_height = height;
    ra_last_handle = ext_handle;

    return true;
#endif /* _WIN32 */
}

/* ========================================================================= */
/* Context callbacks                                                         */
/* ========================================================================= */

static void context_reset(void)
{
    /* Everything below runs on the frontend's thread with the frontend's
     * GL context current. nv2a_context_init() -> early_context_init()
     * leaves the nv2a *display* context current on this thread; that must
     * be undone before returning:
     *  (a) frontends that bind their GL context once at init (RetroArch
     *      1.7.5's gl driver) would keep rendering on OUR context, and
     *  (b) a WGL/GLX context can only be current on one thread — if the
     *      frontend thread still holds the display context, the PFIFO
     *      thread's later MakeCurrent on it fails and its GL calls land
     *      on the wrong context (access violation on real drivers).
     * Restore happens BEFORE waking the PFIFO thread so the display
     * context is guaranteed unbound before the worker binds it. */
    void *prev_ctx = glo_save_current();

    if (!use_vulkan) {
        LRLOG_INFO("[xemu] OpenGL context ready, creating blit resources\n");
        create_blit_resources();


        libretro_gl_prepare();
        nv2a_context_init();
        glo_restore_current(prev_ctx);
        libretro_gl_wake_pfifo();
    } else {
        if (environ_cb) {
            struct retro_hw_render_interface *iface = NULL;
            if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) && iface &&
                iface->interface_type == RETRO_HW_RENDER_INTERFACE_VULKAN) {
                vulkan_if = (struct retro_hw_render_interface_vulkan *)iface;
                LRLOG_INFO("[xemu] Got Vulkan HW render interface (version %u, device=%p)\n",
                           vulkan_if->interface_version, (void*)vulkan_if->device);
                ra_vk_resolve_functions();
            } else {
                LRLOG_ERROR("[xemu] Failed to get Vulkan HW render interface\n");
                vulkan_if = NULL;
            }
        }


        libretro_gl_set_standalone_mode();
        nv2a_context_init();
        glo_restore_current(prev_ctx);
        libretro_gl_wake_pfifo();
    }

    context_ready = true;
}

static void context_destroy(void)
{
    context_ready = false;

    if (!use_vulkan) {
        destroy_blit_resources();
    }

    if (use_vulkan && vulkan_if) {
        ra_vk_cleanup_display();
        for (int i = 0; i < 2; i++) {
            if (ra_vk_fences[i] != VK_NULL_HANDLE && ra_vkDestroyFence) {
                ra_vkWaitForFences(vulkan_if->device, 1, &ra_vk_fences[i], VK_TRUE, UINT64_MAX);
                ra_vkDestroyFence(vulkan_if->device, ra_vk_fences[i], NULL);
                ra_vk_fences[i] = VK_NULL_HANDLE;
            }
            ra_vk_cmds[i] = VK_NULL_HANDLE;
        }
        ra_vk_frame_idx = 0;
        if (ra_vk_cmd_pool != VK_NULL_HANDLE && ra_vkDestroyCommandPool) {
            ra_vkDestroyCommandPool(vulkan_if->device, ra_vk_cmd_pool, NULL);
            ra_vk_cmd_pool = VK_NULL_HANDLE;
        }
        ra_vk_funcs_resolved = false;
    }
    vulkan_if = NULL;
}

/* ========================================================================= */
/* Core option helpers                                                       */
/* ========================================================================= */

static void get_option_string(const char *key, char *buf, size_t buf_sz)
{
    struct retro_variable var = { key, NULL };
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        snprintf(buf, buf_sz, "%s", var.value);
    }
}

/* Surface fatal setup problems in the frontend's OSD, not just the log. */
static void show_user_message(const char *text)
{
    LRLOG_ERROR("%s\n", text);
    if (environ_cb) {
        struct retro_message msg = { text, 360 }; /* ~6 seconds */
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}

static void update_variables(void)
{
    struct retro_variable var;

    /* Optional path overrides. These are plain config variables, not
     * registered options (frontends have no path-typed option UI), so
     * probe them once to keep frontends from logging unknown-variable
     * errors on every option change. */
    static bool paths_probed;
    if (!paths_probed) {
        paths_probed = true;
        get_option_string("xemu_bootrom_path", opt_bootrom_path, sizeof(opt_bootrom_path));
        get_option_string("xemu_bios_path", opt_bios_path, sizeof(opt_bios_path));
        get_option_string("xemu_hdd_path", opt_hdd_path, sizeof(opt_hdd_path));
        get_option_string("xemu_eeprom_path", opt_eeprom_path, sizeof(opt_eeprom_path));
    }

    var.key = "xemu_memory";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_memory_mb = atoi(var.value);
    }

    var.key = "xemu_skip_boot_anim";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_skip_boot_anim = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_network_backend";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "nat")) opt_network_backend = 1;
        else                           opt_network_backend = 0;
    }

    var.key = "xemu_hard_fpu";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_hard_fpu = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_surface_scale";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_surface_scale = atoi(var.value);
        if (opt_surface_scale < 1) opt_surface_scale = 1;
        if (opt_surface_scale > 10) opt_surface_scale = 10;
    }

    var.key = "xemu_console_language";
    var.value = NULL;
    opt_eeprom_language = -1;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        /* Xbox LanguageID values. "auto" leaves the EEPROM alone. */
        static const struct { const char *name; int id; } langs[] = {
            { "english", 1 }, { "japanese", 2 }, { "german", 3 },
            { "french", 4 },  { "spanish", 5 },  { "italian", 6 },
            { "korean", 7 },  { "chinese", 8 },  { "portuguese", 9 },
        };
        if (!strcmp(var.value, "frontend")) {
            unsigned fe_lang = RETRO_LANGUAGE_ENGLISH;
            if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &fe_lang)) {
                switch (fe_lang) {
                case RETRO_LANGUAGE_JAPANESE:   opt_eeprom_language = 2; break;
                case RETRO_LANGUAGE_GERMAN:     opt_eeprom_language = 3; break;
                case RETRO_LANGUAGE_FRENCH:     opt_eeprom_language = 4; break;
                case RETRO_LANGUAGE_SPANISH:    opt_eeprom_language = 5; break;
                case RETRO_LANGUAGE_ITALIAN:    opt_eeprom_language = 6; break;
                case RETRO_LANGUAGE_KOREAN:     opt_eeprom_language = 7; break;
                case RETRO_LANGUAGE_CHINESE_TRADITIONAL:
                case RETRO_LANGUAGE_CHINESE_SIMPLIFIED:
                                                opt_eeprom_language = 8; break;
                case RETRO_LANGUAGE_PORTUGUESE_BRAZIL:
                case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL:
                                                opt_eeprom_language = 9; break;
                default:                        opt_eeprom_language = 1; break;
                }
            } else {
                /* Frontend has no language API (RA 1.7.5): leave it alone
                 * rather than forcing English over the user's setting. */
                LRLOG_INFO("[xemu] Frontend language unavailable; "
                           "leaving console language unchanged\n");
            }
        } else {
            for (size_t i = 0; i < ARRAY_SIZE(langs); i++) {
                if (!strcmp(var.value, langs[i].name)) {
                    opt_eeprom_language = langs[i].id;
                    break;
                }
            }
        }
    }

    var.key = "xemu_console_video_standard";
    var.value = NULL;
    opt_eeprom_video_standard = 0;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "ntsc-m"))      opt_eeprom_video_standard = 0x00400100;
        else if (!strcmp(var.value, "ntsc-j")) opt_eeprom_video_standard = 0x00400200;
        else if (!strcmp(var.value, "pal-i"))  opt_eeprom_video_standard = 0x00800300;
    }

    var.key = "xemu_console_widescreen";
    var.value = NULL;
    opt_eeprom_widescreen = -1;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "on"))       opt_eeprom_widescreen = 1;
        else if (!strcmp(var.value, "off")) opt_eeprom_widescreen = 0;
    }

    var.key = "xemu_use_dsp";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_use_dsp = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_avpack";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "hdtv"))           opt_avpack = CONFIG_SYS_AVPACK_HDTV;
        else if (!strcmp(var.value, "composite")) opt_avpack = CONFIG_SYS_AVPACK_COMPOSITE;
        else if (!strcmp(var.value, "svideo"))    opt_avpack = CONFIG_SYS_AVPACK_SVIDEO;
        else if (!strcmp(var.value, "scart"))     opt_avpack = CONFIG_SYS_AVPACK_SCART;
        else if (!strcmp(var.value, "vga"))       opt_avpack = CONFIG_SYS_AVPACK_VGA;
    }

    var.key = "xemu_cache_shaders";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_cache_shaders = !strcmp(var.value, "enabled");
    }

    var.key = "xemu_display_filtering";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "nearest")) opt_filtering = CONFIG_DISPLAY_FILTERING_NEAREST;
        else                              opt_filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
    }

    var.key = "xemu_renderer";
    var.value = NULL;
    opt_renderer = RENDERER_AUTO;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "opengl"))      opt_renderer = RENDERER_OPENGL;
        else if (!strcmp(var.value, "vulkan")) opt_renderer = RENDERER_VULKAN;
    }

    var.key = "xemu_frame_output";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "hardware"))      opt_frame_output = 1;
        else if (!strcmp(var.value, "software")) opt_frame_output = 2;
        else                                     opt_frame_output = 0;
    }

    var.key = "xemu_audio_volume";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        opt_audio_volume = atoi(var.value);
        if (opt_audio_volume < 0) opt_audio_volume = 0;
        if (opt_audio_volume > 100) opt_audio_volume = 100;
    }

    LRLOG_INFO("[xemu] Options: memory=%dMB, scale=%d, avpack=%d, cache=%d, filter=%d, volume=%d%%, network=%s\n",
               opt_memory_mb, opt_surface_scale,
               opt_avpack, opt_cache_shaders, opt_filtering, opt_audio_volume,
               opt_network_backend == 1 ? "nat" : "disabled");

    /* Apply runtime-safe options */
    if (emu_initialized) {
        g_config.audio.volume_limit = opt_audio_volume / 100.0f;
        g_config.display.quality.surface_scale = opt_surface_scale;
        g_config.display.filtering = opt_filtering;
        g_config.perf.cache_shaders = opt_cache_shaders;
        g_config.audio.use_dsp = opt_use_dsp;
    }
}

/* ========================================================================= */
/* QEMU init helpers                                                         */
/* ========================================================================= */

static void populate_config(const char *dvd_path)
{
    LRLOG_INFO("[xemu] Populating g_config from core options\n");

    /* Set file paths in g_config */
    if (opt_bootrom_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.bootrom_path, opt_bootrom_path);
    }
    if (opt_bios_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.flashrom_path, opt_bios_path);
    }
    if (opt_hdd_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.hdd_path, opt_hdd_path);
    }
    if (opt_eeprom_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, opt_eeprom_path);
    }
    if (dvd_path && dvd_path[0]) {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, dvd_path);
    }

    /* Set renderer */
    if (use_vulkan) {
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    } else {
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    }

    /* Set memory (mem_limit is an int: 0=64MB, 1=128MB) */
    g_config.sys.mem_limit = (opt_memory_mb == 128) ? 1 : 0;

    /* Upstream defaults that the settings stub does not apply (g_config
     * is zero-initialized here; keep in sync with config_spec.yml).
     * Fields covered by core options are set below instead. */
    g_config.audio.use_dsp_jit = true;
    g_config.audio.hrtf = true;

    /* Critical defaults for libretro mode */
    g_config.general.show_welcome = false;
    g_config.audio.volume_limit = opt_audio_volume / 100.0f;
    g_config.perf.cache_shaders = opt_cache_shaders;

    /* Set other options */
    g_config.general.skip_boot_anim = opt_skip_boot_anim;
    g_config.perf.hard_fpu = opt_hard_fpu;
    g_config.audio.use_dsp = opt_use_dsp;
    g_config.display.quality.surface_scale = opt_surface_scale;
    g_config.sys.avpack = opt_avpack;
    g_config.display.filtering = opt_filtering;

    /* Set network configuration */
    g_config.net.enable = (opt_network_backend == 1);
    if (opt_network_backend == 1) {
        g_config.net.backend = CONFIG_NET_BACKEND_NAT;
    }

    LRLOG_INFO("[xemu] Config populated: bootrom=%s, bios=%s, hdd=%s, dvd=%s\n",
               g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)",
               g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)",
               g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)",
               g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
}

/* Thread function for running QEMU main loop */
static void *emu_thread_func(void *opaque)
{
    /* Build QEMU argv */
    const char *argv[] = {
        "xemu",
        "-device", "lpc47m157",
        "-serial", "null",
        NULL,
    };
    int argc = 5;

    qemu_init(argc, (char **)argv);

    /* Create XID USB gamepad devices for all 4 ports (BQL is held) */
    libretro_input_create_xid_devices();

    emu_initialized = true;


    /* Run main loop */
    while (emu_thread_running) {
        /* Check for reset requests */
        ShutdownCause reset_request = qemu_reset_requested_consume();
        if (reset_request) {
            pause_all_vcpus();
            qemu_system_reset(reset_request);
            resume_all_vcpus();
            /*
             * runstate can change in pause_all_vcpus()
             * as iothread mutex is unlocked
             */
            if (!runstate_check(RUN_STATE_RUNNING) &&
                    !runstate_check(RUN_STATE_INMIGRATE) &&
                    !runstate_check(RUN_STATE_FINISH_MIGRATE)) {
                runstate_set(RUN_STATE_PRELAUNCH);
            }
        }
        
        /* Check for shutdown requests */
        if (qemu_shutdown_requested_get()) {
            emu_thread_running = false;
            break;
        }
        
        main_loop_wait(false);

        /* Pause watchdog: when the frontend stops calling retro_run (menu
         * open, focus loss with pause_nonactive), stop the VM instead of
         * letting gameplay continue unattended; resume as soon as calls
         * return. retro_run's vblank bottom-half wakes this loop, so
         * resume latency is one frame. */
        {
            int64_t last = qatomic_read(&last_retro_run_us);
            if (last) {
                int64_t idle_us = g_get_monotonic_time() - last;
                if (!watchdog_paused && idle_us > 200000 &&
                    runstate_is_running()) {
                    vm_stop(RUN_STATE_PAUSED);
                    watchdog_paused = true;
                    LRLOG_INFO("[xemu] Frontend idle; pausing VM\n");
                } else if (watchdog_paused && idle_us < 100000) {
                    watchdog_paused = false;
                    if (runstate_check(RUN_STATE_PAUSED)) {
                        vm_start();
                    }
                    LRLOG_INFO("[xemu] Frontend back; resuming VM\n");
                }
            }
        }

        /* Check for snapshot requests from RetroArch thread */
        if (snapshot_request != SNAPSHOT_NONE) {
            int req = snapshot_request;
            snapshot_request = SNAPSHOT_NONE;
            bool ok = false;
            Error *snap_err = NULL;

            if (req == SNAPSHOT_SAVE) {
                ok = save_snapshot(snapshot_name, true, NULL, false, NULL, &snap_err);
                if (!ok && snap_err) {
                    error_free(snap_err);
                }
            } else if (req == SNAPSHOT_LOAD) {
                bool was_running = runstate_is_running();
                vm_stop(RUN_STATE_RESTORE_VM);
                ok = load_snapshot(snapshot_name, NULL, false, NULL, &snap_err);
                if (ok && was_running) {
                    vm_start();
                } else if (!ok) {
                    if (snap_err) {
                        error_free(snap_err);
                    }
                    if (was_running) vm_start();
                }
            }

            snapshot_result = ok;
            snapshot_done = true;
            if (snapshot_sem_initialized) {
                qemu_sem_post(&snapshot_done_sem);
            }
        }
    }

    bql_unlock();
    return NULL;
}

/* ========================================================================= */
/* Libretro API implementation                                               */
/* ========================================================================= */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    /* Set core options v2 */
    libretro_set_core_options(environ_cb);

    /* Get log interface */
    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
    }
    LRLOG_INFO("[xemu] retro_set_environment called\n");

    /* Get system directory for BIOS files */
    const char *sys_dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
        snprintf(system_dir, sizeof(system_dir), "%s", sys_dir);
        LRLOG_INFO("[xemu] System directory: %s\n", system_dir);
    } else {
        LRLOG_WARN("[xemu] Could not get system directory\n");
    }

    /* We need content (game ISO) */
    bool no_game = false;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);


    /* Get VFS interface */
    struct retro_vfs_interface_info vfs_info = { 3, NULL };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info)) {
        vfs_interface = vfs_info.iface;
        LRLOG_INFO("[xemu] VFS interface obtained (version %u)\n", vfs_info.required_interface_version);
    }

    /* Set pixel format */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    /* Set input descriptors — Xbox controller button labels */
    {
        struct retro_input_descriptor desc[] = {
#define PORT_DESCS(p) \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "White" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Black" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Left Trigger" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Right Trigger" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Left Stick" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Right Stick" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" }, \
            { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" }, \
            { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
            PORT_DESCS(0)
            PORT_DESCS(1)
            PORT_DESCS(2)
            PORT_DESCS(3)
#undef PORT_DESCS
            { 0 },
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
    }

    /* NOTE: deliberately no SET_CONTROLLER_INFO here. Declaring N controller
     * ports makes the frontend call retro_set_controller_port_device() for
     * each one, which the sibling xenia core turned into "four pads are
     * connected" and crashed the guest during launch. The descriptors above
     * are labels only and carry no connection semantics, so they stay. */
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)   { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)     { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)         { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)       { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "xemu";
    info->library_version  = xemu_version;
    info->valid_extensions = "iso|xiso";
    info->need_fullpath    = true;
    info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));

    /* Base is the Xbox's native mode. The guest may present something else
     * and the internal-resolution option scales it; SET_GEOMETRY reports
     * whatever we actually deliver, per frame.
     *
     * max_* is what the frontend sizes its hardware-render framebuffer
     * from, and it is a hard ceiling - a frame larger than this overruns
     * that buffer. It must therefore cover the largest surface the current
     * xemu_surface_scale can produce. The frontend queries AV info right
     * after retro_load_game(), by which point the option has been read. */
    unsigned scale = (unsigned)(opt_surface_scale > 0 ? opt_surface_scale : 1);

    info->geometry.base_width   = XBOX_NATIVE_WIDTH;
    info->geometry.base_height  = XBOX_NATIVE_HEIGHT;
    info->geometry.max_width    = XBOX_NATIVE_WIDTH * scale;
    info->geometry.max_height   = XBOX_NATIVE_HEIGHT * scale;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = console_refresh_hz();
    info->timing.sample_rate    = 48000.0;

    LRLOG_INFO("[xemu] AV info: %s %.2f Hz\n",
               console_is_pal() ? "PAL" : "NTSC", console_refresh_hz());
    LRLOG_INFO("[xemu] AV info: %ux%u base, %ux%u max (scale %ux)\n",
               info->geometry.base_width, info->geometry.base_height,
               info->geometry.max_width, info->geometry.max_height, scale);
}

RETRO_API void retro_init(void)
{
    LRLOG_INFO("[xemu] retro_init\n");

    /* Initialize Windows TLS keys for __thread replacements */
#if defined(LIBRETRO) && defined(_WIN32)
    {
        extern unsigned long tls_key_tcg_ctx;
        extern unsigned long tls_key_current_cpu;
        unsigned long __stdcall TlsAlloc(void);
        if (tls_key_tcg_ctx == 0xFFFFFFFF)
            tls_key_tcg_ctx = TlsAlloc();
        if (tls_key_current_cpu == 0xFFFFFFFF)
            tls_key_current_cpu = TlsAlloc();
        LRLOG_INFO("[xemu] TLS keys initialized: tcg_ctx=%lu current_cpu=%lu\n",
                   tls_key_tcg_ctx, tls_key_current_cpu);
    }
#endif

#ifdef _WIN32
    /* Set Windows timer resolution to 1ms for accurate APU throttle sleeps.
     * Default is ~15.6ms which causes bursty audio production. */
    {
        typedef unsigned int (__stdcall *timeBeginPeriod_t)(unsigned int);
        HMODULE winmm = LoadLibraryA("winmm.dll");
        if (winmm) {
            timeBeginPeriod_t fn = (timeBeginPeriod_t)GetProcAddress(winmm, "timeBeginPeriod");
            if (fn) {
                fn(1);
                LRLOG_INFO("[xemu] Set Windows timer resolution to 1ms\n");
            }
        }
    }
#endif

    /* Initialize the GL readiness wait event early */
    libretro_gl_init_wait_event();
}

RETRO_API void retro_deinit(void)
{
    LRLOG_INFO("[xemu] retro_deinit\n");
    game_loaded = false;
    emu_initialized = false;
    context_ready = false;
    use_vulkan = false;
}

/* Log the md5 of a system file and note whether it matches a known-good
 * dump. Informational only - a mismatch is warned, never fatal, since valid
 * BIOS/HDD variants exist (e.g. plain vs debug Complex, retail vs xemu HDD). */
static void log_system_file_hash(const char *label, const char *path,
                                 const char *const *known_md5, int known_count)
{
    GError *err = NULL;
    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, &err)) {
        if (err) g_error_free(err);
        return;
    }
    char *md5 = g_compute_checksum_for_data(G_CHECKSUM_MD5,
                                            (const guchar *)data, len);
    bool known = false;
    for (int i = 0; i < known_count; i++) {
        if (known_md5[i] && g_ascii_strcasecmp(md5, known_md5[i]) == 0) {
            known = true;
            break;
        }
    }
    if (known) {
        LRLOG_INFO("[xemu] %s md5 %s (known good)\n", label, md5);
    } else {
        LRLOG_WARN("[xemu] %s md5 %s is not a recognized dump; continuing "
                   "anyway (verify the file if the console misbehaves)\n",
                   label, md5);
    }
    g_free(md5);
    g_free(data);
}

/* Probe optional frontend capabilities.
 *
 * Deliberately NOT done in retro_set_environment: RetroArch calls that more
 * than once, and the later calls pass a restricted callback that refuses
 * these queries - caching a result from one of those left every capability
 * reading "no". Do it once per content load, where the callback is the real
 * one. */
static void probe_frontend_caps(void)
{
    if (!environ_cb) {
        return;
    }

    /* Full-machine x86 emulation with a GPU on top; tell frontends that
     * schedule by it not to expect this to be cheap. Advisory only. */
    unsigned perf_level = 15;
    environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &perf_level);

    /* Whether the frontend accepts a NULL frame meaning "repeat the last
     * one". Without it we must always deliver pixels, even on frames the
     * frontend has told us it will discard. */
    frontend_can_dupe = false;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &frontend_can_dupe)) {
        frontend_can_dupe = false;
    }

    /* Whether one call per port can return every digital button at once.
     * RetroArch 1.7.5 does not support this and takes the per-button path. */
    libretro_input_bitmasks =
        environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);

    /* The Duke and Controller S both have motors and xemu drives them from
     * the guest; hand those strengths to the frontend. The call succeeding
     * only means the frontend implements the interface, not that a pad with
     * motors is attached. */
    memset(&libretro_rumble, 0, sizeof(libretro_rumble));
    if (!environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE,
                    &libretro_rumble)) {
        memset(&libretro_rumble, 0, sizeof(libretro_rumble));
    }

    LRLOG_INFO("[xemu] Frontend: frame duping %s, input bitmasks %s, "
               "rumble %s\n",
               frontend_can_dupe ? "yes" : "no",
               libretro_input_bitmasks ? "yes" : "no",
               libretro_rumble.set_rumble_state ? "yes" : "no");
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    LRLOG_INFO("[xemu] retro_load_game called\n");

    if (!game || !game->path) {
        LRLOG_ERROR("[xemu] No game path provided\n");
        return false;
    }

    LRLOG_INFO("[xemu] Loading game: %s\n", game->path);

    /* Read core options */
    probe_frontend_caps();

    update_variables();

    /* Store the DVD path */
    snprintf(opt_dvd_path, sizeof(opt_dvd_path), "%s", game->path);

    /* Set shader cache base path */
    if (system_dir[0]) {
        char base_path[4096];
        if (build_system_path(base_path, sizeof(base_path), "")) {
            libretro_settings_set_base_path(base_path);
            LRLOG_INFO("[xemu] Shader cache base path: %s\n", base_path);
        }
    }

    /* Auto-populate BIOS paths from system directory if not set by options */
    if (system_dir[0]) {
        if (!opt_bootrom_path[0]) {
            build_system_path(opt_bootrom_path, sizeof(opt_bootrom_path),
                              "mcpx_1.0.bin");
            LRLOG_INFO("[xemu] Auto-set bootrom: %s\n", opt_bootrom_path);
        }
        if (!opt_bios_path[0]) {
            /* The Complex 4627 BIOS circulates under a few file names
             * (v1.03 is the debug build); accept whichever exists. When
             * none does, the first name is kept so the missing-file
             * message tells the user the preferred one. */
            static const char *const bios_names[] = {
                "Complex_4627v1.03.bin",
                "Complex_4627.bin",
            };
            for (size_t i = 0; i < ARRAY_SIZE(bios_names); i++) {
                char cand[4096];
                if (!build_system_path(cand, sizeof(cand), bios_names[i])) {
                    continue;
                }
                FILE *bf = fopen(cand, "rb");
                if (bf || i == 0) {
                    snprintf(opt_bios_path, sizeof(opt_bios_path), "%s",
                             cand);
                }
                if (bf) {
                    fclose(bf);
                    break;
                }
            }
            LRLOG_INFO("[xemu] Auto-set bios: %s\n", opt_bios_path);
        }
        if (!opt_hdd_path[0]) {
            build_system_path(opt_hdd_path, sizeof(opt_hdd_path),
                              "xbox_hdd.qcow2");
            LRLOG_INFO("[xemu] Auto-set hdd: %s\n", opt_hdd_path);
        }
        if (!opt_eeprom_path[0]) {
            build_system_path(opt_eeprom_path, sizeof(opt_eeprom_path),
                              "xbox_eeprom.bin");
            LRLOG_INFO("[xemu] Auto-set eeprom: %s\n", opt_eeprom_path);
        }
    }

    /* QEMU cannot re-initialize in-process: its global state (machine,
     * memory regions, chardevs, RCU threads) has no full teardown path.
     * Refuse a second load with a clear message instead of crashing. */
    static bool emu_was_started;
    if (emu_was_started) {
        show_user_message("xemu: restart RetroArch to load another game "
                          "(the emulator cannot re-initialize in-process)");
        return false;
    }
    emu_was_started = true;

    /* Validate required files exist (with frontend OSD messages: the log
     * is invisible to most users) */
    {
        char msg[512];
        FILE *f;
        f = fopen(opt_bootrom_path, "rb");
        if (!f) {
            snprintf(msg, sizeof(msg),
                     "xemu: MCPX boot ROM missing - expected %.400s",
                     opt_bootrom_path);
            show_user_message(msg);
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found bootrom: %s\n", opt_bootrom_path);
        {
            static const char *const mcpx_md5[] = {
                "d49c52a4102f6df7bcf8d0617ac475ed",  /* MCPX v1.0 */
            };
            log_system_file_hash("MCPX boot ROM", opt_bootrom_path, mcpx_md5,
                                 (int)ARRAY_SIZE(mcpx_md5));
        }

        f = fopen(opt_bios_path, "rb");
        if (!f) {
            snprintf(msg, sizeof(msg),
                     "xemu: Xbox BIOS missing - expected %.400s", opt_bios_path);
            show_user_message(msg);
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found bios: %s\n", opt_bios_path);
        {
            static const char *const bios_md5[] = {
                "21445c6f28fca7285b0f167ea770d1e5",  /* Complex 4627 v1.03 */
            };
            log_system_file_hash("Xbox BIOS", opt_bios_path, bios_md5,
                                 (int)ARRAY_SIZE(bios_md5));
        }

        f = fopen(opt_hdd_path, "rb");
        if (!f) {
            snprintf(msg, sizeof(msg),
                     "xemu: Xbox HDD image missing - expected %.400s",
                     opt_hdd_path);
            show_user_message(msg);
            return false;
        }
        fclose(f);
        LRLOG_INFO("[xemu] Found hdd: %s\n", opt_hdd_path);

        /* EEPROM: generate a fresh one if absent (same as standalone xemu
         * first-run behavior) rather than failing later. A wrong-sized file
         * (e.g. truncated by an earlier failed run) is regenerated too -
         * checking existence alone let a corrupt file block boot forever. */
        bool eeprom_ok = false;
        f = fopen(opt_eeprom_path, "rb");
        if (f) {
            long sz = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                sz = ftell(f);
            }
            fclose(f);
            eeprom_ok = sz == (long)sizeof(XboxEEPROM);
            if (!eeprom_ok) {
                snprintf(msg, sizeof(msg),
                         "xemu: EEPROM at %.400s is %ld bytes (expected %u), "
                         "regenerating", opt_eeprom_path, sz,
                         (unsigned)sizeof(XboxEEPROM));
                show_user_message(msg);
            }
        }
        if (!eeprom_ok) {
            /* qcrypto_init: eeprom generation prefers the crypto RNG and
             * we run before qemu_init (gnutls/gcrypt inits are refcounted,
             * so the later init call is unaffected). Generation itself no
             * longer depends on it - see xbox_eeprom_random_bytes. */
            qcrypto_init(NULL);
            if (xbox_eeprom_generate(opt_eeprom_path,
                                     XBOX_EEPROM_VERSION_R1)) {
                snprintf(msg, sizeof(msg), "xemu: generated new EEPROM at %.400s",
                         opt_eeprom_path);
                show_user_message(msg);
            } else {
                snprintf(msg, sizeof(msg),
                         "xemu: failed to write EEPROM at %.400s - check the "
                         "folder exists and is writable", opt_eeprom_path);
                show_user_message(msg);
                return false;
            }
        }

        /* Apply guest-persistent settings to the EEPROM before the machine
         * starts. Everything left on "auto" is untouched, so a value the
         * user set in the Xbox Dashboard survives. */
        {
            LibretroEepromSettings ee = {
                .language        = opt_eeprom_language,
                .video_standard  = opt_eeprom_video_standard,
                .widescreen      = opt_eeprom_widescreen,
            };
            char ee_err[128] = "";
            if (!libretro_eeprom_apply(opt_eeprom_path, &ee,
                                       ee_err, sizeof(ee_err))) {
                /* Non-fatal: the console still boots with its existing
                 * settings, which is better than refusing to start. */
                snprintf(msg, sizeof(msg),
                         "xemu: could not apply console settings - %.100s",
                         ee_err);
                show_user_message(msg);
            } else if (opt_eeprom_language > 0 ||
                       opt_eeprom_video_standard != 0 ||
                       opt_eeprom_widescreen >= 0) {
                LRLOG_INFO("[xemu] EEPROM settings applied: language=%d "
                           "video_standard=0x%08x widescreen=%d "
                           "(-1/0 = left unchanged)\n",
                           opt_eeprom_language, opt_eeprom_video_standard,
                           opt_eeprom_widescreen);
            }

            /* Read back what the console is now set to, rather than what we
             * asked for: on "auto" we asked for nothing, and the existing
             * value is what the guest will obey. */
            uint32_t vs = libretro_eeprom_read_video_standard(opt_eeprom_path);
            if (vs != 0) {
                console_video_standard = vs;
            }
            LRLOG_INFO("[xemu] Console TV standard: 0x%08x (%s, %.2f Hz)\n",
                       console_video_standard,
                       console_is_pal() ? "PAL" : "NTSC",
                       console_refresh_hz());
        }

        /* Warn early about content that is not an xiso image (redump-style
         * dumps need conversion, e.g. with extract-xiso): the console
         * otherwise silently boots to the dashboard. */
        f = fopen(game->path, "rb");
        if (f) {
            static const char xiso_magic[] = "MICROSOFT*XBOX*MEDIA";
            static const int64_t offsets[] = { 0x10000, 0x18310000 };
            char probe[20];
            bool looks_xiso = false;
            for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
                if (fseek(f, offsets[i], SEEK_SET) == 0 &&
                    fread(probe, 1, sizeof(probe), f) == sizeof(probe) &&
                    memcmp(probe, xiso_magic, sizeof(probe)) == 0) {
                    looks_xiso = true;
                    break;
                }
            }
            fclose(f);
            if (!looks_xiso) {
                show_user_message("xemu: content is not an xiso disc image; "
                                  "convert it (e.g. extract-xiso) if the "
                                  "console boots to the dashboard");
            }
        }
    }

    /* Per-content snapshot name: save states live as snapshots inside the
     * shared HDD image, so they must not collide across games. */
    {
        const char *base = strrchr(game->path, '/');
#ifdef _WIN32
        const char *bs = strrchr(game->path, '\\');
        if (bs && (!base || bs > base)) {
            base = bs;
        }
#endif
        base = base ? base + 1 : game->path;
        uint32_t hash = 2166136261u;
        for (const char *c = base; *c; c++) {
            hash = (hash ^ (uint8_t)*c) * 16777619u;
        }
        snprintf(snapshot_name, sizeof(snapshot_name), "lr-%08x", hash);
        LRLOG_INFO("[xemu] Save-state snapshot name: %s\n", snapshot_name);
    }

    /* States restore full machine state but are not deterministic
     * frame-serializations: tell the frontend not to offer rewind,
     * runahead or netplay based on them. */
    {
        uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
        environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    }

    /* Resolve frame output mode first. In software mode the core is fully
     * self-contained: private, isolated GL contexts + CPU readback on the
     * PFIFO thread, and no frontend hardware render context is requested at
     * all. That makes the core immune to the frontend's GL context
     * lifecycle (RetroArch 1.7.5's gl driver destroys/re-inits its master
     * context on video re-init, which orphans a shared child context and
     * faults the PFIFO worker thread on real GPU drivers) and to
     * cross-context sharing quirks (wine/Proton). It is the portable path
     * that works across RetroArch builds, GL drivers and OSes with no user
     * configuration, and it is the memory-frame path EmuVR's capture needs.
     *
     * 'auto' and 'software' therefore both use readback; 'hardware' is an
     * explicit opt-in for the direct-FBO blit (fastest, but only safe on a
     * frontend/driver that keeps its shared GL context alive across
     * re-inits — modern RetroArch with the glcore driver). */
    frame_readback = (opt_frame_output != 1);
    LRLOG_INFO("[xemu] Frame output: %s\n",
               frame_readback ? "software readback (self-contained, isolated GL)"
                              : "hardware (direct FBO)");

    /* Query the frontend's preferred HW render context */
    unsigned preferred_hw = RETRO_HW_CONTEXT_OPENGL_CORE;
    if (!frame_readback && environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred_hw);
    }
    LRLOG_INFO("[xemu] Frontend preferred HW render: %u\n", preferred_hw);

    /* Renderer selection.
     *
     * The internal NV2A renderer and the frontend's context type are two
     * independent choices that used to be assigned from one variable, so
     * the Vulkan renderer was reachable only when the frontend happened to
     * negotiate a Vulkan context. xemu_renderer separates them; "auto"
     * keeps the old behaviour exactly.
     *
     * What the Vulkan renderer can be fed today is narrower than what it
     * can render: its display image is handed to the frontend through
     * external memory, which needs a negotiated Vulkan context (so not in
     * software readback mode) and a Win32 handle (so not on POSIX). Both
     * limits are the frame-delivery side, not the renderer. */
    const char *vk_blocked_by = NULL;

    bool want_vulkan;
    switch (opt_renderer) {
    case RENDERER_OPENGL:
        want_vulkan = false;
        break;
    case RENDERER_VULKAN:
        want_vulkan = true;
        break;
    default:
        want_vulkan = (preferred_hw == RETRO_HW_CONTEXT_VULKAN);
        break;
    }

    if (want_vulkan && vk_blocked_by) {
        if (opt_renderer == RENDERER_VULKAN) {
            /* The user asked for it explicitly; say why they did not get
             * it rather than silently rendering with the other backend. */
            LRLOG_ERROR("[xemu] Vulkan renderer requested but unavailable: "
                        "%s. Using OpenGL.\n", vk_blocked_by);
        } else {
            LRLOG_INFO("[xemu] Frontend prefers Vulkan but %s; "
                       "requesting OpenGL\n", vk_blocked_by);
        }
        want_vulkan = false;
    }

    LRLOG_INFO("[xemu] Renderer: %s (option: %s)\n",
               want_vulkan ? "Vulkan" : "OpenGL",
               opt_renderer == RENDERER_OPENGL ? "opengl" :
               opt_renderer == RENDERER_VULKAN ? "vulkan" : "auto");

    if (frame_readback) {
        /* Pure software core: the frontend must render our frames itself
         * (RA 1.7.5 won't display memory frames while a HW context is
         * negotiated). The emulator's GL contexts are created isolated,
         * with the frontend's pixel format discovered by enumerating our
         * process's windows (correct rendering depends on the pixel
         * format; sharing is avoided — broken under Proton). */
        /* The renderer choice still stands: xemu's Vulkan backend creates
         * its own instance and device, so it does not need the frontend to
         * negotiate anything. Frames reach the frontend through readback
         * either way. */
        use_vulkan = want_vulkan;
    } else {
    /* Setup hardware rendering based on frontend preference */
    memset(&hw_render, 0, sizeof(hw_render));
    hw_render.context_reset      = context_reset;
    hw_render.context_destroy    = context_destroy;
    hw_render.depth              = true;
    hw_render.stencil            = true;
    hw_render.bottom_left_origin = true;
    hw_render.cache_context      = true;

    if (want_vulkan) {
        LRLOG_INFO("[xemu] Requesting Vulkan HW context\n");
        hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
        hw_render.version_major = 1;
        hw_render.version_minor = 3;
    } else {
        /* Request a legacy GL context like mupen64plus does: drivers hand
         * back their highest compatibility context (e.g. 4.6), which has
         * every 4.0 feature we need. Strict core-4.0 contexts are the odd
         * configuration in the wild (FBO quirks under wine WGL; the only
         * context type EmuVR's capture doesn't handle). Fallback below
         * still tries core 4.0 if the frontend rejects this. */
        LRLOG_INFO("[xemu] Requesting OpenGL (compatibility) HW context\n");
        hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
        hw_render.version_major = 0;
        hw_render.version_minor = 0;
    }

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
        /* Preferred context failed - try the other one */
        LRLOG_INFO("[xemu] Preferred context failed, trying fallback\n");
        memset(&hw_render, 0, sizeof(hw_render));
        hw_render.context_reset      = context_reset;
        hw_render.context_destroy    = context_destroy;
        hw_render.depth              = true;
        hw_render.stencil            = true;
        hw_render.bottom_left_origin = true;
        hw_render.cache_context      = true;

        if (want_vulkan) {
            hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
            hw_render.version_major = 0;
            hw_render.version_minor = 0;
            want_vulkan = false;
        } else {
            /* Legacy GL rejected: try strict core 4.0 */
            hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
            hw_render.version_major = 4;
            hw_render.version_minor = 0;
        }

        if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
            LRLOG_ERROR("[xemu] Failed to set HW render (neither GL nor VK available)\n");
            return false;
        }
    }

    use_vulkan = want_vulkan;
    if (use_vulkan) {
        LRLOG_INFO("[xemu] Using Vulkan HW rendering\n");
    } else {
        LRLOG_INFO("[xemu] Using OpenGL HW rendering\n");
        environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, NULL);
    }
    } /* !frame_readback */

    /* Populate xemu config from core options */
    populate_config(opt_dvd_path);

    if (frame_readback) {
        if (use_vulkan) {
            nv2a_vk_display_readback_set_enabled(true);
        } else {
            nv2a_gl_display_readback_set_enabled(true);
        }
        LRLOG_INFO("[xemu] Software output via %s; GL context setup deferred "
                   "to first retro_run\n", use_vulkan ? "Vulkan" : "OpenGL");
    } else if (getenv("XEMU_DUMP_DISPLAY")) {
        /* Debug: enable the capture (and its PPM dump) in HW mode too */
        nv2a_gl_display_readback_set_enabled(true);
    }

    /* Start emulation thread */
    emu_thread_running = true;
    qemu_thread_create(&emu_thread, "xemu-emu", emu_thread_func,
                       NULL, QEMU_THREAD_JOINABLE);

    game_loaded = true;

    LRLOG_INFO("[xemu] retro_load_game completed successfully\n");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type,
                                        const struct retro_game_info *info,
                                        size_t num_info)
{
    (void)type; (void)info; (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    LRLOG_INFO("[xemu] retro_unload_game\n");

    if (emu_thread_running) {
        emu_thread_running = false;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
        qemu_thread_join(&emu_thread);
        LRLOG_INFO("[xemu] Emulation thread joined\n");
    }

    game_loaded = false;
    emu_initialized = false;
}

/*
 * VGA framebuffer fallback: non-3D content (boot splash, many game menus,
 * some FMV) is rendered by the VGA scanout path into the QEMU console's
 * DisplaySurface, never touching the NV2A 3D display pipeline. Mirror that
 * surface into a shared buffer (QEMU thread, BQL held) so retro_run can
 * present it whenever no 3D surface exists — the libretro equivalent of
 * upstream xemu's VGA fallback in ui/xemu.c.
 */
static QemuMutex vga_fb_lock;
static bool vga_fb_lock_inited;
static uint32_t *vga_fb_pixels;
static int vga_fb_w, vga_fb_h, vga_fb_cap;
static uint32_t vga_fb_seq;

static void libretro_copy_vga_surface(QemuConsole *con)
{
    DisplaySurface *ds = qemu_console_surface(con);
    if (!ds) {
        return;
    }
    int w = surface_width(ds);
    int h = surface_height(ds);
    int stride = surface_stride(ds);
    pixman_format_code_t fmt = surface_format(ds);
    uint8_t *data = surface_data(ds);

    if (!data || w <= 0 || h <= 0 ||
        (fmt != PIXMAN_x8r8g8b8 && fmt != PIXMAN_a8r8g8b8)) {
        return;
    }

    if (!vga_fb_lock_inited) {
        qemu_mutex_init(&vga_fb_lock);
        vga_fb_lock_inited = true;
    }

    qemu_mutex_lock(&vga_fb_lock);
    if (vga_fb_cap < w * h) {
        vga_fb_pixels = g_realloc(vga_fb_pixels,
                                  (size_t)w * h * sizeof(uint32_t));
        vga_fb_cap = w * h;
    }
    /* DisplaySurface rows are top-down: already libretro's convention */
    for (int y = 0; y < h; y++) {
        memcpy(vga_fb_pixels + (size_t)y * w, data + (size_t)y * stride,
               (size_t)w * sizeof(uint32_t));
    }
    vga_fb_w = w;
    vga_fb_h = h;
    vga_fb_seq++;
    qemu_mutex_unlock(&vga_fb_lock);
}

/* Runs on the emulator thread (scheduled from retro_run). */
static void libretro_vblank_update(void *opaque)
{
    QemuConsole *con = (QemuConsole *)opaque;
    graphic_hw_update(con);
    /* Mirror the VGA scanout for both output modes: presented whenever
     * no NV2A surface exists (upstream xemu's fallback). */
    libretro_copy_vga_surface(con);
}

/* Called from retro_run (frontend thread). */
static bool libretro_get_vga_frame(uint32_t *dst, int cap_pixels,
                                   int *out_w, int *out_h)
{
    if (!vga_fb_lock_inited) {
        return false;
    }
    qemu_mutex_lock(&vga_fb_lock);
    int w = vga_fb_w, h = vga_fb_h;
    if (!vga_fb_pixels || w <= 0 || h <= 0 || w * h > cap_pixels) {
        qemu_mutex_unlock(&vga_fb_lock);
        return false;
    }
    memcpy(dst, vga_fb_pixels, (size_t)w * h * sizeof(uint32_t));
    *out_w = w;
    *out_h = h;
    qemu_mutex_unlock(&vga_fb_lock);
    return true;
}

/* Verbose per-frame diagnostics, enabled with XEMU_DEBUG=1 in the
 * environment. Quiet by default for release. */
static bool xemu_debug_logs(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("XEMU_DEBUG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

RETRO_API void retro_run(void)
{
    static int run_count = 0;
    run_count++;

    stats.frames++;
    /* Time-based, not frame-based: a frame counter goes quiet exactly when
     * the core is slow, which is when the line is most wanted. */
    stats_report_if_due();
    qatomic_set(&last_retro_run_us, g_get_monotonic_time());
    if (xemu_debug_logs() && (run_count <= 5 || (run_count % 300) == 0)) {
        LRLOG_INFO("[xemu] retro_run #%d (emu_init=%d ctx_ready=%d game=%d)\n",
                   run_count, emu_initialized, context_ready, game_loaded);
    }

    /* Pace retro_run to the emulated display rate when the frontend does
     * not throttle us (vsync unavailable or disabled, audio sync starved
     * because the APU produces audio in real time, occluded windows on
     * some Wayland compositors). Overspeeding is actively harmful here:
     * every retro_run performs a blocking display sync against the
     * PFIFO thread, so excess calls starve the emulation threads —
     * seen as stutter in heavier titles. When the frontend already
     * throttles at or below content rate the deadline is always in the
     * past and this block is a no-op. */
    /* Frontend state for this frame. Both calls are optional: an older
     * frontend simply leaves the defaults, which is the previous
     * behaviour. */
    bool fast_forwarding = false;
    if (environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fast_forwarding);
    }
    {
        static int last_ff = -1;
        if ((int)fast_forwarding != last_ff) {
            LRLOG_INFO("[xemu] Fast-forward %s\n",
                       fast_forwarding ? "engaged (pacer bypassed)"
                                       : "released (pacing to content rate)");
            last_ff = (int)fast_forwarding;
        }
    }

    int av_enable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    if (environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_enable);
    }
    /* Only skip video work if the frontend can repeat the previous frame;
     * otherwise it still needs pixels from us. */
    const bool skip_video =
        frontend_can_dupe && !(av_enable & RETRO_AV_ENABLE_VIDEO);

    if (fast_forwarding) {
        stats.ff_frames++;
    }

    if (!fast_forwarding)
    {
        int64_t pace_enter_us = g_get_monotonic_time();
        static int64_t next_frame_us;
        /* Pace to the console's own refresh - 50 Hz on a PAL console -
         * or we would self-pace at 60 while advertising 50. */
        const int64_t frame_us = console_is_pal() ? 20000 : 16683;
        int64_t now_us = g_get_monotonic_time();
        if (next_frame_us == 0 || now_us > next_frame_us + 2 * frame_us) {
            next_frame_us = now_us; /* first frame, or resync after a stall */
        }
        while (now_us < next_frame_us) {
            int64_t remain_us = next_frame_us - now_us;
            g_usleep(remain_us > 2000 ? remain_us - 1000 : remain_us);
            now_us = g_get_monotonic_time();
        }
        next_frame_us += frame_us;
        stats.pace_sleep_us +=
            (uint64_t)(g_get_monotonic_time() - pace_enter_us);
    }

    /* Schedule vblank (graphic_hw_update + VGA scanout mirror) on the
     * emulator thread */
    {
        QemuConsole *con = nv2a_get_vga_console();
        if (con) {
            aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                    libretro_vblank_update, con);
        }
    }

    /* Check if variables have been updated */
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        update_variables();
    }

    /* Poll input */
    if (input_poll_cb) {
        input_poll_cb();
    }

    /* Software mode: one-time GL setup. No hardware context is negotiated
     * in this mode, so context_reset() never runs and this is the only
     * place the emulator's GL contexts get created. The PFIFO thread is
     * already blocked in libretro_gl_wait_for_contexts(); if we never get
     * here it times out and runs pgraph_gl_init() with nothing current,
     * which aborts the frontend inside epoxy. */
    if (frame_readback && !context_ready) {
#ifdef _WIN32
        /* Discover the frontend's window pixel format (its GL window lives
         * in our process) and create isolated contexts with it - rendering
         * is wrong on contexts built from ChoosePixelFormat defaults. */
        int pf = 0;
        HDC frontend_dc = NULL;
        HWND hwnd = NULL;
        while ((hwnd = FindWindowExA(NULL, hwnd, NULL, NULL)) != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) {
                continue;
            }
            HDC dc = GetDC(hwnd);
            if (dc) {
                int wpf = GetPixelFormat(dc);
                if (wpf > 0) {
                    pf = wpf;
                    frontend_dc = dc; /* keep: used for format description */
                    break;
                }
                ReleaseDC(hwnd, dc);
            }
        }
        LRLOG_INFO("[xemu] Frontend window pixel format: %d%s\n", pf,
                   pf > 0 ? "" : " (not found; using defaults)");
#endif
        /* nv2a_context_init() leaves the nv2a display context current on
         * this (frontend) thread; restore the frontend's own context
         * before returning and before waking the PFIFO thread — see the
         * matching comment in context_reset(). */
        void *prev_ctx = glo_save_current();
#ifdef _WIN32
        libretro_gl_set_isolated_mode(pf, frontend_dc);
#else
        /* POSIX has no pixel-format equivalent to inherit, so use the
         * self-contained path: contexts off EGL_DEFAULT_DISPLAY (or GLX),
         * shared only with each other. Same shape the Vulkan branch of
         * context_reset() uses. */
        LRLOG_INFO("[xemu] Software mode: creating standalone GL contexts\n");
        libretro_gl_set_standalone_mode();
#endif
        nv2a_context_init();
        glo_restore_current(prev_ctx);
        libretro_gl_wake_pfifo();
        context_ready = true;
        LRLOG_INFO("[xemu] Software mode: GL contexts ready\n");
    }

    if (!emu_initialized || !context_ready) {
        /* Emulator not ready yet, draw black frame */
        if (frame_readback) {
            /* readback_frame is zero-initialized = black */
            video_cb(readback_frame, 640, 480, 640 * sizeof(uint32_t));
        } else if (!use_vulkan && hw_render.get_current_framebuffer) {
            uintptr_t fbo = hw_render.get_current_framebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
        } else if (use_vulkan) {
            /* VK mode: pass NULL frame (no HW framebuffer concept) */
            video_cb(NULL, 640, 480, 0);
        } else {
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
        }

        /* Push silence for audio */
        if (audio_batch_cb) {
            int16_t silence[1600] = {0}; /* ~800 samples stereo */
            audio_batch_cb(silence, 800);
        }
        return;
    }

    /* Audio first: keeps sound continuous even when the display sync below
     * blocks (heavy titles running behind real time). */
    libretro_drain_audio();

    /* ---- Software readback path ---- */
    if (frame_readback) {
        /* Mirror the proven hardware path exactly: the full
         * get_framebuffer_surface cycle validates the surface at the
         * display address, maintains its frame_time (surface-cache aging
         * depends on it — skipping this causes wrong/stale surface
         * selection: black menus, misoriented FMV), and blocks until
         * render_display has completed, at which point our PFIFO-side
         * capture holds exactly the frame the HW path would blit. */
        int pg_tex = 0;
        if (emu_initialized) {
            pg_tex = nv2a_get_framebuffer_surface();
            nv2a_release_framebuffer_surface();
        }

        int w = 0, h = 0;
        bool got = false;
        bool used_vga = false;

        /* The frontend has told us it will discard this frame (fast-forward,
         * rewind, run-ahead). The surface cycle above still runs - it ages
         * the surface cache and picking it up late causes stale-surface
         * artifacts - but the GPU->CPU readback, which is the expensive part
         * of this path, is skipped and the frontend repeats its last frame.
         * The hardware paths do not do this: their blit is GPU-side and
         * cheap, so skipping it would add risk for no real saving. */
        if (skip_video) {
            stats.duped++;
            video_cb(NULL, last_frame_width, last_frame_height, 0);
            goto readback_done;
        }

        const int64_t capture_start_us = g_get_monotonic_time();
        if (use_vulkan) {
            /* The Vulkan capture runs on the emulation thread off the back
             * of render_display, so there is no surface handle to check
             * first - a frame is either ready or it is not. */
            got = nv2a_vk_get_display_frame(readback_frame,
                                            READBACK_MAX_W * READBACK_MAX_H,
                                            &w, &h);
        } else if (pg_tex) {
            got = nv2a_gl_get_display_frame(readback_frame,
                                            READBACK_MAX_W * READBACK_MAX_H,
                                            &w, &h);
        }

        if (!got) {
            /* No 3D surface at the display address: present the VGA
             * scanout surface — verbatim upstream xemu's fallback. */
            int vw = 0, vh = 0;
            if (libretro_get_vga_frame(readback_frame,
                                       READBACK_MAX_W * READBACK_MAX_H,
                                       &vw, &vh)) {
                w = vw;
                h = vh;
                got = true;
                used_vga = true;
            }
        }

        if (xemu_debug_logs() && (run_count <= 5 || (run_count % 600) == 0)) {
            LRLOG_INFO("[xemu] sw-frame#%d: got=%d %dx%d src=%s tex=%d\n",
                       run_count, got, w, h,
                       used_vga ? "vga" : "pgraph", pg_tex);
        }
        stats.capture_us +=
            (uint64_t)(g_get_monotonic_time() - capture_start_us);
        if (got) {
            stats.delivered++;
            if (use_vulkan) {
                stats.src_vk++;
            } else if (used_vga) {
                stats.src_vga++;
            } else {
                stats.src_pgraph++;
            }
        } else {
            stats.black++;
        }

        if (got) {
            update_display_geometry((unsigned)w, (unsigned)h);
            last_frame_width  = (unsigned)w;
            last_frame_height = (unsigned)h;
            video_cb(readback_frame, w, h, w * sizeof(uint32_t));
        } else {
            /* No display frame yet: show black */
            memset(readback_frame, 0,
                   XBOX_NATIVE_WIDTH * XBOX_NATIVE_HEIGHT * sizeof(uint32_t));
            video_cb(readback_frame, XBOX_NATIVE_WIDTH, XBOX_NATIVE_HEIGHT,
                     XBOX_NATIVE_WIDTH * sizeof(uint32_t));
        }
readback_done:
        ;
    }
    /* ---- OpenGL HW path ---- */
    else if (!use_vulkan) {
        /* Get NV2A framebuffer texture */
        GLuint tex = nv2a_get_framebuffer_surface();
        uintptr_t fbo = hw_render.get_current_framebuffer();

        unsigned width = XBOX_NATIVE_WIDTH;
        unsigned height = XBOX_NATIVE_HEIGHT;

        if (tex) {
            /* Use the surface's real size rather than assuming the native
             * mode. The Vulkan path already does this via
             * nv2a_get_vk_display_info(); there is no equivalent accessor
             * on the GL side, so query the texture the way upstream's own
             * RenderFramebuffer() does. Reporting a fixed 640x480 here
             * pinned output at native res and made xemu_surface_scale
             * invisible on modern RetroArch. */
            GLint tw = 0, th = 0;
            glBindTexture(GL_TEXTURE_2D, tex);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (tw > 0 && th > 0) {
                width = (unsigned)tw;
                height = (unsigned)th;
            }
            blit_nv2a_texture(tex, width, height, fbo);
        } else {
            /* No NV2A surface: present the VGA scanout (boot/legal/menu
             * screens rendered CPU-side) — upstream xemu's fallback path.
             * Rows are flipped to match the blit's texture conventions. */
            int vw = 0, vh = 0;
            static GLuint vga_tex;
            static uint32_t vga_stage[READBACK_MAX_W * READBACK_MAX_H];
            if (libretro_get_vga_frame(vga_stage,
                                       READBACK_MAX_W * READBACK_MAX_H,
                                       &vw, &vh)) {
                for (int y = 0; y < vh; y++) {
                    memcpy(readback_frame + (size_t)(vh - 1 - y) * vw,
                           vga_stage + (size_t)y * vw,
                           (size_t)vw * sizeof(uint32_t));
                }
                if (!vga_tex) {
                    glGenTextures(1, &vga_tex);
                }
                glBindTexture(GL_TEXTURE_2D, vga_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0,
                             GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                             readback_frame);
                glBindTexture(GL_TEXTURE_2D, 0);
                width = (unsigned)vw;
                height = (unsigned)vh;
                blit_nv2a_texture(vga_tex, width, height, fbo);
            } else {
                /* Nothing at all yet: dark blue */
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
                glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }

        nv2a_release_framebuffer_surface();
        update_display_geometry(width, height);
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
    }
    /* ---- Vulkan path ---- */
    else if (use_vulkan) {
        unsigned width = 640;
        unsigned height = 480;

        if (!vulkan_if) {
            video_cb(NULL, width, height, 0);
            goto vk_audio;
        }

        static bool vk_display_initialized = false;

        if (!vk_display_initialized) {
            /* First frame: blocking sync to initialize display image */
            int tex = nv2a_get_framebuffer_surface();
            nv2a_release_framebuffer_surface();
            if (tex) {
                vk_display_initialized = true;
            }
        } else {
            /* Subsequent frames: trigger async render (non-blocking) */
            nv2a_trigger_display_render();
        }

        /* Get VK display info */
        void *ext_handle = NULL;
        int disp_w = 0, disp_h = 0;
        nv2a_get_vk_display_info(&ext_handle, &disp_w, &disp_h);

        /* Win32 hands us the external-memory HANDLE here; POSIX exports a
         * fresh fd inside the import instead, so only the dimensions gate
         * it there. */
#ifdef _WIN32
        const bool have_display = (ext_handle != NULL);
#else
        const bool have_display = true;
#endif
        if (have_display && disp_w > 0 && disp_h > 0) {
            /* Import xemu's display image into RA's VkDevice */
            static bool vk_layout_set = false;
            if (ra_vk_import_display(ext_handle, disp_w, disp_h)) {
                if (!vk_layout_set) {
                    ra_vk_transition_layout(ra_vk_image,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    vk_layout_set = true;
                }
                ra_vk_transition_layout(ra_vk_image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

                /* Set the image for RetroArch to display */
                retro_vk_image.image_view = ra_vk_image_view;
                retro_vk_image.image_layout = VK_IMAGE_LAYOUT_GENERAL;
                retro_vk_image.create_info = ra_vk_view_ci;

                vulkan_if->set_image(vulkan_if->handle, &retro_vk_image,
                                     0, NULL, VK_QUEUE_FAMILY_IGNORED);

                width = disp_w;
                height = disp_h;

                update_display_geometry(width, height);
                video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
            } else {
                video_cb(NULL, width, height, 0);
            }
        } else {
            video_cb(NULL, width, height, 0);
        }
    }

vk_audio:
    ; /* audio drained at the top of retro_run */
}

/* Pull audio from the APU ring buffer (hw/xbox/mcpx/apu/monitor.c) and hand
 * it to the frontend. Runs BEFORE any video work each retro_run: the display
 * sync can block for a while when emulation runs behind (heavy titles), and
 * draining audio first keeps sound continuous through those stalls. */
static void libretro_drain_audio(void)
{
    if (audio_batch_cb) {

        /* One-time flush: discard stale boot audio on first active pull */
        static bool audio_flushed = false;
        if (!audio_flushed) {
            libretro_audio_flush();
            audio_flushed = true;
        }

        /* Drain the whole backlog each call and let the frontend's
         * audio-sync rate control pace us. Fixed-size pulls (the old
         * 801-frame cap) underrun or force discards whenever the
         * frontend's retro_run cadence doesn't match 59.94 Hz exactly,
         * which is audible as crackle. */
        int avail = libretro_audio_ring_frames();

        /* Skip ahead only on a genuine stall (>100ms backlog), e.g. after
         * pause or fast-forward; keep ~33ms so playback stays seamless. */
        if (avail > 4800) {
            int16_t discard_buf[1024];
            int skip = avail - 1600;
            while (skip > 0) {
                int chunk = skip > 512 ? 512 : skip;
                libretro_audio_pull(discard_buf, chunk);
                skip -= chunk;
            }
            avail = libretro_audio_ring_frames();
        }

        /* Push in modest chunks; some audio drivers dislike huge batches. */
        while (avail > 0) {
            int16_t audio_buf[512 * 2];
            int want = avail > 512 ? 512 : avail;
            int frames = libretro_audio_pull(audio_buf, want);
            if (frames <= 0) {
                break;
            }
            audio_batch_cb(audio_buf, frames);
            stats.audio_frames += (unsigned)frames;
            avail -= frames;
        }
    }
}

RETRO_API void retro_reset(void)
{
    LRLOG_INFO("[xemu] retro_reset\n");
    if (emu_initialized) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/* ========================================================================= */
/* Save states via QEMU snapshot system                                      */
/* ========================================================================= */

/*
 * Save state header for RetroArch's .state file.
 * Actual VM state lives in the qcow2 HDD image via QEMU snapshots.
 */
#define LIBRETRO_SAVESTATE_SIZE  4096
#define LIBRETRO_SAVESTATE_MAGIC 0x58454D55 /* 'XEMU' */

struct libretro_savestate_header {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;
};

static bool snapshot_dispatch(int request_type, int timeout_ms)
{
    if (!snapshot_sem_initialized) {
        qemu_sem_init(&snapshot_done_sem, 0);
        snapshot_sem_initialized = true;
    }

    snapshot_done = false;
    snapshot_result = false;
    snapshot_request = request_type;
    /* The emulator loop may be idle (VM paused by the watchdog while the
     * frontend menu is open — the usual moment for save states). */
    qemu_notify_event();

    /* Wait for the emu thread to process it */
    if (qemu_sem_timedwait(&snapshot_done_sem, timeout_ms) < 0) {
        LRLOG_INFO("[xemu] snapshot dispatch: timeout after %dms\n", timeout_ms);
        snapshot_request = SNAPSHOT_NONE;
        return false;
    }

    return snapshot_result;
}

RETRO_API size_t retro_serialize_size(void)
{
    if (!emu_initialized) return 0;
    return LIBRETRO_SAVESTATE_SIZE;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    if (!emu_initialized || !data || size < LIBRETRO_SAVESTATE_SIZE)
        return false;

    memset(data, 0, LIBRETRO_SAVESTATE_SIZE);

    bool ok = snapshot_dispatch(SNAPSHOT_SAVE, 30000);

    if (!ok) {
        LRLOG_INFO("[xemu] retro_serialize: save failed\n");
        return false;
    }

    struct libretro_savestate_header *hdr = (struct libretro_savestate_header *)data;
    hdr->magic = LIBRETRO_SAVESTATE_MAGIC;
    hdr->version = 1;
    hdr->timestamp = (uint64_t)time(NULL);

    LRLOG_INFO("[xemu] retro_serialize: snapshot saved to HDD image\n");
    return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    if (!emu_initialized || !data || size < LIBRETRO_SAVESTATE_SIZE)
        return false;

    const struct libretro_savestate_header *hdr =
        (const struct libretro_savestate_header *)data;
    if (hdr->magic != LIBRETRO_SAVESTATE_MAGIC) {
        LRLOG_INFO("[xemu] retro_unserialize: invalid magic 0x%08x\n", hdr->magic);
        return false;
    }

    bool ok = snapshot_dispatch(SNAPSHOT_LOAD, 30000);

    if (!ok) {
        LRLOG_INFO("[xemu] retro_unserialize: load failed\n");
        return false;
    }

    LRLOG_INFO("[xemu] retro_unserialize: snapshot loaded from HDD image\n");
    return true;
}

/* ========================================================================= */
/* Memory stubs                                                              */
/* ========================================================================= */

RETRO_API void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

/* ========================================================================= */
/* Misc                                                                      */
/* ========================================================================= */

RETRO_API unsigned retro_get_region(void)
{
    return console_is_pal() ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    LRLOG_INFO("[xemu] Port %u device %u\n", port, device);
}
