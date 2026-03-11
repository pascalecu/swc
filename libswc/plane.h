/**
 * \file plane.h
 * \brief Hardware plane management for swc.
 * \author Michael Forney
 * \date 2019
 * \copyright MIT
 *
 * This file defines the plane structure and its associated lifecycle functions.
 * It wraps DRM planes to be used as compositor views.
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

#ifndef SWC_PLANE_H
#define SWC_PLANE_H

#include "plane.h"
#include "view.h"

#include <wayland-server.h>

/**
 * \brief Represents a hardware DRM plane mapped to a compositor view.
 */
struct plane {
	struct view view;                /**< The base view structure. */
	struct screen *screen;           /**< The screen this plane is attached to. */
	uint32_t id;                     /**< The DRM object ID of the plane. */
	uint32_t fb;                     /**< The DRM framebuffer ID currently attached. */
	int type;                        /**< The DRM plane type (e.g., Overlay, Primary, Cursor). */
	uint32_t possible_crtcs;         /**< Bitmask of CRTCs this plane can be used with. */
	struct wl_listener swc_listener; /**< Listener for global SWC events. */
	struct wl_list link;             /**< List node for compositor plane lists. */
};

/**
 * \brief Allocates and initializes a new hardware plane.
 * \param id The DRM object ID for the plane.
 * \return A pointer to the newly allocated plane, or NULL on failure.
 */
struct plane *plane_new(uint32_t id);

/**
 * \brief Destroys a plane and frees associated resources.
 * \param plane The plane to destroy.
 */
void plane_destroy(struct plane *plane);

#endif
