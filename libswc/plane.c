/**
 * \file plane.c
 * \brief Implementation of DRM plane management and view integration.
 * \author Michael Forney
 * \date 2019
 * \copyright MIT
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

#include "plane.h"
#include "drm.h"
#include "event.h"
#include "internal.h"
#include "screen.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <wld/drm.h>
#include <wld/wld.h>
#include <xf86drmMode.h>

/**
 * \brief Internal enumeration of known DRM plane properties.
 */
enum plane_property {
	PLANE_TYPE,
	PLANE_IN_FENCE_FD,
	PLANE_CRTC_ID,
	PLANE_CRTC_X,
	PLANE_CRTC_Y,
	PLANE_CRTC_W,
	PLANE_CRTC_H,
	PLANE_SRC_X,
	PLANE_SRC_Y,
	PLANE_SRC_W,
	PLANE_SRC_H,
	PLANE_COUNT
};

/**
 * \brief Updates the DRM plane's physical state.
 * \param view The view base class of the plane to update.
 * \return true on success, false on failure.
 */
static bool
update(struct view *view)
{
	struct plane *plane = wl_container_of(view, plane, view);

	/* Don't touch hardware if the compositor is inactive or not assigned a screen */
	if (!plane->screen || !swc.active)
		return false;

	const int32_t dx = view->geometry.x - plane->screen->base.geometry.x;
	const int32_t dy = view->geometry.y - plane->screen->base.geometry.y;
	const uint32_t dw = view->geometry.width;
	const uint32_t dh = view->geometry.height;

	const uint32_t sx = 0;
	const uint32_t sy = 0;
	const uint32_t sw = dw << 16;
	const uint32_t sh = dh << 16;

	/* Guard against invalid geometries that might cause kernel errors */
	if (dw == 0 || dh == 0)
		return true;

	if (drmModeSetPlane(swc.drm->fd, plane->id, plane->screen->crtc,
	                    plane->fb, 0, dx, dy, dw, dh,
	                    sx, sy, sw, sh)
	    < 0) {
		ERROR("Failed to update DRM plane %u: %s\n", plane->id, strerror(errno));
		return false;
	}

	return true;
}

/**
 * \brief Attaches a WLD buffer to the plane.
 * \param view The view base class of the plane.
 * \param buffer The buffer to attach.
 * \return 0 on success.
 */
static int
attach(struct view *view, struct wld_buffer *buffer)
{
	struct plane *plane = wl_container_of(view, plane, view);

	plane->fb = drm_get_framebuffer(buffer);
	view_set_size_from_buffer(view, buffer);
	return 0;
}

/**
 * \brief Updates the view coordinates of the plane.
 * \param view The view base class of the plane.
 * \param x The new X coordinate.
 * \param y The new Y coordinate.
 * \return always returns true.
 */
static bool
move(struct view *view, int32_t x, int32_t y)
{
	view_set_position(view, x, y);
	return true;
}

static const struct view_impl view_impl = {
	.update = update,
	.attach = attach,
	.move = move,
};

/**
 * \brief Finds the internal enumeration value for a given DRM property name.
 * \param name The string name of the DRM property.
 * \return Property index or -1 if unknown.
 */
static int
find_prop_index(const char *name)
{
	static const char property_names[][16] = {
		[PLANE_TYPE] = "type",
		[PLANE_IN_FENCE_FD] = "IN_FENCE_FD",
		[PLANE_CRTC_ID] = "CRTC_ID",
		[PLANE_CRTC_X] = "CRTC_X",
		[PLANE_CRTC_Y] = "CRTC_Y",
		[PLANE_CRTC_W] = "CRTC_W",
		[PLANE_CRTC_H] = "CRTC_H",
		[PLANE_SRC_X] = "SRC_X",
		[PLANE_SRC_Y] = "SRC_Y",
		[PLANE_SRC_W] = "SRC_W",
		[PLANE_SRC_H] = "SRC_H",
	};

	for (size_t i = 0; i < PLANE_COUNT; ++i) {
		if (property_names[i] && strcmp(name, property_names[i]) == 0)
			return (int)i;
	}
	return -1;
}

/**
 * \brief Handles global compositor events for the plane.
 * \param listener The listener triggered by the event.
 * \param data The event data payload.
 */
static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	struct event *event = data;
	struct plane *plane = wl_container_of(listener, plane, swc_listener);

	if (event->type == SWC_EVENT_ACTIVATED)
		update(&plane->view);
}

/**
 * \brief Helper to find the hardware type of a DRM plane (Primary, Overlay, or Cursor).
 */
static void
get_plane_type(int fd, struct plane *plane, drmModeObjectProperties *props)
{
	for (uint32_t i = 0; i < props->count_props; ++i) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			continue;

		int index = find_prop_index(prop->name);
		if (index == PLANE_TYPE) {
			plane->type = props->prop_values[i];
			drmModeFreeProperty(prop);
			break;
		}

		drmModeFreeProperty(prop);
	}
}

/**
 * \brief Allocates and initializes a new hardware plane.
 * \param id The DRM object ID for the plane.
 * \return A pointer to the newly allocated plane, or NULL on failure.
 */
struct plane *
plane_new(uint32_t id)
{
	struct plane *plane;
	drmModePlane *drm_plane;
	drmModeObjectProperties *props;

	if (!(plane = calloc(1, sizeof(*plane))))
		return NULL;

	drm_plane = drmModeGetPlane(swc.drm->fd, id);
	if (!drm_plane) {
		free(plane);
		return NULL;
	}

	plane->id = id;
	plane->possible_crtcs = drm_plane->possible_crtcs;
	plane->type = -1;
	drmModeFreePlane(drm_plane);

	props = drmModeObjectGetProperties(swc.drm->fd, id, DRM_MODE_OBJECT_PLANE);
	if (props) {
		get_plane_type(swc.drm->fd, plane, props);
		drmModeFreeObjectProperties(props);
	}

	plane->swc_listener.notify = handle_swc_event;
	wl_signal_add(&swc.event_signal, &plane->swc_listener);
	view_initialize(&plane->view, &view_impl);

	return plane;
}

/**
 * \brief Destroys a plane and frees associated resources.
 * \param plane The plane to destroy.
 */
void
plane_destroy(struct plane *plane)
{
	if (!plane)
		return;

	wl_list_remove(&plane->swc_listener.link);
	free(plane);
}
