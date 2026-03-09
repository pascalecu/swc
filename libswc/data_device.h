/**
 * \file data_device.h
 *
 * \brief Wayland wl_data_device implementation for swc.
 *
 * \author Michael Forney
 * \date 2013--2019
 * \copyright MIT
 *
 * This module provides the compositor-side representation of the Wayland \c
 * wl_data_device interface.
 *
 * A data device is associated with a compositor seat and manages clipboard
 * selections and drag-and-drop interactions between clients.
 *
 * Clients obtain a \c wl_data_device through the \c wl_data_device_manager
 * global and use it to:
 *
 *  - receive data offers
 *  - set the current clipboard selection
 *  - initiate drag-and-drop operations
 *
 * The compositor maintains one \ref data_device per seat and tracks the
 * currently active selection data source.
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

#ifndef SWC_DATA_DEVICE_H
#define SWC_DATA_DEVICE_H

#include <stdbool.h>
#include <wayland-server.h>

/**
 * Events emitted through \ref data_device.event_signal.
 */
enum {
	/** Clipboard selection changed. */
	DATA_DEVICE_EVENT_SELECTION_CHANGED
};

/**
 * \brief Compositor-side representation of a \c wl_data_device.
 *
 * A data device tracks the current clipboard selection and the set of
 * \c wl_data_device protocol objects bound by clients.
 *
 * The structure is typically owned by a seat and is responsible for
 * notifying clients about clipboard and drag-and-drop state changes.
 */
struct data_device {

	/** Current clipboard selection (\c wl_data_source resource). */
	struct wl_resource *selection;

	/**
	 * Listener attached to the current selection resource.
	 *
	 * Used to detect when the selection data source is destroyed so the
	 * compositor can clear the selection state.
	 */
	struct wl_listener selection_destroy_listener;

	/**
	 * Signal emitted when the internal state of the data device changes.
	 *
	 * Currently used to notify listeners when the clipboard selection
	 * changes.
	 */
	struct wl_signal event_signal;

	/**
	 * List of \c wl_data_device resources bound by clients.
	 *
	 * Each entry corresponds to a client-visible protocol object.
	 */
	struct wl_list resources;
};

/**
 * \brief Create a new data device.
 *
 * Initializes a compositor-side data device used to manage clipboard selections
 * and client \c wl_data_device resources.
 *
 * \return The newly created \ref data_device, or \c NULL on allocation failure.
 */
struct data_device *data_device_create(void);

/**
 * \brief Destroy a data device.
 *
 * All \c wl_data_device resources associated with the device are destroyed
 * before the structure itself is freed.
 *
 * \param[in] data_device Data device to destroy.
 */
void data_device_destroy(struct data_device *data_device);

/**
 * \brief Bind a \c wl_data_device resource for a client.
 *
 * Creates the client-visible \c wl_data_device protocol object and associates
 * it with the compositor-side \ref data_device.
 *
 * \param[in] data_device Data device being bound.
 * \param[in] client Client requesting the binding.
 * \param[in] version Protocol version requested by the client.
 * \param[in] id Object ID for the created \c wl_data_device resource.
 *
 * \return The created \c wl_resource representing the \c wl_data_device, or \c
 * NULL on allocation failure.
 */
struct wl_resource *data_device_bind(struct data_device *data_device, struct wl_client *client, uint32_t version, uint32_t id);

/**
 * \brief Offer the current clipboard selection to a client.
 *
 * If a selection exists, a \c wl_data_offer is created and sent to the client's
 * \c wl_data_device resource.
 *
 * \param[in] data_device Data device providing the selection.
 * \param[in] client Client receiving the selection offer.
 */
void data_device_offer_selection(struct data_device *data_device, struct wl_client *client);

#endif
