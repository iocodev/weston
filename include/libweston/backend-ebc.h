/*
 * Copyright © 2026
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WESTON_COMPOSITOR_EBC_H
#define WESTON_COMPOSITOR_EBC_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include <libweston/libweston.h>

#define WESTON_EBC_BACKEND_CONFIG_VERSION 1

struct weston_ebc_backend_config {
	struct weston_backend_config base;

	/** Select the renderer to use. Only pixman is supported by the ebc
	 *  backend; other values are forced to pixman. */
	enum weston_renderer_type renderer;

	/** Seat id used for the libinput seat. NULL means "seat0". */
	char *seat_id;

	/** Initial ebc display (waveform) mode name, one of:
	 *  "auto", "reset", "handwrite", "gc16-full", "gc16-part",
	 *  "norm-full", "norm-part", "trans-full", "trans-part",
	 *  "gl16-full", "gl16-part", "gcc16-full", "gcc16-part",
	 *  "a2", "du", "gc16-mono-part". Default: "auto". */
	char *disp_mode_name;

	/** Page transition type, see TRANS_* in ebc_uapi.h. Default: 0. */
	int trans_type;

	/** Page transition duration in milliseconds. Default: 0. */
	int trans_dur_ms;

	/** Do a GC16 full refresh right after the output is enabled, to
	 *  clean up the panel. Default: true. */
	bool initial_full_refresh;

	/** Do a GC16 full refresh every N repainted frames, 0 disables
	 *  periodic full refresh. Default: 0. */
	int full_refresh_frames;
};

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_EBC_H */
