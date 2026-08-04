/*
 *  Offscreen OpenGL abstraction layer - libretro frontend glue
 *
 *  Copyright (c) 2026 Tharwidu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef GLOFFSCREEN_LIBRETRO_H_
#define GLOFFSCREEN_LIBRETRO_H_

#include <stdbool.h>

/*
 * Handshake between the libretro frontend glue (ui/libretro.c, frontend
 * thread) and the gloffscreen backend that creates the emulator's GL
 * contexts (PFIFO thread). Implemented twice: gloffscreen_libretro.c for
 * WGL, gloffscreen_libretro_posix.c for EGL/GLX.
 */

/* Create the event the PFIFO thread waits on. Call before starting the VM. */
void libretro_gl_init_wait_event(void);

/* Capture the frontend's current GL context/surface for later sharing. Runs
 * on the frontend thread, inside context_reset. */
void libretro_gl_prepare(void);

/* Release the PFIFO thread once the frontend context has been captured. */
void libretro_gl_wake_pfifo(void);

/* Block until the emulator's contexts exist. Frontend thread. */
void libretro_gl_wait_for_contexts(void);

/* Announce that the emulator's contexts are usable. PFIFO thread. */
void libretro_gl_signal_ready(void);

/* Create contexts without sharing with the frontend, for the software
 * readback path where no frontend GL context is negotiated. */
void libretro_gl_set_standalone_mode(void);

/* Block until libretro_gl_signal_ready() has been called. */
void libretro_gl_wait_ready(void);

/* Non-blocking form of libretro_gl_wait_ready(). */
bool libretro_gl_is_ready(void);

#ifdef _WIN32
/* Create isolated contexts that reuse the frontend's pixel format without
 * sharing objects with it. Win32 only: a pixel format index and an HDC have
 * no EGL/GLX equivalent, and the POSIX backend uses standalone mode instead. */
void libretro_gl_set_isolated_mode(int pf, void *frontend_dc);
#endif

/* Route backend diagnostics through the frontend's log callback. Defined in
 * ui/libretro.c; the backend's own stderr never reaches RetroArch's log. */
void libretro_glo_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif /* GLOFFSCREEN_LIBRETRO_H_ */
