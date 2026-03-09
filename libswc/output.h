/**
 * \file output.h
 * \brief Output (display connector) representation.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * This module defines the compositor representation of a display output. An
 * output corresponds to a DRM connector and exposes information such as
 * physical dimensions, supported display modes, and damage tracking.
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

#ifndef SWC_OUTPUT_H
#define SWC_OUTPUT_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-util.h>
#include <xf86drmMode.h>

struct wl_display;

/**
 * \brief Representation of a display output.
 *
 * Each output corresponds to a DRM connector and maintains the list of
 * available display modes, damage tracking regions, and Wayland resources
 * associated with the output global.
 */
struct output {
	struct screen *screen; /**< Screen this output belongs to */

	/** Physical dimensions of the display in millimeters */
	uint32_t physical_width;
	uint32_t physical_height;

	/** Supported display modes (array of struct mode) */
	struct wl_array modes;

	/** Preferred display mode, if advertised by the connector */
	struct mode *preferred_mode;

	/** Damage regions for current frame */
	pixman_region32_t current_damage;

	/** Damage regions for previous frame */
	pixman_region32_t previous_damage;

	/** DRM connector identifier */
	uint32_t connector;

	/** Wayland global for the wl_output interface */
	struct wl_global *global;

	/** List of wl_output resources bound by clients */
	struct wl_list resources;

	/** Link in the compositor's output list */
	struct wl_list link;
};

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
struct output *output_new(drmModeConnector *connector);

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
void output_destroy(struct output *output);

#endif
