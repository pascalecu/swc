/**
 * \file panel.c
 * \brief Implementation of the swc_panel surface role.
 *
 * \author Michael Forney
 * \date 2014-2019
 * \copyright MIT
 *
 * This module implements the server side of the `swc_panel` protocol.
 *
 * A panel is a special surface role intended for desktop UI elements such as
 * taskbars, docks, and status bars. Panels attach to an edge of a screen and
 * may reserve part of the screen's geometry so that normal application windows
 * do not overlap them.
 *
 * Internally a panel consists of:
 *
 *  - a Wayland resource implementing the `swc_panel` interface
 *  - a compositor view used to render the surface
 *  - a screen modifier used to adjust the screen's usable region
 *
 * When docked, the panel positions its view along the selected screen edge and
 * optionally reserves space ("strut") from the usable screen geometry.
 *
 * The panel object automatically follows the lifetime of the associated Wayland
 * surface. If the surface is destroyed, the panel resource is destroyed as
 * well.
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

#include "panel.h"
#include "compositor.h"
#include "internal.h"
#include "keyboard.h"
#include "output.h"
#include "screen.h"
#include "seat.h"
#include "surface.h"
#include "util.h"
#include "view.h"

#include "swc-server-protocol.h"
#include <assert.h>
#include <stdlib.h>

/**
 * \brief Internal representation of a panel.
 *
 * A panel wraps a compositor view and tracks the state required to dock
 * the view to a screen edge. Panels may optionally reserve part of the
 * screen's usable region via the screen modifier system.
 */
struct panel {
	/** Wayland resource implementing the swc_panel interface */
	struct wl_resource *resource;

	/** Listener used to detect when the underlying surface is destroyed */
	struct wl_listener surface_destroy_listener;

	/** Compositor view used to display the panel surface */
	struct compositor_view *view;

	/** Handles events emitted by the view (currently resize events) */
	struct view_handler view_handler;

	/** Screen the panel is currently docked to */
	struct screen *screen;

	/** Screen modifier used to adjust usable screen geometry */
	struct screen_modifier modifier;

	/** Edge the panel is docked to (top/bottom/left/right) */
	uint32_t edge;

	/** Offset from the origin of the selected edge */
	uint32_t offset;

	/** Amount of space reserved from the usable screen region */
	uint32_t strut_size;

	/** Indicates whether the panel is currently docked */
	bool docked;
};

/**
 * \brief Update the panel's position.
 *
 * Recomputes the panel's position based on its edge, offset, and the geometry
 * of the associated screen and view.
 */
static void
update_position(struct panel *panel)
{
	if (!panel || !panel->screen)
		return;

	struct swc_rectangle *screen = &panel->screen->base.geometry;
	struct swc_rectangle *view = &panel->view->base.geometry;

	int32_t x = screen->x;
	int32_t y = screen->y;

	switch (panel->edge) {
	case SWC_PANEL_EDGE_TOP:
		x += panel->offset;
		break;

	case SWC_PANEL_EDGE_BOTTOM:
		x += panel->offset;
		y += screen->height - view->height;
		break;

	case SWC_PANEL_EDGE_LEFT:
		y += screen->height - view->height - panel->offset;
		break;

	case SWC_PANEL_EDGE_RIGHT:
		x += screen->width - view->width;
		y += panel->offset;
		break;

	default:
		return;
	}

	view_move(&panel->view->base, x, y);
}

/**
 * \brief Dock the panel to a screen edge.
 *
 * Attaches the panel to the specified edge of a screen and updates its
 * position. If requested, the panel may also receive keyboard focus.
 *
 * The screen's usable geometry is updated through the screen modifier mechanism
 * when the panel reserves space.
 */
static void
dock(struct wl_client *client, struct wl_resource *resource, uint32_t edge, struct wl_resource *screen_resource, uint32_t focus)
{
	(void)client;

	struct panel *panel = wl_resource_get_user_data(resource);
	struct screen *screen;
	uint32_t length;

	if (screen_resource)
		screen = wl_resource_get_user_data(screen_resource);
	else
		screen = wl_container_of(swc.screens.next, screen, link);

	switch (edge) {
	case SWC_PANEL_EDGE_TOP:
	case SWC_PANEL_EDGE_BOTTOM:
		length = screen->base.geometry.width;
		break;

	case SWC_PANEL_EDGE_LEFT:
	case SWC_PANEL_EDGE_RIGHT:
		length = screen->base.geometry.height;
		break;

	default:
		return;
	}

	if (panel->screen && screen != panel->screen) {
		wl_list_remove(&panel->modifier.link);
		screen_update_usable_geometry(panel->screen);
	}

	panel->screen = screen;
	panel->edge = edge;
	panel->docked = true;

	update_position(panel);

	compositor_view_show(panel->view);
	wl_list_insert(&screen->modifiers, &panel->modifier.link);

	if (focus)
		keyboard_set_focus(swc.seat->keyboard, panel->view);

	swc_panel_send_docked(resource, length);
}

/**
 * \brief Set the panel offset.
 *
 * Updates the panel's offset along the edge it is docked to. If the panel is
 * already docked, its position is immediately updated.
 */
static void
set_offset(struct wl_client *client, struct wl_resource *resource, uint32_t offset)
{
	struct panel *panel = wl_resource_get_user_data(resource);

	panel->offset = offset;
	if (panel->docked)
		update_position(panel);
}

/**
 * \brief Set the panel strut size.
 *
 * Defines how much space the panel reserves from the screen's usable region.
 * This ensures normal windows avoid overlapping the panel.
 */
static void
set_strut(struct wl_client *client, struct wl_resource *resource, uint32_t size, uint32_t begin, uint32_t end)
{
	struct panel *panel = wl_resource_get_user_data(resource);

	panel->strut_size = size;
	if (panel->docked)
		screen_update_usable_geometry(panel->screen);
}

static const struct swc_panel_interface panel_impl = {
	.dock = dock,
	.set_offset = set_offset,
	.set_strut = set_strut,
};

/**
 * \brief Handle view resize events.
 *
 * When the panel surface changes size, the panel position is recomputed to keep
 * it aligned to the configured edge.
 */
static void
handle_resize(struct view_handler *handler, uint32_t old_width, uint32_t old_height)
{
	struct panel *panel = wl_container_of(handler, panel, view_handler);
	update_position(panel);
}

static const struct view_handler_impl view_handler_impl = {
	.resize = handle_resize,
};

/**
 * \brief Modify the screen usable region.
 *
 * Panels can reserve space along a screen edge. This function updates the
 * usable region accordingly so other windows avoid overlapping the panel.
 */
static void
modify(struct screen_modifier *modifier, const struct swc_rectangle *geom, pixman_region32_t *usable)
{
	struct panel *panel = wl_container_of(modifier, panel, modifier);
	pixman_box32_t box = {
		.x1 = geom->x,
		.y1 = geom->y,
		.x2 = geom->x + geom->width,
		.y2 = geom->y + geom->height
	};

	assert(panel->docked);

	DEBUG("Original geometry { x1: %d, y1: %d, x2: %d, y2: %d }\n",
	      box.x1, box.y1, box.x2, box.y2);

	switch (panel->edge) {
	case SWC_PANEL_EDGE_TOP:
		box.y1 = MAX(box.y1, geom->y + panel->strut_size);
		break;
	case SWC_PANEL_EDGE_BOTTOM:
		box.y2 = MIN(box.y2, geom->y + geom->height - panel->strut_size);
		break;
	case SWC_PANEL_EDGE_LEFT:
		box.x1 = MAX(box.x1, geom->x + panel->strut_size);
		break;
	case SWC_PANEL_EDGE_RIGHT:
		box.x2 = MIN(box.x2, geom->x + geom->width - panel->strut_size);
		break;
	}

	DEBUG("Usable region { x1: %d, y1: %d, x2: %d, y2: %d }\n",
	      box.x1, box.y1, box.x2, box.y2);

	pixman_region32_reset(usable, &box);
}

/**
 * \brief Destroy a panel.
 *
 * Called when the Wayland resource is destroyed. This removes the panel from
 * the screen modifier list, destroys its view, and frees resources.
 */
static void
destroy_panel(struct wl_resource *resource)
{
	struct panel *panel = wl_resource_get_user_data(resource);

	if (panel->docked) {
		wl_list_remove(&panel->modifier.link);
		screen_update_usable_geometry(panel->screen);
	}

	wl_list_remove(&panel->surface_destroy_listener.link);

	compositor_view_destroy(panel->view);
	free(panel);
}

/**
 * \brief Handle destruction of the associated surface.
 *
 * When the underlying surface is destroyed, the panel resource is destroyed as
 * well.
 */
static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct panel *panel = wl_container_of(listener, panel, surface_destroy_listener);
	wl_resource_destroy(panel->resource);
}

/**
 * \brief Create a panel for a surface.
 *
 * Assigns the panel role to the specified surface and creates the
 * associated compositor view.
 *
 * The returned object is owned by the Wayland resource created for the
 * client. It will be destroyed automatically when the client destroys
 * the panel resource or when the underlying surface is destroyed.
 *
 * \param client Wayland client requesting the panel.
 * \param version Version of the `swc_panel` interface.
 * \param id Resource identifier for the new panel object.
 * \param surface Surface that will back the panel view.
 *
 * \return Pointer to the newly created panel on success, or NULL if
 *         allocation or initialization fails.
 */
struct panel *
panel_new(struct wl_client *client, uint32_t version, uint32_t id, struct surface *surface)
{
	struct panel *panel;

	panel = calloc(1, sizeof(*panel));
	if (!panel)
		return NULL;

	panel->resource = wl_resource_create(client, &swc_panel_interface, version, id);
	if (!panel->resource)
		goto error;

	panel->view = compositor_create_view(surface);
	if (!panel->view)
		goto error_resource;

	wl_resource_set_implementation(panel->resource, &panel_impl, panel, destroy_panel);

	panel->surface_destroy_listener.notify = handle_surface_destroy;
	panel->view_handler.impl = &view_handler_impl;
	panel->modifier.modify = modify;

	wl_list_insert(&panel->view->base.handlers, &panel->view_handler.link);
	wl_resource_add_destroy_listener(surface->resource, &panel->surface_destroy_listener);

	return panel;

error_resource:
	wl_resource_destroy(panel->resource);
error:
	free(panel);
	return NULL;
}
