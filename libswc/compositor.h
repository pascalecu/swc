/**
 * \file compositor.h
 * \brief Compositor and view management for swc
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * \details
 * This header defines the compositor and compositor-specific view structures
 * used by swc. It provides functions to initialize and finalize the compositor,
 * create and destroy compositor views, manage visibility and borders, and
 * handle signals for new surface creation and view destruction.
 */

/*
 * Copyright (c) 2013, 2014 Michael Forney
 *
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

#ifndef SWC_COMPOSITOR_H
#define SWC_COMPOSITOR_H

#include "view.h"

#include <pixman.h>
#include <stdbool.h>
#include <wayland-server.h>

/**
 * \struct swc_compositor
 * \brief Represents the compositor and its core input and signal interfaces.
 *
 * \details
 * The compositor manages pointer input and emits signals when surfaces are
 * created. Compositor views are associated with the surfaces managed by the
 * compositor.
 */
struct swc_compositor {
	/**
	 * \brief Handler for pointer input events.
	 */
	struct pointer_handler *const pointer_handler;
	struct {
		/**
		 * Emitted when a new surface is created.
		 *
		 * The `data` argument of the signal refers to the surface that has been
		 * created.
		 */
		struct wl_signal new_surface;
	} signal;
};

/**
 * \brief Initialize the compositor subsystem, create targets and register
 * globals.
 *
 * \return true on success, false on failure.
 */
bool compositor_initialize(void);

/**
 * \brief Finalize the compositor subsystem and release resources.
 */
void compositor_finalize(void);

/**
 * \struct compositor_view
 * \brief Represents a compositor-managed view associated with a surface.
 *
 * \details
 * Each compositor view wraps a base view and tracks compositor-specific
 * properties such as visibility, borders, clipping, and parent-child
 * relationships.
 */
struct compositor_view {
	/**
	 * \brief Base view structure.
	 */
	struct view base;

	/**
	 * \brief Surface associated with this view.
	 */
	struct surface *surface;

	/**
	 * \brief Current buffer attached to the surface.
	 */
	struct wld_buffer *buffer;

	/**
	 * \brief Associated window, if any.
	 */
	struct window *window;

	/**
	 * \brief Parent view for stacking / transforms.
	 */
	struct compositor_view *parent;

	/**
	 * \brief Visibility flag (mapped/unmapped).
	 */
	bool visible;

	/**
	 * \brief Box covering the surface including border.
	 */
	pixman_box32_t extents;

	/**
	 * \brief Region occluded by opaque regions of higher surfaces.
	 */
	pixman_region32_t clip;

	/**
	 * \brief The border properties.
	 */
	struct {
		/**
		 * \brief Border width in pixels.
		 */
		uint32_t width;

		/**
		 * \brief Border color as ARGB.
		 */
		uint32_t color;

		/**
		 * \brief Whether the border has been marked as needing redraw.
		 */
		bool damaged;
	} border;

	/**
	 * \brief Linked list node for compositor tracking.
	 */
	struct wl_list link;

	/**
	 * \brief Signal emitted when this view is destroyed.
	 */
	struct wl_signal destroy_signal;
};

/**
 * \brief Create a compositor_view for the given surface.
 *
 * \param[in,out] surface The surface to wrap.
 * \return Pointer to the newly created compositor_view, or NULL on allocation failure.
 */
struct compositor_view *compositor_create_view(struct surface *surface);

/**
 * \brief Destroy a compositor view and free resources.
 *
 * \param[in,out] view The view to destroy.
 */
void compositor_view_destroy(struct compositor_view *view);

/**
 * \brief Helper to cast a base view to a compositor_view when appropriate.
 *
 * \param[in] view Base view pointer.
 * \return Pointer to compositor_view or NULL if base is not a compositor_view.
 */
struct compositor_view *compositor_view(struct view *view);

/**
 * \brief Set the parent of a compositor view.
 *
 * \param[in,out] view   Compositor view whose parent will be set.
 * \param[in]     parent New parent view.
 */
void compositor_view_set_parent(struct compositor_view *view, struct compositor_view *parent);

/**
 * \brief Show (map) a compositor view and recursively show children.
 *
 * \param[in,out] view The view to show.
 */
void compositor_view_show(struct compositor_view *view);

/**
 * \brief Hide (unmap) a compositor view and recursively hide children.
 *
 * \param[in,out] view The view to hide.
 */
void compositor_view_hide(struct compositor_view *view);

/**
 * \brief Set a view's border color and mark for damage if changed.
 *
 * \param[in,out] view  The view to modify.
 * \param[in]     color Border color in ARGB.
 */
void compositor_view_set_border_color(struct compositor_view *view, uint32_t color);

/**
 * \brief Set a view's border width and mark for damage if changed.
 *
 * \param[in,out] view  The view to modify.
 * \param[in]     width New border width in pixels.
 */
void compositor_view_set_border_width(struct compositor_view *view, uint32_t width);

#endif
