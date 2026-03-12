/**
 * \file primary_plane.c
 * \brief Primary DRM plane implementation
 * \author Michael Forney
 * \date 2013-2014, 2016
 * \copyright MIT
 *
 * This file implements a simple "primary plane" which attaches framebuffers
 * obtained from a compositor's buffer system to a DRM CRTC. The implementation
 * presents frames either via a modeset or a pageflip and notifies the view of
 * frame completion.
 */

/*
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "primary_plane.h"
#include "drm.h"
#include "event.h"
#include "internal.h"
#include "launch.h"
#include "util.h"

#include <errno.h>
#include <wld/drm.h>
#include <wld/wld.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/**
 * \brief Update callback for the primary plane's view implementation.
 *
 * This function is called by the view system when an update may be required.
 * The primary plane does not maintain complex state here and therefore simply
 * returns true to indicate the update was processed. Keep this function
 * lightweight since it may be invoked frequently.
 *
 * \param view The view being updated (must not be NULL).
 * \return true when update completed successfully, false otherwise.
 */
static bool
update(struct view *view)
{
	if (!view) {
		ERROR("update(): view is NULL\n");
		return false;
	}

	/* Currently no per-frame updates are required for the primary plane. */
	return true;
}

/**
 * \brief Idle callback to send a frame to the view system.
 *
 * This callback is scheduled after a successful modeset so that the view is
 * given a chance to render the first frame. It simply forwards the current
 * timestamp to view_frame().
 *
 * \param data A pointer to struct primary_plane.
 */
static void
send_frame(void *data)
{
	struct primary_plane *plane = data;

	if (!plane) {
		ERROR("send_frame(): plane is NULL\n");
		return;
	}

	view_frame(&plane->view, get_time());
}

/**
 * \brief Attach a DRM framebuffer derived from a wld_buffer to this plane.
 *
 * Attaches the provided buffer's framebuffer to the plane either by performing
 * a full modeset (when needed) or by requesting a page flip. On success
 * schedules the frame notification and returns 0. On failure returns a negative
 * error value and logs an explanatory message.
 *
 * \param view The view for this primary plane.
 * \param buffer The buffer containing the DRM framebuffer object.
 * \return 0 on success or a negative errno on failure.
 */
static int
attach(struct view *view, struct wld_buffer *buffer)
{
	struct primary_plane *plane = wl_container_of(view, plane, view);
	uint32_t fb;
	int ret;

	if (!view || !buffer || !plane) {
		ERROR("attach(): invalid argument(s)\n");
		return -EINVAL;
	}

	fb = drm_get_framebuffer(buffer);
	if (!fb) {
		ERROR("attach(): buffer has no valid framebuffer\n");
		return -EINVAL;
	}

	if (plane->need_modeset) {
		ret = drmModeSetCrtc(swc.drm->fd, plane->crtc, fb, 0, 0,
		                     (uint32_t *)plane->connectors.data,
		                     plane->connectors.size / sizeof(uint32_t),
		                     &plane->mode.info);

		if (ret < 0) {
			ERROR("Could not set CRTC to next framebuffer: %s\n", strerror(errno));
			return -errno ? -errno : -EIO;
		}

		/* Schedule the first frame after a successful modeset. */
		wl_event_loop_add_idle(swc.event_loop, send_frame, plane);
		plane->need_modeset = false;
	} else {
		/* Perform a page flip for fast buffer swap. */
		ret = drmModePageFlip(swc.drm->fd, plane->crtc, fb, DRM_MODE_PAGE_FLIP_EVENT,
		                      &plane->drm_handler);

		if (ret < 0) {
			ERROR("Page flip failed: %s\n", strerror(errno));
			return -errno ? -errno : -EIO;
		}
	}

	return 0;
}

/**
 * \brief Move the view associated with this primary plane.
 *
 * The primary plane supports setting the position in the view geometry. In
 * practice this is rarely used for a fullscreen primary plane but is provided
 * for completeness.
 *
 * \param view View to move.
 * \param x New X position.
 * \param y New Y position.
 * \return true on success.
 */
static bool
move(struct view *view, int32_t x, int32_t y)
{
	if (!view) {
		ERROR("move(): view is NULL\n");
		return false;
	}

	view_set_position(view, x, y);
	return true;
}

static const struct view_impl view_impl = {
	.update = update,
	.attach = attach,
	.move = move,
};

/**
 * \brief DRM page flip event handler.
 *
 * Called by the DRM event handling code when a page flip completes. The handler
 * forwards the timestamp to view_frame so the view can schedule the next frame.
 *
 * \param handler DRM handler embedded in struct primary_plane.
 * \param time Timestamp supplied by the DRM event.
 */
static void
handle_page_flip(struct drm_handler *handler, uint32_t time)
{
	struct primary_plane *plane = wl_container_of(handler, plane, drm_handler);

	if (!plane) {
		ERROR("handle_page_flip(): plane is NULL\n");
		return;
	}

	view_frame(&plane->view, time);
}

/**
 * \brief Listener for swc events.
 *
 * Currently only reacts to activation events by marking the plane for a modeset
 * on the next attach so that the CRTC configuration is refreshed.
 *
 * \param listener The wl_listener that received the event (embedded in struct
 *                 primary_plane).
 * \param data Event data (expected to be struct event *).
 */
static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	struct event *event = data;
	struct primary_plane *plane = wl_container_of(listener, plane, swc_listener);

	if (!event || !plane) {
		ERROR("handle_swc_event(): invalid args\n");
		return;
	}

	switch (event->type) {
	case SWC_EVENT_ACTIVATED:
		plane->need_modeset = true;
		break;
	default:
		/* Unknown events are ignored. */
		break;
	}
}

/**
 * \brief Initialize a primary plane structure.
 *
 * This sets up a primary plane that will control the specified CRTC and
 * connectors. The original CRTC state is stored so it can be restored in
 * \ref primary_plane_finalize().
 *
 * \param plane Output pointer to the primary_plane structure to initialize.
 * \param crtc The CRTC id this plane will drive.
 * \param mode The desired mode to use on modeset.
 * \param connectors Array of connector ids to attach to the CRTC.
 * \param num_connectors Number of entries in \p connectors.
 * \return true on success, false on failure.
 */
bool
primary_plane_initialize(struct primary_plane *plane, uint32_t crtc, struct mode *mode, uint32_t *connectors, uint32_t num_connectors)
{
	uint32_t *plane_connectors;

	if (!plane || !mode || (!connectors && num_connectors > 0)) {
		ERROR("primary_plane_initialize(): invalid argument(s)\n");
		return false;
	}

	plane->original_crtc_state = drmModeGetCrtc(swc.drm->fd, crtc);
	if (!plane->original_crtc_state) {
		ERROR("Failed to get CRTC state for CRTC %u: %s\n", crtc, strerror(errno));
		return false;
	}

	wl_array_init(&plane->connectors);
	plane_connectors = wl_array_add(&plane->connectors, num_connectors * sizeof(connectors[0]));

	if (!plane_connectors) {
		ERROR("Failed to allocate connector array\n");
		drmModeFreeCrtc(plane->original_crtc_state);
		wl_array_release(&plane->connectors);
		return false;
	}

	memcpy(plane_connectors, connectors, num_connectors * sizeof(connectors[0]));
	plane->crtc = crtc;
	plane->need_modeset = true;
	view_initialize(&plane->view, &view_impl);
	plane->view.geometry.width = mode->width;
	plane->view.geometry.height = mode->height;
	plane->drm_handler.page_flip = &handle_page_flip;
	plane->swc_listener.notify = &handle_swc_event;
	plane->mode = *mode;
	wl_signal_add(&swc.event_signal, &plane->swc_listener);

	return true;
}

/**
 * \brief Finalize a primary plane and restore the original CRTC configuration.
 *
 * This function attempts to restore the CRTC to the state it had when the plane
 * was initialized. Any resources allocated by primary_plane_initialize() are
 * released.
 *
 * \param plane The primary_plane to finalize (must not be NULL).
 */
void
primary_plane_finalize(struct primary_plane *plane)
{
	drmModeCrtcPtr crtc;

	if (!plane)
		return;

	/* Release connector array memory first. */
	wl_array_release(&plane->connectors);

	crtc = plane->original_crtc_state;

	/* Nothing to restore. */
	if (!crtc) {
		return;
	}

	/* Attempt to restore the original CRTC configuration */
	if (drmModeSetCrtc(swc.drm->fd, crtc->crtc_id, crtc->buffer_id,
	                   crtc->x, crtc->y, NULL, 0, &crtc->mode)
	    < 0) {
		ERROR("Failed to restore original CRTC state: %s\n", strerror(errno));
	}

	drmModeFreeCrtc(crtc);
	plane->original_crtc_state = NULL;
}
