/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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
#include "hw/display/vga_int.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "renderer.h"

#include <math.h>

/*
 * The display path shares an object space (and, in libretro mode, a screen
 * DC) with the frontend's GL context. Frontend shader pipelines (e.g. legacy
 * GLSL presets in RetroArch 1.7.x) can leave GL errors latched where they
 * become observable here. Those are not display-path invariant violations,
 * so tolerate and report them instead of aborting the whole frontend.
 */
static void display_drain_gl_errors(const char *where)
{
    static int reports = 0;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        if (reports < 16) {
            fprintf(stderr, "[nv2a] tolerated GL error 0x%04x at %s\n",
                    err, where);
            reports++;
        }
    }
}

#ifdef LIBRETRO
/*
 * Self-contained display readback: the PFIFO thread copies each rendered
 * display frame (gl_display_buffer) to CPU memory on xemu's own display
 * context, so the libretro frontend can consume plain software frames with
 * no dependency on the frontend's GL context or texture sharing.
 */
static struct {
    bool inited;
    bool enabled;
    QemuMutex lock;
    uint32_t *pixels;   /* rows bottom-up, as glReadPixels produces them */
    int width, height;
    int cap_pixels;
    uint32_t *scratch;      /* written by the emulation thread, unguarded */
    int scratch_pixels;
    uint64_t copy_us;
    unsigned copy_count;
    bool has_frame;
    /* Async transfer: glReadPixels goes into a PBO (returns without
     * draining the GPU); the previous frame's PBO is mapped and copied out
     * on the next capture. One frame of extra latency in the mirror, but
     * the PFIFO thread no longer stalls on a GPU->CPU sync every frame. */
    GLuint pbo[2];
    int pbo_index;
    int pbo_w[2], pbo_h[2];  /* dimensions of the pending readback, 0=none */
} disp_readback;

void nv2a_gl_display_readback_set_enabled(bool enable)
{
    if (!disp_readback.inited) {
        qemu_mutex_init(&disp_readback.lock);
        disp_readback.inited = true;
    }
    disp_readback.enabled = enable;
}

void nv2a_gl_get_capture_stats(uint64_t *out_us, unsigned *out_count)
{
    if (!disp_readback.inited) {
        *out_us = 0;
        *out_count = 0;
        return;
    }
    qemu_mutex_lock(&disp_readback.lock);
    *out_us = disp_readback.copy_us;
    *out_count = disp_readback.copy_count;
    disp_readback.copy_us = 0;
    disp_readback.copy_count = 0;
    qemu_mutex_unlock(&disp_readback.lock);
}

/* nv2a-level entry for the software readback path; see
 * pgraph_gl_age_display_surface() for why this does not block. */
int nv2a_gl_age_display_surface_now(void)
{
    NV2AState *d = g_nv2a;
    if (!d) {
        return 0;
    }
    return pgraph_gl_age_display_surface(d);
}

bool nv2a_gl_get_display_frame(uint32_t *dst, int dst_cap_pixels,
                               int *out_width, int *out_height)
{
    if (!disp_readback.inited || !disp_readback.enabled) {
        return false;
    }
    qemu_mutex_lock(&disp_readback.lock);
    int w = disp_readback.width, h = disp_readback.height;
    if (!disp_readback.has_frame || w <= 0 || h <= 0 ||
        w * h > dst_cap_pixels) {
        qemu_mutex_unlock(&disp_readback.lock);
        return false;
    }
    /* flip rows: stored bottom-up, frontend wants top-down */
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w,
               disp_readback.pixels + (size_t)(h - 1 - y) * w,
               (size_t)w * sizeof(uint32_t));
    }
    *out_width = w;
    *out_height = h;
    qemu_mutex_unlock(&disp_readback.lock);
    return true;
}

/* Runs on the PFIFO thread with the display context current. */
static void capture_display_frame(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    int w = r->gl_display_buffer_width;
    int h = r->gl_display_buffer_height;
    if (w <= 0 || h <= 0 || !r->gl_display_buffer) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->disp_rndr.fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, r->gl_display_buffer, 0);

    /* Kick this frame's readback into a PBO (asynchronous). */
    if (!disp_readback.pbo[0]) {
        glGenBuffers(2, disp_readback.pbo);
    }
    int cur = disp_readback.pbo_index;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, disp_readback.pbo[cur]);
    if (disp_readback.pbo_w[cur] * disp_readback.pbo_h[cur] < w * h) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)w * h * sizeof(uint32_t),
                     NULL, GL_STREAM_READ);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    disp_readback.pbo_w[cur] = w;
    disp_readback.pbo_h[cur] = h;

    /* Collect the previous frame's readback; its DMA has had a frame to
     * complete, so the map rarely waits. */
    int prev = cur ^ 1;
    int pw = disp_readback.pbo_w[prev], ph = disp_readback.pbo_h[prev];
    if (pw > 0 && ph > 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, disp_readback.pbo[prev]);
        const uint32_t *src = (const uint32_t *)glMapBufferRange(
            GL_PIXEL_PACK_BUFFER, 0, (size_t)pw * ph * sizeof(uint32_t),
            GL_MAP_READ_BIT);
        if (src) {
            const int64_t t0 = g_get_monotonic_time();
            /* Copy out of the mapped PBO without the lock held. Doing this
             * inside it made every frontend fetch wait on the emulation
             * thread for a full-frame copy out of driver memory - the same
             * contention that cost the Vulkan path most of its frame rate. */
            if (disp_readback.scratch_pixels < pw * ph) {
                qemu_mutex_lock(&disp_readback.lock);
                g_free(disp_readback.scratch);
                g_free(disp_readback.pixels);
                disp_readback.scratch =
                    g_malloc((size_t)pw * ph * sizeof(uint32_t));
                disp_readback.pixels =
                    g_malloc((size_t)pw * ph * sizeof(uint32_t));
                disp_readback.scratch_pixels = pw * ph;
                disp_readback.cap_pixels = pw * ph;
                disp_readback.has_frame = false;
                qemu_mutex_unlock(&disp_readback.lock);
            }
            memcpy(disp_readback.scratch, src,
                   (size_t)pw * ph * sizeof(uint32_t));
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            qemu_mutex_lock(&disp_readback.lock);
            uint32_t *previous = disp_readback.pixels;
            disp_readback.pixels = disp_readback.scratch;
            disp_readback.scratch = previous;
            disp_readback.width = pw;
            disp_readback.height = ph;
            disp_readback.has_frame = true;
            disp_readback.copy_us += (uint64_t)(g_get_monotonic_time() - t0);
            disp_readback.copy_count++;
            qemu_mutex_unlock(&disp_readback.lock);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    disp_readback.pbo_index = prev;

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    display_drain_gl_errors("capture_display_frame");

    /* Debug: dump captured frames as PPM when XEMU_DUMP_DISPLAY is set */
    {
        static int dump_count;
        const char *dump_dir = getenv("XEMU_DUMP_DISPLAY");
        if (dump_dir && (++dump_count % 300) == 150) {
            char path[512];
            snprintf(path, sizeof(path), "%s/disp_%06d.ppm",
                     dump_dir, dump_count);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                /* write top-down (flip rows) */
                for (int y = h - 1; y >= 0; y--) {
                    for (int x = 0; x < w; x++) {
                        uint32_t px = disp_readback.pixels[(size_t)y * w + x];
                        uint8_t rgb[3] = { (px >> 16) & 0xff,
                                           (px >> 8) & 0xff, px & 0xff };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
            }
        }
    }
}
#endif /* LIBRETRO */

void pgraph_gl_init_display(NV2AState *d)
{
    struct PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glo_set_current(g_nv2a_context_display);

    glGenTextures(1, &r->gl_display_buffer);
    r->gl_display_buffer_internal_format = 0;
    r->gl_display_buffer_width = 0;
    r->gl_display_buffer_height = 0;
    r->gl_display_buffer_format = 0;
    r->gl_display_buffer_type = 0;

    const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    /* FIXME: improve interlace handling, pvideo */

    const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform bool pvideo_enable;\n"
        "uniform sampler2D pvideo_tex;\n"
        "uniform vec2 pvideo_in_pos;\n"
        "uniform vec4 pvideo_pos;\n"
        "uniform vec3 pvideo_scale;\n"
        "uniform bool pvideo_color_key_enable;\n"
        "uniform vec3 pvideo_color_key;\n"
        "uniform vec2 display_size;\n"
        "uniform float line_offset;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 texCoord = gl_FragCoord.xy/display_size;\n"
        "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
        "    texCoord.y = rel*(1.0f - texCoord.y);\n"
        "    out_Color.rgba = texture(tex, texCoord);\n"
        "    if (pvideo_enable) {\n"
        "        vec2 screenCoord = gl_FragCoord.xy - 0.5;\n"
        "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
        "        bvec4 clip = bvec4(lessThan(screenCoord, output_region.xy),\n"
        "                           greaterThan(screenCoord, output_region.zw));\n"
        "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
        "            vec2 out_xy = (screenCoord - pvideo_pos.xy) * pvideo_scale.z;\n"
        "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
        "            in_st.y *= -1.0;\n"
        "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
        "        }\n"
        "    }\n"
        "}\n";

    r->disp_rndr.prog = pgraph_gl_compile_shader(vs, fs);
    r->disp_rndr.tex_loc = glGetUniformLocation(r->disp_rndr.prog, "tex");
    r->disp_rndr.pvideo_enable_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_enable");
    r->disp_rndr.pvideo_tex_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_tex");
    r->disp_rndr.pvideo_in_pos_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_in_pos");
    r->disp_rndr.pvideo_pos_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_pos");
    r->disp_rndr.pvideo_scale_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_scale");
    r->disp_rndr.pvideo_color_key_enable_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_color_key_enable");
    r->disp_rndr.pvideo_color_key_loc = glGetUniformLocation(r->disp_rndr.prog, "pvideo_color_key");
    r->disp_rndr.display_size_loc = glGetUniformLocation(r->disp_rndr.prog, "display_size");
    r->disp_rndr.line_offset_loc = glGetUniformLocation(r->disp_rndr.prog, "line_offset");

    glGenVertexArrays(1, &r->disp_rndr.vao);
    glBindVertexArray(r->disp_rndr.vao);
    glGenBuffers(1, &r->disp_rndr.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->disp_rndr.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
    glGenFramebuffers(1, &r->disp_rndr.fbo);
    glGenTextures(1, &r->disp_rndr.pvideo_tex);
    display_drain_gl_errors("pgraph_gl_init_display");

    glo_set_current(g_nv2a_context_render);
}

void pgraph_gl_finalize_display(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    glo_set_current(g_nv2a_context_display);

    glDeleteTextures(1, &r->gl_display_buffer);
    r->gl_display_buffer = 0;

    glDeleteProgram(r->disp_rndr.prog);
    r->disp_rndr.prog = 0;

    glDeleteVertexArrays(1, &r->disp_rndr.vao);
    r->disp_rndr.vao = 0;

    glDeleteBuffers(1, &r->disp_rndr.vbo);
    r->disp_rndr.vbo = 0;

    glDeleteFramebuffers(1, &r->disp_rndr.fbo);
    r->disp_rndr.fbo = 0;

    glDeleteTextures(1, &r->disp_rndr.pvideo_tex);
    r->disp_rndr.pvideo_tex = 0;

    glo_set_current(g_nv2a_context_render);
}

static uint8_t *convert_texture_data__CR8YB8CB8YA8(const uint8_t *data,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   unsigned int pitch)
{
    uint8_t *converted_data = (uint8_t *)g_malloc(width * height * 4);
    int x, y;
    for (y = 0; y < height; y++) {
        const uint8_t *line = &data[y * pitch];
        const uint32_t row_offset = y * width;
        for (x = 0; x < width; x++) {
            uint8_t *pixel = &converted_data[(row_offset + x) * 4];
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    return converted_data;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                           unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void render_display_pvideo_overlay(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    bool enabled = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE)
        && d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    glUniform1ui(r->disp_rndr.pvideo_enable_loc, enabled);
    if (!enabled) {
        return;
    }

    hwaddr base = d->pvideo.regs[NV_PVIDEO_BASE];
    hwaddr limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    hwaddr offset = d->pvideo.regs[NV_PVIDEO_OFFSET];

    int in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    int in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);

    int in_s = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_S);
    int in_t = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_T);

    int in_pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    int in_color =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    unsigned int out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    unsigned int ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    unsigned int dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    if (ds_dx != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_x = pvideo_calculate_scale(ds_dx, out_width);
    }
    if (dt_dy != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_y = pvideo_calculate_scale(dt_dy, out_height);
    }

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (in_width > out_width) {
        in_width = floorf((float)out_width * scale_x + 0.5f);
    }
    if (in_height > out_height) {
        in_height = floorf((float)out_height * scale_y + 0.5f);
    }

    /* TODO: support other color formats */
    assert(in_color == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    unsigned int out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    unsigned int out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);

    unsigned int color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);
    glUniform1ui(r->disp_rndr.pvideo_color_key_enable_loc,
                 color_key_enabled);

    unsigned int color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;
    glUniform3f(r->disp_rndr.pvideo_color_key_loc,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);

    assert(offset + in_pitch * in_height <= limit);
    hwaddr end = base + offset + in_pitch * in_height;
    assert(end <= memory_region_size(d->vram));

    pgraph_apply_scaling_factor(pg, &out_x, &out_y);
    pgraph_apply_scaling_factor(pg, &out_width, &out_height);

    // Translate for the GL viewport origin.
    out_y = MAX(r->gl_display_buffer_height - 1 - (int)(out_y + out_height), 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, r->disp_rndr.pvideo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    uint8_t *tex_rgba = convert_texture_data__CR8YB8CB8YA8(
        d->vram_ptr + base + offset, in_width, in_height, in_pitch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, in_width, in_height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, tex_rgba);
    g_free(tex_rgba);
    glUniform1i(r->disp_rndr.pvideo_tex_loc, 1);
    glUniform2f(r->disp_rndr.pvideo_in_pos_loc, in_s / 16.f, in_t / 8.f);
    glUniform4f(r->disp_rndr.pvideo_pos_loc,
                out_x, out_y, out_width, out_height);
    glUniform3f(r->disp_rndr.pvideo_scale_loc,
                scale_x, scale_y, 1.0f / pg->surface_scale_factor);
}

/* See the matching counter in pgraph/vk/display.c: same metric in both
 * renderers so standalone and libretro numbers are comparable. */
static void perf_count_display_render(const char *renderer)
{
    static bool checked, enabled;
    static int64_t window_start_us;
    static unsigned count;

    if (!checked) {
        enabled = getenv("XEMU_PERF") != NULL;
        checked = true;
    }
    if (!enabled) {
        return;
    }

    int64_t now = g_get_monotonic_time();
    count++;
    if (window_start_us == 0) {
        window_start_us = now;
        count = 0;
        return;
    }
    if (now - window_start_us >= 5 * 1000 * 1000) {
        double secs = (double)(now - window_start_us) / 1e6;
        fprintf(stderr, "[xemu-perf] %s: %.1f display renders/s (%u in %.1fs)\n",
                renderer, count / secs, count, secs);
        window_start_us = now;
        count = 0;
    }
}

static void render_display(NV2AState *d, SurfaceBinding *surface)
{
    struct PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int width, height;
    VGADisplayParams vga_display_params;
    d->vga.get_resolution(&d->vga, (int*)&width, (int*)&height);
    d->vga.get_params(&d->vga, &vga_display_params);
    int line_offset = vga_display_params.line_offset ? surface->pitch / vga_display_params.line_offset : 1;

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] != NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }

    pgraph_apply_scaling_factor(pg, &width, &height);

    glBindFramebuffer(GL_FRAMEBUFFER, r->disp_rndr.fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->gl_display_buffer);
    bool recreate = (
        surface->fmt.gl_internal_format != r->gl_display_buffer_internal_format
        || width != r->gl_display_buffer_width
        || height != r->gl_display_buffer_height
        || surface->fmt.gl_format != r->gl_display_buffer_format
        || surface->fmt.gl_type != r->gl_display_buffer_type
        );

    if (recreate) {
        /* XXX: There's apparently a bug in some Intel OpenGL drivers for
         * Windows that will leak this texture when its orphaned after use in
         * another context, apparently regardless of which thread it's created
         * or released on.
         *
         * Driver: 27.20.100.8729 9/11/2020 W10 x64
         * Track: https://community.intel.com/t5/Graphics/OpenGL-Windows-drivers-for-Intel-HD-630-leaking-GPU-memory-when/td-p/1274423
         */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        r->gl_display_buffer_internal_format = surface->fmt.gl_internal_format;
        r->gl_display_buffer_width = width;
        r->gl_display_buffer_height = height;
        r->gl_display_buffer_format = surface->fmt.gl_format;
        r->gl_display_buffer_type = surface->fmt.gl_type;
        glTexImage2D(GL_TEXTURE_2D, 0,
            r->gl_display_buffer_internal_format,
            r->gl_display_buffer_width,
            r->gl_display_buffer_height,
            0,
            r->gl_display_buffer_format,
            r->gl_display_buffer_type,
            NULL);
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, r->gl_display_buffer, 0);
    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers);
    pgraph_gl_check_fbo(__func__);

    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glBindVertexArray(r->disp_rndr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->disp_rndr.vbo);
    glUseProgram(r->disp_rndr.prog);
    glProgramUniform1i(r->disp_rndr.prog, r->disp_rndr.tex_loc, 0);
    glUniform2f(r->disp_rndr.display_size_loc, width, height);
    glUniform1f(r->disp_rndr.line_offset_loc, line_offset);
    render_display_pvideo_overlay(d);

    glViewport(0, 0, width, height);
    glColorMask(true, true, true, true);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, 0, 0);
}

static void gl_fence(void)
{
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    int result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                                         (GLuint64)(5000000000));
    assert(result == GL_CONDITION_SATISFIED || result == GL_ALREADY_SIGNALED);
    glDeleteSync(fence);
}

void pgraph_gl_sync(NV2AState *d)
{
    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_gl_surface_get_within(d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width || !surface->height) {
        qemu_event_set(&d->pgraph.sync_complete);
        return;
    }

    /* FIXME: Sanity check surface dimensions */

    /* Wait for queued commands to complete */
    pgraph_gl_upload_surface_data(d, surface, !tcg_enabled());
    gl_fence();
    display_drain_gl_errors("pgraph_gl_sync/upload");

#ifdef LIBRETRO
    /* Display-path diagnostics: log the parameters that drive the display
     * shader's Y-flip so orientation bugs can be attributed. */
    {
        static int sync_count;
        static int dbg = -1;
        if (dbg < 0) {
            const char *v = getenv("XEMU_DEBUG");
            dbg = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        sync_count++;
        if (dbg && (sync_count <= 5 || (sync_count % 300) == 0)) {
            unsigned int dw = 0, dh = 0;
            VGADisplayParams p;
            d->vga.get_resolution(&d->vga, (int *)&dw, (int *)&dh);
            d->vga.get_params(&d->vga, &p);
            int lo = p.line_offset ? surface->pitch / p.line_offset : 1;
            fprintf(stderr,
                    "[nv2a] disp#%d: surf=%dx%d pitch=%d vga=%ux%u "
                    "line_offset_param=%d lo=%d swizzle=%d pv=%d\n",
                    sync_count, surface->width, surface->height,
                    surface->pitch, dw, dh, (int)p.line_offset, lo,
                    surface->swizzle,
                    (d->pvideo.regs[NV_PVIDEO_BUFFER] &
                     NV_PVIDEO_BUFFER_0_USE) != 0);
        }
    }
#endif

    /* Render framebuffer in display context */
    glo_set_current(g_nv2a_context_display);
    perf_count_display_render("opengl");
    render_display(d, surface);
    gl_fence();
    display_drain_gl_errors("pgraph_gl_sync/render_display");

#ifdef LIBRETRO
    if (disp_readback.enabled) {
        capture_display_frame(d);
    }
#endif

    /* Switch back to original context */
    glo_set_current(g_nv2a_context_render);

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

/* Non-blocking counterpart of pgraph_gl_get_framebuffer_surface below.
 *
 * That function does two separable things: it ages the display surface in
 * the cache (surface->frame_time), which is what keeps surface selection
 * correct, and it then waits for the PFIFO thread to finish a display pass.
 * The wait is only needed by callers that consume the returned texture.
 *
 * The software readback path does not: it uses the return value purely as
 * "is there a surface at the display address", then reads the PBO mirror,
 * which is a frame behind by design anyway. So it wants the aging without
 * the wait - the same shape the Vulkan path already uses via
 * nv2a_trigger_display_render(). Returns the display buffer, which may be
 * from the previous pass; callers must not treat it as this frame's image.
 */
int pgraph_gl_age_display_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_gl_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    surface->frame_time = pg->frame_time;
    if (!qatomic_read(&pg->sync_pending)) {
        qemu_event_reset(&pg->sync_complete);
        qatomic_set(&pg->sync_pending, true);
        pfifo_kick(d);
    }
    qemu_mutex_unlock(&d->pfifo.lock);

    return r->gl_display_buffer;
}

int pgraph_gl_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);
    // FIXME: Possible race condition with pgraph, consider lock

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_gl_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);
    assert(surface->fmt.gl_attachment == GL_COLOR_ATTACHMENT0);
    assert(surface->fmt.gl_format == GL_RGBA
        || surface->fmt.gl_format == GL_RGB
        || surface->fmt.gl_format == GL_BGR
        || surface->fmt.gl_format == GL_BGRA
        );

    surface->frame_time = pg->frame_time;
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);

    return r->gl_display_buffer;
}
