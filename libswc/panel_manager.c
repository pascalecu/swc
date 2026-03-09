/**
 * \file panel_manager.c
 * \brief Implementation of the panel manager Wayland global.
 * \author Michael Forney
 * \date 2014–-2020
 * \copyright MIT
 *
 * This module implements the `swc_panel_manager` Wayland global. It allows
 * clients to create panel objects associated with existing surfaces.
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

#include "panel_manager.h"
#include "internal.h"
#include "panel.h"

#include "swc-server-protocol.h"
#include <wayland-server.h>

/**
 * \brief Handle the create_panel request from a client.
 *
 * Creates a new panel object associated with the given surface.
 *
 * \param client Client issuing the request.
 * \param resource Panel manager resource.
 * \param id New object id for the panel.
 * \param surface_resource Wayland surface resource.
 */
static void
create_panel(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	if (!client)
		return;

	if (!surface_resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct surface *surface = wl_resource_get_user_data(surface_resource);

	if (!panel_new(client, wl_resource_get_version(resource), id, surface))
		wl_client_post_no_memory(client);
}

static const struct swc_panel_manager_interface panel_manager_impl = {
	.create_panel = create_panel,
};

/**
 * \brief Bind handler for the panel manager global.
 *
 * Creates a swc_panel_manager resource for the client and installs the request
 * implementation.
 *
 * \param client Client binding to the global.
 * \param data Unused user data.
 * \param version Requested interface version.
 * \param id Resource id.
 */
static void
bind_panel_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &swc_panel_manager_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &panel_manager_impl, NULL, NULL);
}

/**
 * \brief Create the panel manager global.
 *
 * Registers the `swc_panel_manager` global on the given Wayland display,
 * allowing clients to request panel objects.
 *
 * \param display Wayland display to register the global on.
 * \return The created wl_global object, or NULL on failure.
 */
struct wl_global *
panel_manager_create(struct wl_display *display)
{
	if (!display)
		return NULL;

	return wl_global_create(display,
	                        &swc_panel_manager_interface,
	                        1,
	                        NULL,
	                        &bind_panel_manager);
}
