/**
 * \file kde_decoration.c
 * \brief Implementation of the KDE server-side decoration manager.
 *
 * \author Michael Forney
 * \date 2020
 * \copyright MIT
 *
 * Provides the Wayland global and per-resource handlers required to
 * advertise server-side decoration support to clients via the
 * org_kde_kwin_server_decoration_manager protocol.
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

#include "kde_decoration.h"
#include "util.h"

#include "server-decoration-server-protocol.h"
#include <wayland-server.h>

/**
 * \brief Handles a client's request to change the decoration mode.
 *
 * \param[in] client The wl_client issuing the request.
 * \param[in] resource The decoration resource associated with the toplevel.
 * \param[in] mode The requested decoration mode.
 */
static void
handle_request_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode)
{
	(void)client;

	/* The server is required to acknowledge the requested mode. We currently
	 * enforce server-side decorations, so we simply echo the mode back to the
	 * client to satisfy the protocol's requirements. */
	org_kde_kwin_server_decoration_send_mode(resource, mode);
}

static const struct org_kde_kwin_server_decoration_interface decoration_impl = {
	.release = destroy_resource,
	.request_mode = handle_request_mode,
};

/**
 * \brief Creates a new server-side decoration resource for a toplevel.
 *
 * \param[in] client The wl_client that requested the decoration.
 * \param[in] resource The manager resource from which the create request was sent.
 * \param[in] id The ID to use for the new decoration resource.
 * \param[in] toplevel_resource The wl_resource representing the client's toplevel.
 */
static void
handle_create(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel_resource)
{
	struct wl_resource *decoration =
	    wl_resource_create(client,
	                       &org_kde_kwin_server_decoration_interface,
	                       wl_resource_get_version(resource), id);

	if (!decoration) {
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(decoration, &decoration_impl, NULL, NULL);

	/* Immediately notify the client that the compositor enforces server-side decorations. */
	org_kde_kwin_server_decoration_send_mode(
	    decoration, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

static const struct org_kde_kwin_server_decoration_manager_interface decoration_manager_impl = {
	.create = handle_create,
};

/**
 * \brief Bind handler for the decoration manager global.
 *
 * \param[in] client The wl_client binding the global.
 * \param[in] data Opaque user data supplied when the global was created (unused).
 * \param[in] version The protocol version requested by the client.
 * \param[in] id The ID for the created manager resource.
 */
static void
bind_decoration_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;

	if (!client)
		return;

	struct wl_resource *resource = wl_resource_create(
	    client, &org_kde_kwin_server_decoration_manager_interface,
	    version, id);

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &decoration_manager_impl, NULL, NULL);

	/* Advertise the compositor's default decoration policy upon successful bind. */
	org_kde_kwin_server_decoration_manager_send_default_mode(
	    resource, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

/**
 * \brief Creates and registers the KDE server-side decoration manager global.
 *
 * Advertises the org_kde_kwin_server_decoration_manager protocol to Wayland
 * clients.
 *
 * \param[in] display The Wayland display to attach the global to.
 * \return A pointer to the created wl_global, or NULL on failure.
 */
struct wl_global *
kde_decoration_manager_create(struct wl_display *display)
{
	return wl_global_create(
	    display, &org_kde_kwin_server_decoration_manager_interface,
	    1, NULL, bind_decoration_manager);
}
