/**
 * \file data_device.c
 *
 * \brief Compositor-side implementation of the Wayland wl_data_device interface.
 *
 * \author Michael Forney
 * \date 2013--2019
 * \copyright MIT
 *
 * This module manages clipboard selections and drag-and-drop interactions for
 * Wayland clients.
 *
 * Each seat owns a single data_device instance, which tracks the currently
 * active clipboard selection and all client-bound wl_data_device resources.
 *
 * Responsibilities include:
 *  - tracking selection changes
 *  - sending data offers to clients
 *  - managing resource lifetimes
 *
 * \see https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_device
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

#include "data_device.h"
#include "data.h"
#include "event.h"
#include "util.h"

/**
 * \brief Handle wl_data_device.start_drag.
 *
 * Initiates a drag-and-drop operation requested by a client.
 *
 * Currently unimplemented.
 *
 * \param[in] client Client issuing the request.
 * \param[in] resource wl_data_device resource used for the request.
 * \param[in] source_resource Data source providing the drag data.
 * \param[in] origin_resource Surface where the drag originates.
 * \param[in] icon_resource Optional drag icon surface.
 * \param[in] serial Input event serial associated with the drag.
 *
 * \see https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_device-request-start_drag
 */
static void
start_drag(struct wl_client *client, struct wl_resource *resource,
           struct wl_resource *source_resource, struct wl_resource *origin_resource,
           struct wl_resource *icon_resource, uint32_t serial)
{
	(void)client;
	(void)resource;
	(void)source_resource;
	(void)origin_resource;
	(void)icon_resource;
	(void)serial;

	/* XXX: Implement */
}

/**
 * \brief Handle wl_data_device.set_selection.
 *
 * Updates the current clipboard selection for the data device. Cancels the
 * previous selection if one exists and registers a destroy listener for the new
 * selection.
 *
 * \param[in] client Client issuing the request.
 * \param[in] resource wl_data_device resource used for the request.
 * \param[in] data_source New selection data source (\c wl_data_source).
 * \param[in] serial Input event serial validating the request.
 *
 * \see
 * https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_device-request-set_selection
 */
static void
set_selection(struct wl_client *client, struct wl_resource *resource, struct wl_resource *data_source, uint32_t serial)
{
	struct data_device *data_device = wl_resource_get_user_data(resource);

	(void)client;
	(void)serial;

	/* No change? Do nothing. */
	if (data_source == data_device->selection)
		return;

	/* Cancel the old selection if it exists. */
	if (data_device->selection) {
		wl_data_source_send_cancelled(data_device->selection);
		wl_list_remove(&data_device->selection_destroy_listener.link);
		data_device->selection = NULL;
	}

	/* Set the new selection and add a destroy listener if valid. */
	if (data_source) {
		data_device->selection = data_source;
		wl_resource_add_destroy_listener(data_source, &data_device->selection_destroy_listener);
	}

	/* Notify any listeners that the selection has changed.*/
	send_event(&data_device->event_signal, DATA_DEVICE_EVENT_SELECTION_CHANGED, NULL);
}

/** wl_data_device request handler implementation. */
static const struct wl_data_device_interface data_device_impl = {
	.start_drag = start_drag,
	.set_selection = set_selection,
	.release = destroy_resource,
};

/**
 * \brief Handle destruction of the current selection.
 *
 * Triggered when the wl_data_source associated with the selection is destroyed.
 *
 * Clears the selection and emits \c DATA_DEVICE_EVENT_SELECTION_CHANGED.
 *
 * \param[in] listener Listener attached to the selection resource.
 * \param[in] data Unused.
 */
static void
handle_selection_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct data_device *data_device =
	    wl_container_of(listener, data_device, selection_destroy_listener);

	data_device->selection = NULL;
	send_event(&data_device->event_signal, DATA_DEVICE_EVENT_SELECTION_CHANGED, NULL);
}

/**
 * \brief Allocate and initialize a new data device.
 *
 * Initializes selection tracking, event signals, and the list of client
 * resources.
 *
 * \return Pointer to the new \c data_device, or \c NULL on allocation failure.
 */
struct data_device *
data_device_create(void)
{
	struct data_device *data_device = calloc(1, sizeof(*data_device));
	if (!data_device)
		return NULL;

	/* Set up destroy listener for selection resource. */
	data_device->selection_destroy_listener.notify = &handle_selection_destroy;

	/* Initialize event signal for selection changes. */
	wl_signal_init(&data_device->event_signal);

	/* Initialize list of client resources. */
	wl_list_init(&data_device->resources);

	return data_device;
}

/**
 * \brief Destroy a data device and all bound client resources.
 *
 * \param[in] data_device Data device to destroy.
 */
void
data_device_destroy(struct data_device *data_device)
{
	if (!data_device)
		return;

	struct wl_resource *resource, *tmp;

	/* Destroy all client-bound wl_data_device resources. */
	wl_list_for_each_safe (resource, tmp, &data_device->resources, link)
		wl_resource_destroy(resource);

	free(data_device);
}

/**
 * \brief Bind a wl_data_device resource for a client.
 *
 * Creates a client-visible wl_data_device and adds it to the data device's
 * resource list.
 *
 * \param[in] data_device Data device instance.
 * \param[in] client Client requesting the binding.
 * \param[in] version Protocol version requested.
 * \param[in] id Object ID for the created resource.
 *
 * \return The new \c wl_resource representing the wl_data_device, or NULL on
 * failure.
 */
struct wl_resource *
data_device_bind(struct data_device *data_device, struct wl_client *client, uint32_t version, uint32_t id)
{
	if (!data_device || !client)
		return NULL;

	struct wl_resource *resource =
	    wl_resource_create(client, &wl_data_device_interface, version, id);

	if (!resource)
		return NULL;

	wl_resource_set_implementation(resource, &data_device_impl, data_device, &remove_resource);
	wl_list_insert(&data_device->resources, &resource->link);

	return resource;
}

/**
 * \brief Create a new data offer for a client.
 *
 * \param[in] resource Client's wl_data_device resource.
 * \param[in] client Client receiving the offer.
 * \param[in] source Selection data source providing the offer.
 * \return The created wl_data_offer resource, or NULL on failure.
 */
static struct wl_resource *
new_offer(struct wl_resource *resource, struct wl_client *client, struct wl_resource *source)
{
	if (!resource || !client || !source)
		return NULL;

	const uint32_t version = wl_resource_get_version(resource);

	struct wl_resource *offer = data_offer_new(client, source, version);
	if (!offer)
		return NULL;

	/* Notify the client of the new data offer and advertised supported MIME types. */
	wl_data_device_send_data_offer(resource, offer);
	data_send_mime_types(source, offer);

	return offer;
}

/**
 * \brief Offer the current selection to a client.
 *
 * If a selection exists, a new wl_data_offer is created and sent to the
 * client's wl_data_device resource.
 *
 * \param[in] data_device Data device providing the selection.
 * \param[in] client Client to receive the offer.
 */
void
data_device_offer_selection(struct data_device *data_device, struct wl_client *client)
{
	if (!data_device || !client)
		return;

	/* Look for the client's data_device resource. */
	struct wl_resource *resource = wl_resource_find_for_client(&data_device->resources, client);

	/* If the client does not have a data device, there is nothing to do. */
	if (!resource)
		return;

	/* If we have a selection, create a new offer for the client. */
	struct wl_resource *offer = NULL;
	if (data_device->selection)
		offer = new_offer(resource, client, data_device->selection);

	wl_data_device_send_selection(resource, offer);
}
