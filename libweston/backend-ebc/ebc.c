/*
 * EBC (e-paper bitmap controller) backend for libweston.
 *
 * Renders the compositor into a shadow buffer with the pixman renderer,
 * converts the damaged region into the rkebc display buffer (8-bit gray for
 * monochrome panels, RGB565 for color panels), then hands the buffer to the
 * rkebc server for partial refresh.  The design mirrors the LVGL rkebc
 * display driver (lv_linux_rkebc.c):
 *
 *   - connect to the rkebc server (rkebc_create_client)
 *   - query the panel geometry/CFA through properties
 *   - dequeue a display buffer, draw into it, enqueue it for display,
 *     then dequeue the next buffer
 *   - select the waveform (display mode) and the 8-aligned dirty area
 *     per frame
 *
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

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include <libudev.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <libweston/libweston.h>
#include <libweston/backend-ebc.h>
#include "shared/helpers.h"
#include "shared/xalloc.h"
#include "shared/timespec-util.h"
#include "shared/weston-drm-fourcc.h"
#include "pixel-formats.h"
#include "pixman-renderer.h"
#include "libinput-seat.h"
#include "launcher-util.h"
#include "backend.h"
#include "libweston-internal.h"
#include "presentation-time-server-protocol.h"

#include <rkebc/rkebc_client.h>

/* Presentation refresh rate of the ebc output, purely informational since
 * repaints are driven by damage, not by vsync. */
#define EBC_OUTPUT_REPAINT_REFRESH	20000 /* in mHz */

struct ebc_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct udev *udev;
	struct udev_input input;
	bool input_initialized;

	rkebc_client node;

	int panel_width;
	int panel_height;
	int panel_cfa;

	enum weston_renderer_type renderer;
	char *seat_id;
	char *disp_mode_name;
	int disp_mode;
	int trans_type;
	int trans_dur_ms;
	bool initial_full_refresh;
	int full_refresh_frames;
	int refresh;

	struct weston_binding *full_refresh_binding;
};

struct ebc_head {
	struct weston_head base;
};

struct ebc_output {
	struct weston_output base;
	struct ebc_backend *backend;

	struct weston_mode mode;
	struct weston_renderbuffer *renderbuffer;
	struct rkebc_buf *disp_buf;

	int color_fmt;		/* rkebc image format */
	int frame_count;
	bool pending_full_refresh;	/* one-shot GC16 full refresh */
	bool force_full_refresh;	/* set by the debug binding */

	/* finish_frame is deferred to this timer because it must be called
	 * after repaint() returns (compositor then sets
	 * REPAINT_AWAITING_COMPLETION), like the headless backend. */
	struct wl_event_source *finish_frame_timer;
};

static void
ebc_destroy(struct weston_backend *backend);

static inline struct ebc_head *
to_ebc_head(struct weston_head *base)
{
	if (base->backend->destroy != ebc_destroy)
		return NULL;
	return container_of(base, struct ebc_head, base);
}

static void
ebc_output_destroy(struct weston_output *base);

static inline struct ebc_output *
to_ebc_output(struct weston_output *base)
{
	if (base->destroy != ebc_output_destroy)
		return NULL;
	return container_of(base, struct ebc_output, base);
}

static inline struct ebc_backend *
to_ebc_backend(struct weston_backend *base)
{
	return container_of(base, struct ebc_backend, base);
}

/*
 * Parse an ebc display (waveform) mode name into an ebc_disp_mode_t value.
 * Unknown names fall back to EBC_DISP_AUTO.
 */
static int
ebc_parse_mode(const char *name)
{
	static const struct {
		const char *name;
		int mode;
	} modes[] = {
		{ "auto",		EBC_DISP_AUTO },
		{ "reset",		EBC_DISP_RESET },
		{ "handwrite",		EBC_DISP_HANDWRITE },
		{ "norm-full",		EBC_DISP_NORM_FULL },
		{ "norm-part",		EBC_DISP_NORM_PART },
		{ "gc16-full",		EBC_DISP_NORM_FULL },
		{ "gc16-part",		EBC_DISP_NORM_PART },
		{ "full",		EBC_DISP_NORM_FULL },
		{ "part",		EBC_DISP_NORM_PART },
		{ "trans-full",		EBC_DISP_TRANS_FULL },
		{ "trans-part",		EBC_DISP_TRANS_PART },
		{ "gl16-full",		EBC_DISP_GL16_FULL },
		{ "gl16-part",		EBC_DISP_GL16_PART },
		{ "gcc16-full",		EBC_DISP_GCC16_FULL },
		{ "gcc16-part",		EBC_DISP_GCC16_PART },
		{ "a2",			EBC_DISP_A2 },
		{ "du",			EBC_DISP_DU },
		{ "gc16-mono-part",	EBC_DISP_GC16_MONO_PART },
	};
	size_t i;

	if (name == NULL)
		return EBC_DISP_AUTO;

	for (i = 0; i < ARRAY_LENGTH(modes); i++) {
		if (strcmp(name, modes[i].name) == 0)
			return modes[i].mode;
	}

	weston_log("ebc: unknown display mode \"%s\", using \"auto\"\n", name);
	return EBC_DISP_AUTO;
}

/*
 * Convert a damage region into an ebc dirty area.  The ebc driver requires
 * x/y/w/h to be 8-byte aligned, so expand the bounding box accordingly and
 * clip it to the panel.
 */
static void
ebc_dirty_area_from_region(struct ebc_output *output,
			   pixman_region32_t *region,
			   struct ebc_area *area)
{
	struct ebc_backend *b = output->backend;
	const pixman_box32_t *box = pixman_region32_extents(region);
	int x1, y1, x2, y2;

	x1 = box->x1 & ~7;
	y1 = box->y1 & ~7;
	x2 = (box->x2 + 7) & ~7;
	y2 = (box->y2 + 7) & ~7;

	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > b->panel_width)
		x2 = b->panel_width;
	if (y2 > b->panel_height)
		y2 = b->panel_height;

	if (x2 <= x1 || y2 <= y1) {
		memset(area, 0, sizeof(*area));
		return;
	}

	area->x = x1;
	area->y = y1;
	area->w = x2 - x1;
	area->h = y2 - y1;
}

/*
 * Copy the damaged region of the rendered shadow buffer into the rkebc
 * display buffer, converting XRGB8888 to the panel format:
 *   - monochrome panel (cfa == 0): 8-bit gray, ITU-R BT.601 luma
 *   - color panel: RGB565, same pixel layout that LVGL feeds rkebc
 *     (RKEBC_FMT_BGR565 + little-endian RGB565 words)
 */
static void
ebc_copy_to_disp_buf(struct ebc_output *output, pixman_image_t *image,
		     struct rkebc_buf *buf, const struct ebc_area *area)
{
	const uint8_t *src;
	uint8_t *dst;
	int src_stride, dst_stride;
	int bytespp;
	int x, y;

	if (area->w <= 0 || area->h <= 0)
		return;

	src = (const uint8_t *)pixman_image_get_data(image);
	src_stride = pixman_image_get_stride(image);

	dst = buf->addr;

	/* rkebc reports width_stride in pixels (like LVGL's
	 * lv_draw_buf_width_to_stride()); convert to bytes. */
	bytespp = (output->color_fmt == RKEBC_FMT_GRAY) ? 1 : 2;
	dst_stride = buf->width_stride * bytespp;
	if (dst_stride < (int)(buf->width * bytespp))
		dst_stride = buf->width * bytespp;

	for (y = 0; y < area->h; y++) {
		const uint32_t *s = (const uint32_t *)
			(src + (area->y + y) * src_stride) + area->x;
		uint8_t *d = dst + (area->y + y) * dst_stride + area->x * bytespp;

		if (output->color_fmt == RKEBC_FMT_GRAY) {
			for (x = 0; x < area->w; x++) {
				uint32_t p = s[x];
				uint8_t r = (p >> 16) & 0xff;
				uint8_t g = (p >> 8) & 0xff;
				uint8_t bl = p & 0xff;

				d[x] = (77 * r + 150 * g + 29 * bl) >> 8;
			}
		} else {
			uint16_t *d16 = (uint16_t *)d;

			for (x = 0; x < area->w; x++) {
				uint32_t p = s[x];
				uint8_t r = (p >> 16) & 0xff;
				uint8_t g = (p >> 8) & 0xff;
				uint8_t bl = p & 0xff;

				d16[x] = ((r >> 3) << 11) | ((g >> 2) << 5) |
					(bl >> 3);
			}
		}
	}
}

static int
ebc_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static void
ebc_full_refresh_binding(struct weston_keyboard *keyboard,
			 const struct timespec *time,
			 uint32_t key, void *data)
{
	struct ebc_backend *b = data;
	struct weston_output *output;

	(void) keyboard;
	(void) time;
	(void) key;

	wl_list_for_each(output, &b->compositor->output_list, link) {
		if (to_ebc_output(output))
			to_ebc_output(output)->force_full_refresh = true;
	}

	weston_compositor_damage_all(b->compositor);
}

static int
ebc_finish_frame_handler(void *data)
{
	struct ebc_output *output = data;

	weston_output_finish_frame_from_timer(&output->base);

	return 1;
}

static int
ebc_output_repaint(struct weston_output *output_base)
{
	struct ebc_output *output = to_ebc_output(output_base);
	struct ebc_backend *b = output->backend;
	struct weston_compositor *ec = output->base.compositor;
	const struct pixman_renderer_interface *pixman;
	pixman_region32_t damage;
	int delay_msec;
	bool full_refresh;

	pixman_region32_init(&damage);
	weston_output_flush_damage_for_primary_plane(output_base, &damage);

	if (!pixman_region32_not_empty(&damage) || !output->renderbuffer)
		goto out;

	if (!output->disp_buf) {
		output->disp_buf = rkebc_client_dequeue_buf(b->node,
							   output->color_fmt, 0);
		if (!output->disp_buf) {
			weston_log("ebc: failed to dequeue a display buffer, "
				   "dropping frame\n");
			/* Keep the damage so the frame is presented on the
			 * next repaint once a buffer is available. */
			weston_output_damage(output_base);
			goto out;
		}
	}

	ec->renderer->repaint_output(output_base, &damage, output->renderbuffer);

	output->frame_count++;
	full_refresh = output->pending_full_refresh || output->force_full_refresh;
	if (!full_refresh && b->full_refresh_frames > 0 &&
	    output->frame_count >= b->full_refresh_frames)
		full_refresh = true;

	if (full_refresh) {
		/* GC16 full refresh cleans up ghosts on the panel */
		output->disp_buf->dirty_area.x = 0;
		output->disp_buf->dirty_area.y = 0;
		output->disp_buf->dirty_area.w = b->panel_width;
		output->disp_buf->dirty_area.h = b->panel_height;
		output->disp_buf->disp_mode = EBC_DISP_NORM_FULL;

		output->pending_full_refresh = false;
		output->force_full_refresh = false;
		output->frame_count = 0;
	} else {
		ebc_dirty_area_from_region(output, &damage,
					   &output->disp_buf->dirty_area);
		output->disp_buf->disp_mode = b->disp_mode;
	}

	output->disp_buf->trans_type = b->trans_type;
	output->disp_buf->trans_dur_ms = b->trans_dur_ms;

	pixman = ec->renderer->pixman;
	ebc_copy_to_disp_buf(output,
			     pixman->renderbuffer_get_image(output->renderbuffer),
			     output->disp_buf, &output->disp_buf->dirty_area);

	weston_log("ebc: enqueue mode 0x%x area (%d,%d)(%dx%d) full=%d frame=%d\n",
		   output->disp_buf->disp_mode,
		   output->disp_buf->dirty_area.x, output->disp_buf->dirty_area.y,
		   output->disp_buf->dirty_area.w, output->disp_buf->dirty_area.h,
		   full_refresh, output->frame_count);
	if (rkebc_client_enqueue_buf(output->disp_buf) < 0) {
		weston_log("ebc: failed to enqueue a display buffer\n");
		rkebc_client_drop_buf(output->disp_buf);
		output->disp_buf = NULL;
		goto out;
	}

	output->disp_buf = rkebc_client_dequeue_buf(b->node,
						    output->color_fmt, 0);
	if (!output->disp_buf)
		weston_log("ebc: failed to dequeue the next display buffer\n");

out:
	pixman_region32_fini(&damage);

	/* finish_frame must run after repaint() returns; the compositor
	 * sets REPAINT_AWAITING_COMPLETION only then. Defer it via a
	 * timer exactly like the headless backend does. */
	delay_msec = millihz_to_nsec(output->mode.refresh) / 1000000;
	wl_event_source_timer_update(output->finish_frame_timer, delay_msec);

	return 0;
}

static int
ebc_output_disable(struct weston_output *base)
{
	struct ebc_output *output = to_ebc_output(base);

	assert(output);

	if (!output->base.enabled)
		return 0;

	if (output->disp_buf) {
		if (rkebc_client_drop_buf(output->disp_buf) < 0)
			weston_log("ebc: failed to drop display buffer\n");
		output->disp_buf = NULL;
	}

	if (output->renderbuffer) {
		weston_renderbuffer_unref(output->renderbuffer);
		output->renderbuffer = NULL;
	}

	base->compositor->renderer->pixman->output_destroy(base);

	return 0;
}

static void
ebc_output_destroy(struct weston_output *base)
{
	struct ebc_output *output = to_ebc_output(base);

	assert(output);

	if (output->finish_frame_timer)
		wl_event_source_remove(output->finish_frame_timer);

	ebc_output_disable(&output->base);
	weston_output_release(&output->base);
	free(output);
}

static int
ebc_output_enable(struct weston_output *base)
{
	struct ebc_output *output = to_ebc_output(base);
	struct ebc_backend *b = output->backend;
	const struct pixman_renderer_interface *pixman;
	const struct weston_mode *mode = base->current_mode;
	const struct pixel_format_info *format;
	const struct pixman_renderer_output_options options = {
		.use_shadow = false,
		.fb_size = {
			.width = mode->width,
			.height = mode->height
		},
		.format = pixel_format_get_info(DRM_FORMAT_XRGB8888),
	};

	assert(output);

	pixman = base->compositor->renderer->pixman;

	if (pixman->output_create(base, &options) < 0) {
		weston_log("ebc: failed to create pixman renderer output "
			   "state\n");
		return -1;
	}

	format = pixel_format_get_info(DRM_FORMAT_XRGB8888);
	output->renderbuffer = pixman->create_image(base, format,
						    mode->width, mode->height);
	if (!output->renderbuffer) {
		weston_log("ebc: failed to create renderbuffer\n");
		pixman->output_destroy(base);
		return -1;
	}

	output->disp_buf = rkebc_client_dequeue_buf(b->node,
						    output->color_fmt, 0);
	if (!output->disp_buf) {
		weston_log("ebc: failed to dequeue a display buffer\n");
		weston_renderbuffer_unref(output->renderbuffer);
		output->renderbuffer = NULL;
		pixman->output_destroy(base);
		return -1;
	}

	output->pending_full_refresh = b->initial_full_refresh;
	weston_compositor_damage_all(base->compositor);

	return 0;
}

static struct weston_output *
ebc_output_create(struct weston_backend *backend, const char *name)
{
	struct ebc_backend *b = container_of(backend, struct ebc_backend, base);
	struct ebc_output *output;
	struct wl_event_loop *loop;

	assert(name);

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	weston_output_init(&output->base, b->compositor, name);

	output->base.destroy = ebc_output_destroy;
	output->base.disable = ebc_output_disable;
	output->base.enable = ebc_output_enable;
	output->base.attach_head = NULL;

	output->backend = b;

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = b->panel_width;
	output->mode.height = b->panel_height;
	output->mode.refresh = b->refresh;
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	/* 1:1 output scale (physical pixels); weston_output_enable() asserts
	 * current_scale is non-zero and transform is set. */
	output->base.current_scale = 1;
	output->base.native_scale = 1;
	output->base.transform = WL_OUTPUT_TRANSFORM_NORMAL;

	output->base.current_mode = &output->mode;

	output->base.start_repaint_loop = ebc_output_start_repaint_loop;
	output->base.repaint = ebc_output_repaint;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	/* Monochrome panels use 8-bit gray; color e-ink uses RGB565, the
	 * same format pairing the LVGL rkebc driver feeds the server. */
	if (b->panel_cfa == 0)
		output->color_fmt = RKEBC_FMT_GRAY;
	else
		output->color_fmt = RKEBC_FMT_BGR565;

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, ebc_finish_frame_handler, output);
	if (output->finish_frame_timer == NULL) {
		weston_log("ebc: failed to add finish frame timer\n");
		weston_output_release(&output->base);
		free(output);
		return NULL;
	}

	weston_compositor_add_pending_output(&output->base, b->compositor);

	return &output->base;
}

static int
ebc_head_create(struct ebc_backend *backend, const char *name)
{
	struct ebc_head *head;

	assert(name);

	head = zalloc(sizeof *head);
	if (head == NULL)
		return -1;

	weston_head_init(&head->base, name);

	head->base.backend = &backend->base;

	weston_head_set_connection_status(&head->base, true);
	weston_head_set_monitor_strings(&head->base, "weston", "ebc", NULL);

	weston_compositor_add_head(backend->compositor, &head->base);

	return 0;
}

static void
ebc_head_destroy(struct weston_head *base)
{
	struct ebc_head *head = to_ebc_head(base);

	assert(head);

	weston_head_release(&head->base);
	free(head);
}

static void
ebc_destroy(struct weston_backend *backend)
{
	struct ebc_backend *b = to_ebc_backend(backend);
	struct weston_head *base, *next;

	wl_list_remove(&b->base.link);

	wl_list_for_each_safe(base, next, &b->compositor->head_list,
			      compositor_link) {
		if (to_ebc_head(base))
			ebc_head_destroy(base);
	}

	if (b->full_refresh_binding)
		weston_binding_destroy(b->full_refresh_binding);

	if (b->input_initialized)
		udev_input_destroy(&b->input);
	if (b->udev)
		udev_unref(b->udev);
	if (b->compositor->launcher) {
		weston_launcher_destroy(b->compositor->launcher);
		b->compositor->launcher = NULL;
	}

	if (b->node)
		rkebc_destroy_client(b->node);

	free(b);
}

static struct ebc_backend *
ebc_backend_create(struct weston_compositor *compositor,
		   struct weston_ebc_backend_config *config)
{
	struct ebc_backend *b;
	const char *seat_id;
	int ret;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	b->renderer = config->renderer;
	b->seat_id = config->seat_id;
	b->disp_mode_name = config->disp_mode_name;
	b->trans_type = config->trans_type;
	b->trans_dur_ms = config->trans_dur_ms;
	b->initial_full_refresh = config->initial_full_refresh;
	b->full_refresh_frames = config->full_refresh_frames;
	b->refresh = EBC_OUTPUT_REPAINT_REFRESH;

	/* Connect to the rkebc server, which owns the kernel ebc device. */
	b->node = rkebc_create_client();
	if (b->node == NULL) {
		weston_log("ebc: failed to connect to the rkebc server\n");
		free(b);
		return NULL;
	}

	rkebc_client_property_get_int(b->node, "panel_width",
				      &b->panel_width);
	rkebc_client_property_get_int(b->node, "panel_height",
				      &b->panel_height);
	rkebc_client_property_get_int(b->node, "panel_cfa", &b->panel_cfa);

	weston_log("ebc: panel %dx%d, cfa %d\n",
		   b->panel_width, b->panel_height, b->panel_cfa);

	if (b->panel_width <= 0 || b->panel_height <= 0) {
		weston_log("ebc: invalid panel size %dx%d\n",
			   b->panel_width, b->panel_height);
		goto err_client;
	}

	b->disp_mode = ebc_parse_mode(b->disp_mode_name);
	weston_log("ebc: display mode %s (0x%x), trans %d/%d ms, "
		   "full refresh: %s%s\n",
		   b->disp_mode_name ? b->disp_mode_name : "auto",
		   b->disp_mode, b->trans_type, b->trans_dur_ms,
		   b->initial_full_refresh ? "initial" : "off",
		   b->full_refresh_frames > 0 ? ", periodic" : "");

	wl_list_insert(&compositor->backend_list, &b->base.link);

	b->base.supported_presentation_clocks =
		WESTON_PRESENTATION_CLOCKS_SOFTWARE;

	b->base.destroy = ebc_destroy;
	b->base.create_output = ebc_output_create;

	if (b->renderer != WESTON_RENDERER_AUTO &&
	    b->renderer != WESTON_RENDERER_PIXMAN)
		weston_log("ebc: only the pixman renderer is supported, "
			   "forcing pixman\n");

	if (!compositor->renderer) {
		ret = weston_compositor_init_renderer(compositor,
						      WESTON_RENDERER_PIXMAN,
						      NULL);
		if (ret < 0) {
			weston_log("ebc: failed to initialize the pixman "
				   "renderer\n");
			goto err_backend;
		}
	}

	/* libinput input (touchscreen/keys) through udev, non-fatal.
	 * libinput opens /dev/input/* through the shared launcher, so create
	 * it first (drm-backend does the same); without a launcher
	 * (logind/seatd unavailable) we run without input. */
	seat_id = config->seat_id ? config->seat_id : "seat0";
	compositor->launcher = weston_launcher_connect(compositor, seat_id,
						       false);
	if (!compositor->launcher)
		weston_log("ebc: no launcher available, running without "
			   "libinput input\n");

	b->udev = udev_new();
	if (b->udev && compositor->launcher) {
		if (udev_input_init(&b->input, compositor, b->udev,
				    seat_id, NULL) == 0) {
			b->input_initialized = true;
		} else {
			weston_log("ebc: failed to initialize input devices, "
				   "continuing without input\n");
		}
	}

	/* Debug key F5 forces a full refresh; needs WESTON_DEBUG_BINDING=1. */
	b->full_refresh_binding =
		weston_compositor_add_debug_binding(compositor, KEY_F5,
						    ebc_full_refresh_binding, b);

	/* Announce the panel; the compositor creates and enables the output
	 * through the deferred heads-changed signal. */
	if (ebc_head_create(b, "ebc") < 0) {
		weston_log("ebc: failed to create head\n");
		goto err_backend;
	}

	return b;

err_backend:
	if (b->full_refresh_binding)
		weston_binding_destroy(b->full_refresh_binding);
	if (b->input_initialized)
		udev_input_destroy(&b->input);
	if (b->udev)
		udev_unref(b->udev);
	if (compositor->launcher) {
		weston_launcher_destroy(compositor->launcher);
		compositor->launcher = NULL;
	}
	wl_list_remove(&b->base.link);
err_client:
	rkebc_destroy_client(b->node);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_ebc_backend_config *config)
{
	config->renderer = WESTON_RENDERER_PIXMAN;
	config->seat_id = NULL;
	config->disp_mode_name = "auto";
	config->trans_type = 0;
	config->trans_dur_ms = 0;
	config->initial_full_refresh = true;
	config->full_refresh_frames = 0;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct ebc_backend *b;
	struct weston_ebc_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_EBC_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_ebc_backend_config)) {
		weston_log("ebc backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = ebc_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
