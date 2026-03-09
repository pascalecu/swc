/**
 * \file output.c
 * \brief Output implementation.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * Implementation of the output abstraction defined in output.h. This file
 * manages the creation and destruction of outputs from DRM connectors and
 * exposes them to Wayland clients through the wl_output global.
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

#include "output.h"
#include "drm.h"
#include "internal.h"
#include "mode.h"
#include "screen.h"
#include "util.h"

#include <drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

static const struct wl_output_interface output_impl = {
	.release = destroy_resource,
};

/**
 * \brief Bind handler for the wl_output global.
 *
 * Creates a wl_output resource for the client and sends the static output
 * metadata (geometry and supported modes).
 *
 * \param client The binding client.
 * \param data Pointer to the output struct passed to wl_global_create.
 * \param version Protocol version requested by the client.
 * \param id The id for the new resource.
 */
static void
bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct output *output = data;
	struct screen *screen = output->screen;
	struct mode *mode;
	struct wl_resource *resource;
	uint32_t flags;

	resource = wl_resource_create(client, &wl_output_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	/* Install implementation and track resource so we can clean up later. */
	wl_resource_set_implementation(resource, &output_impl, output, &remove_resource);
	wl_list_insert(&output->resources, wl_resource_get_link(resource));

	/* Send geometry. If screen is not available, send a safe default. */
	if (screen)
		wl_output_send_geometry(resource, screen->base.geometry.x, screen->base.geometry.y,
		                        output->physical_width, output->physical_height,
		                        0, "unknown", "unknown", WL_OUTPUT_TRANSFORM_NORMAL);
	else
		wl_output_send_geometry(resource, 0, 0,
		                        output->physical_width, output->physical_height,
		                        0, "unknown", "unknown", WL_OUTPUT_TRANSFORM_NORMAL);

	/* Advertise supported modes. Mark preferred/current where applicable. */
	wl_array_for_each (mode, &output->modes) {
		flags = 0;

		if (mode->preferred)
			flags |= WL_OUTPUT_MODE_PREFERRED;

		if (screen) {
			/* Guard against missing primary plane or uninitialized mode. */
			if (mode_equal(&screen->planes.primary.mode, mode))
				flags |= WL_OUTPUT_MODE_CURRENT;
		}

		wl_output_send_mode(resource, flags, mode->width, mode->height, mode->refresh);
	}

	/* Newer protocol versions expect a done event. */
	if (version >= 2)
		wl_output_send_done(resource);
}

/**
 * \brief Allocate and initialize an output structure from a DRM connector.
 *
 * The returned output is registered as a global (wl_output) and contains a copy
 * of the connector's mode list. The caller is responsible for adding the output
 * to the compositor's list of outputs and for setting `output->screen` if
 * appropriate.
 *
 * \param connector DRM connector describing the physical output.
 * \return Newly allocated output on success, NULL on failure.
 */
struct output *
output_new(drmModeConnectorPtr connector)
{
	struct output *output = NULL;
	struct mode *modes = NULL;
	uint32_t i;

	if (!connector) {
		ERROR("output_new: connector is NULL\n");
		return NULL;
	}

	output = calloc(1, sizeof(*output));
	if (!output) {
		ERROR("Failed to allocate output\n");
		return NULL;
	}

	/* Initialize fields that do not depend on DRM state. */
	output->physical_width = connector->mmWidth;
	output->physical_height = connector->mmHeight;
	output->preferred_mode = NULL;
	wl_list_init(&output->resources);
	wl_array_init(&output->modes);
	pixman_region32_init(&output->current_damage);
	pixman_region32_init(&output->previous_damage);
	output->connector = connector->connector_id;

	/* Copy connector modes into the wl_array. */
	if (connector->count_modes == 0)
		goto error;

	modes = wl_array_add(&output->modes, connector->count_modes * sizeof(*modes));
	if (!modes)
		goto error;

	for (i = 0; i < connector->count_modes; ++i) {
		mode_initialize(&modes[i], &connector->modes[i]);
		if (modes[i].preferred)
			output->preferred_mode = &modes[i];
	}

	if (!output->preferred_mode)
		output->preferred_mode = &modes[0];

	/* Create the wl_output global after the output is in a valid state. */
	output->global = wl_global_create(swc.display, &wl_output_interface, 3, output, &bind_output);
	if (!output->global) {
		ERROR("Failed to create output global\n");
		goto error;
	}

	return output;

error:
	/* Clean up partially-initialized resources. */
	if (output) {
		wl_array_release(&output->modes);
		pixman_region32_fini(&output->current_damage);
		pixman_region32_fini(&output->previous_damage);
		free(output);
	}

	return NULL;
}

/**
 * \brief Destroy an output previously created with output_new().
 *
 * This releases all local resources allocated for the output. Any bound Wayland
 * resources associated with clients are not forcibly destroyed here; the
 * Wayland core will handle client-side resource cleanup when the global is
 * removed. Callers should ensure the output has been removed from compositor
 * lists before destroying it.
 *
 * \param output The output to destroy.
 */
void
output_destroy(struct output *output)
{
	if (!output)
		return;

	wl_array_release(&output->modes);
	pixman_region32_fini(&output->current_damage);
	pixman_region32_fini(&output->previous_damage);

	/* Remove the wl_output global so new clients cannot bind. Existing
	 * client resources will be cleaned up by the Wayland core when clients
	 * disconnect or when the compositor decides to destroy resources. */
	if (output->global)
		wl_global_destroy(output->global);

	free(output);
}
