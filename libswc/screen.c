/**
 * \file screen.c
 * \brief Screen management and initialization for the SWC Wayland compositor.
 * \author Michael Forney
 * \date 2013–2014
 * \copyright MIT
 *
 * This file handles the creation, destruction, and geometry management of
 * physical screens (displays) managed by the compositor, including their
 * Wayland protocol bindings and pointer interactions.
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

#include "screen.h"
#include "drm.h"
#include "event.h"
#include "internal.h"
#include "mode.h"
#include "output.h"
#include "plane.h"
#include "pointer.h"
#include "util.h"

#include "swc-server-protocol.h"
#include <stdlib.h>

#define INTERNAL(s) ((struct screen *)(s))

static struct screen *active_screen;
static const struct swc_screen_handler null_handler;

static bool handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t x, wl_fixed_t y);

struct pointer_handler screens_pointer_handler = {
	.motion = handle_motion,
};

/**
 * \brief Assigns a custom handler to a specific screen.
 * \param base The base swc screen struct.
 * \param handler The handler containing callback functions for screen events.
 * \param data User-defined data to pass to the handler callbacks.
 */
EXPORT void
swc_screen_set_handler(struct swc_screen *base, const struct swc_screen_handler *handler, void *data)
{
	struct screen *screen = INTERNAL(base);

	/* Fallback to null_handler if NULL is passed to prevent dereference crashes */
	screen->handler = handler ? handler : &null_handler;
	screen->handler_data = data;
}

/**
 * \brief Initializes the global screen list and requests DRM to create screens.
 * \return true if screens were successfully created and initialized, false
 * otherwise.
 */
bool
screens_initialize(void)
{
	wl_list_init(&swc.screens);

	if (!drm_create_screens(&swc.screens))
		return false;

	return !wl_list_empty(&swc.screens);
}

/**
 * \brief Destroys all currently active screens and cleans up the screen list.
 */
void
screens_finalize(void)
{
	struct screen *screen, *tmp;

	wl_list_for_each_safe (screen, tmp, &swc.screens, link)
		screen_destroy(screen);
}

/**
 * \brief Binds a Wayland client to a screen resource.
 * \param client The Wayland client requesting the bind.
 * \param data The screen struct being bound.
 * \param version The protocol version.
 * \param id The ID for the new resource.
 */
static void
bind_screen(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct screen *screen = data;
	if (!screen)
		return;

	struct wl_resource *resource = wl_resource_create(client, &swc_screen_interface, version, id);

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, NULL, screen, &remove_resource);
	wl_list_insert(&screen->resources, wl_resource_get_link(resource));
}

/**
 * \brief Allocates and initializes a new screen.
 * \param crtc The DRM CRTC ID associated with this screen.
 * \param output The primary output tied to this screen.
 * \param cursor_plane The plane to be used for hardware cursors.
 * \return A pointer to the newly allocated screen, or NULL on failure.
 */
struct screen *
screen_new(uint32_t crtc, struct output *output, struct plane *cursor_plane)
{
	struct screen *screen;
	int32_t x = 0;

	/* Simple heuristic for initial screen positioning. */
	wl_list_for_each (screen, &swc.screens, link)
		x = MAX(x, screen->base.geometry.x + screen->base.geometry.width);

	if (!(screen = malloc(sizeof(*screen))))
		return NULL;

	screen->global = wl_global_create(swc.display, &swc_screen_interface, 1, screen, &bind_screen);

	if (!screen->global) {
		ERROR("Failed to create screen global\n");
		free(screen);
		return NULL;
	}

	screen->crtc = crtc;

	if (!primary_plane_initialize(&screen->planes.primary, crtc, output->preferred_mode, &output->connector, 1)) {
		ERROR("Failed to initialize primary plane\n");
		wl_global_destroy(screen->global);
		free(screen);
		return NULL;
	}

	cursor_plane->screen = screen;
	screen->planes.cursor = cursor_plane;

	screen->handler = &null_handler;
	wl_signal_init(&screen->destroy_signal);
	wl_list_init(&screen->resources);
	wl_list_init(&screen->outputs);
	wl_list_insert(&screen->outputs, &output->link);
	wl_list_init(&screen->modifiers);

	view_move(&screen->planes.primary.view, x, 0);
	screen->base.geometry = screen->planes.primary.view.geometry;
	screen->base.usable_geometry = screen->base.geometry;

	swc.manager->new_screen(&screen->base);

	return screen;
}

/**
 * \brief Tears down a screen, cleaning up its planes, outputs, and signals.
 * \param screen The screen to destroy.
 */
void
screen_destroy(struct screen *screen)
{
	struct output *output, *next;

	if (active_screen == screen)
		active_screen = NULL;

	if (screen->handler->destroy)
		screen->handler->destroy(screen->handler_data);

	wl_signal_emit(&screen->destroy_signal, NULL);
	wl_list_for_each_safe (output, next, &screen->outputs, link)
		output_destroy(output);

	primary_plane_finalize(&screen->planes.primary);
	plane_destroy(screen->planes.cursor);
	free(screen);
}

/**
 * \brief Recalculates the usable geometry of a screen by applying screen modifiers.
 *
 * This is used to reserve space for panels, docks, or other UI elements that
 * reduce the effective screen area available for standard windows.
 *
 * \param screen The screen whose geometry needs to be updated.
 */
void
screen_update_usable_geometry(struct screen *screen)
{
	pixman_region32_t total_usable, usable;
	pixman_box32_t *extents;
	struct screen_modifier *modifier;
	struct swc_rectangle *geom = &screen->base.geometry;

	DEBUG("Updating usable geometry\n");

	pixman_region32_init_rect(&total_usable, geom->x, geom->y, geom->width, geom->height);
	pixman_region32_init(&usable);

	wl_list_for_each (modifier, &screen->modifiers, link) {
		modifier->modify(modifier, geom, &usable);
		pixman_region32_intersect(&total_usable, &total_usable, &usable);
	}

	extents = pixman_region32_extents(&total_usable);

	if (extents->x1 != screen->base.usable_geometry.x
	    || extents->y1 != screen->base.usable_geometry.y
	    || (extents->x2 - extents->x1) != screen->base.usable_geometry.width
	    || (extents->y2 - extents->y1) != screen->base.usable_geometry.height) {
		screen->base.usable_geometry.x = extents->x1;
		screen->base.usable_geometry.y = extents->y1;
		screen->base.usable_geometry.width = extents->x2 - extents->x1;
		screen->base.usable_geometry.height = extents->y2 - extents->y1;

		if (screen->handler->usable_geometry_changed)
			screen->handler->usable_geometry_changed(screen->handler_data);
	}

	pixman_region32_fini(&usable);
	pixman_region32_fini(&total_usable);
}

/**
 * \brief Handles pointer motion to determine and update the active screen.
 * \param handler The pointer handler context.
 * \param time The timestamp of the motion event.
 * \param fx The fixed-point X coordinate of the pointer.
 * \param fy The fixed-point Y coordinate of the pointer.
 * \return Always returns false, allowing the event to propagate further if needed.
 */
bool
handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx, wl_fixed_t fy)
{
	struct screen *screen;
	int32_t x = wl_fixed_to_int(fx), y = wl_fixed_to_int(fy);

	wl_list_for_each (screen, &swc.screens, link) {
		if (rectangle_contains_point(&screen->base.geometry, x, y)) {
			if (screen != active_screen) {
				active_screen = screen;

				if (screen->handler->entered)
					screen->handler->entered(screen->handler_data);
			}
			break;
		}
	}

	return false;
}
