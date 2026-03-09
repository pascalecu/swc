/**
 * \file data_device_manager.c
 * \brief Implementation of the Wayland wl_data_device_manager global.
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * \details
 * This module provides the compositor-side implementation of the
 * wl_data_device_manager interface used for clipboard and drag-and-drop data
 * transfers between Wayland clients.
 *
 * Clients bind to this global through the Wayland registry and may then:
 *
 *  - create wl_data_source objects to offer transferable data
 *  - obtain wl_data_device objects associated with a wl_seat
 *
 * The manager acts as the entry point for all clipboard and drag-and-drop
 * operations within the compositor.
 *
 * Most functionality is implemented in:
 *
 *  - \c data_source_new() - creation of data sources
 *  - \c data_device_bind() - client binding to seat data devices
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

#include "data_device_manager.h"
#include "data.h"
#include "data_device.h"
#include "internal.h"
#include "seat.h"

/**
 * \brief Handle wl_data_device_manager.create_data_source.
 * \details
 * Creates a new wl_data_source object owned by the requesting client.
 * The data source represents a provider of clipboard or drag-and-drop
 * data that may later be offered to other clients.
 *
 * \param[in] client Client issuing the request.
 * \param[in] resource wl_data_device_manager resource used for the request.
 * \param[in] id Object ID for the newly created wl_data_source.
 */
static void
create_data_source(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	const uint32_t version = wl_resource_get_version(resource);
	if (!data_source_new(client, version, id))
		wl_resource_post_no_memory(resource);
}

/**
 * \brief Handle wl_data_device_manager.get_data_device.
 * \details
 * Returns a wl_data_device associated with the specified wl_seat. The data
 * device allows the client to participate in clipboard and drag-and-drop
 * operations for that seat.
 *
 * \param[in] client Client issuing the request.
 * \param[in] resource wl_data_device_manager resource used for the request.
 * \param[in] id Object ID for the created wl_data_device.
 * \param[in] seat_resource wl_seat identifying the seat whose data device
 * should be bound.
 */
static void
get_data_device(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *seat_resource)
{
	const uint32_t version = wl_resource_get_version(resource);
	struct swc_seat *seat = wl_resource_get_user_data(seat_resource);

	if (!data_device_bind(seat->data_device, client, version, id))
		wl_resource_post_no_memory(resource);
}

/** Request handler implementation for wl_data_device_manager. */
static const struct wl_data_device_manager_interface data_device_manager_impl = {
	.create_data_source = create_data_source,
	.get_data_device = get_data_device,
};

/**
 * \brief Bind the wl_data_device_manager global to a client.
 * \details
 * Called by the Wayland server when a client binds the wl_data_device_manager
 * global advertised in the registry.
 *
 * A wl_resource representing the manager interface is created and associated
 * with the request handler implementation.
 *
 * \param[in] client Client binding the global.
 * \param[in] data User data associated with the global (unused).
 * \param[in] version Protocol version requested by the client.
 * \param[in] id Object ID for the created wl_data_device_manager resource.
 */
static void
bind_data_device_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;

	resource = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &data_device_manager_impl, NULL, NULL);
}

/**
 * \brief Create the wl_data_device_manager global.
 * \details
 * Registers the wl_data_device_manager interface with the Wayland display so
 * that clients can bind to it through the registry.
 *
 * The global is advertised with protocol version 2.
 *
 * \param[in] display Wayland display used to register the global.
 * \return The created wl_global object.
 */
struct wl_global *
data_device_manager_create(struct wl_display *display)
{
	return wl_global_create(display, &wl_data_device_manager_interface, 2, NULL, &bind_data_device_manager);
}
