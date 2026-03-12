/**
 * \file region.c
 * \brief Wayland region implementation backed by pixman regions.
 * \author Michael Forney
 * \date 2013-2020
 * \copyright MIT
 *
 * This module implements the Wayland \c wl_region interface using
 * \c pixman_region32_t as the underlying region representation.
 *
 * A region represents a set of rectangles used for clipping, input
 * regions, opaque regions, and similar compositor operations.
 *
 * Each wl_region resource stores a pixman region which is modified
 * through the Wayland protocol requests \c add and \c subtract.
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

#include "region.h"
#include "util.h"

#include <pixman.h>
#include <stdlib.h>
#include <wayland-server.h>

/**
 * \brief Add a rectangle to the region.
 *
 * Implements the Wayland \c wl_region.add request. The rectangle is unioned
 * with the existing region.
 *
 * \param client Wayland client issuing the request.
 * \param resource The wl_region resource.
 * \param x Rectangle X coordinate.
 * \param y Rectangle Y coordinate.
 * \param width Rectangle width.
 * \param height Rectangle height.
 */
static void
add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;

	pixman_region32_t *region = wl_resource_get_user_data(resource);
	if (!region)
		return;

	pixman_region32_union_rect(region, region, x, y, width, height);
}

/**
 * \brief Subtract a rectangle from the region.
 *
 * Implements the Wayland \c wl_region.subtract request. The specified rectangle
 * area is removed from the region.
 *
 * \param client Wayland client issuing the request.
 * \param resource The wl_region resource.
 * \param x Rectangle X coordinate.
 * \param y Rectangle Y coordinate.
 * \param width Rectangle width.
 * \param height Rectangle height.
 */
static void
subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;

	pixman_region32_t *region = wl_resource_get_user_data(resource);
	if (!region)
		return;

	pixman_region32_t rect;
	pixman_region32_init_rect(&rect, x, y, width, height);
	pixman_region32_subtract(region, region, &rect);
	pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_impl = {
	.destroy = destroy_resource,
	.add = add,
	.subtract = subtract,
};

/**
 * \brief Destroy callback for a wl_region resource.
 *
 * Cleans up the pixman region associated with the resource.
 *
 * \param resource The Wayland resource being destroyed.
 */
static void
region_destroy(struct wl_resource *resource)
{
	pixman_region32_t *region = wl_resource_get_user_data(resource);
	if (!region)
		return;

	pixman_region32_fini(region);
	free(region);
}

/**
 * \brief Create a new Wayland region resource.
 *
 * Allocates and initializes a \c pixman_region32_t and attaches it to a
 * new \c wl_region resource.
 *
 * \param client The Wayland client creating the region.
 * \param version The protocol version supported by the client.
 * \param id The object ID for the new resource.
 *
 * \return The newly created \c wl_resource, or NULL on failure.
 */
struct wl_resource *
region_new(struct wl_client *client, uint32_t version, uint32_t id)
{
	pixman_region32_t *region = calloc(1, sizeof(*region));
	if (!region)
		return NULL;

	pixman_region32_init(region);

	struct wl_resource *resource =
	    wl_resource_create(client, &wl_region_interface, version, id);

	if (!resource) {
		pixman_region32_fini(region);
		free(region);
		return NULL;
	}

	wl_resource_set_implementation(resource, &region_impl, region, region_destroy);

	return resource;
}
