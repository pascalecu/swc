/**
 * \file data.h
 * \brief Compositor-side Wayland data source/offer utilities.
 *
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * This module implements the Wayland data transfer protocol objects:
 *  - wl_data_source: represents transferable data (clipboard/drag-and-drop)
 *  - wl_data_offer: represents an offer of transferable data to a client
 *
 * The compositor uses these helpers to create, send, and manage offers
 * and MIME types.
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

#ifndef SWC_DATA_H
#define SWC_DATA_H

#include <stdint.h>

struct wl_client;

/**
 * \brief Create a new wl_data_source object.
 *
 * Allocates and initializes a data source for clipboard or drag-and-drop
 * operations. The data source is associated with the given client.
 *
 * \param[in] client The client creating the data source.
 * \param[in] version Protocol version to use.
 * \param[in] id Object ID for the new wl_data_source resource.
 * \return The created wl_resource, or NULL on allocation failure.
 *
 * \see https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_source
 */
struct wl_resource *data_source_new(struct wl_client *client, uint32_t version, uint32_t id);

/**
 * \brief Create a new wl_data_offer object.
 *
 * Creates a client-visible data offer backed by the given wl_data_source.
 *
 * \param[in] client The client receiving the offer.
 * \param[in] source The wl_data_source providing the data.
 * \param[in] version Protocol version to use.
 * \return The created wl_resource representing the offer, or NULL on failure.
 *
 * \see https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_offer
 */
struct wl_resource *data_offer_new(struct wl_client *client, struct wl_resource *source, uint32_t version);

/**
 * \brief Send all MIME types from a data source to a data offer.
 *
 * \param[in] source The wl_data_source resource.
 * \param[in] offer The wl_data_offer resource.
 */
void data_send_mime_types(struct wl_resource *source, struct wl_resource *offer);

#endif
