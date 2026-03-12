/**
 * \file screen.h
 * \brief Screen and geometry management definitions for swc.
 * \author Michael Forney
 * \date 2013–2014
 * \copyright MIT
 *
 * This header defines the internal screen structures, including geometry
 * modifiers used for panels/docks and the primary screen object that links DRM
 * CRTCs to Wayland globals.
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

#ifndef SWC_SCREEN_H
#define SWC_SCREEN_H

#include "primary_plane.h"
#include "swc.h"

#include <wayland-util.h>

struct output;
struct pixman_region32;

/**
 * \brief Interface for objects that modify the usable area of a screen.
 *
 * Useful for UI elements like status bars or docks that need to "shrink"
 * the area where windows can be tiled or maximized.
 */
struct screen_modifier {
	/**
	 * \brief Calculates the remaining usable region after this modifier is applied.
	 * \param modifier The modifier instance.
	 * \param geometry The total physical geometry of the screen.
	 * \param usable An initialized pixman region to be populated with the result.
	 */
	void (*modify)(struct screen_modifier *modifier, const struct swc_rectangle *geometry, struct pixman_region32 *usable);

	struct wl_list link;
};

/**
 * \brief Represents a physical display managed by the compositor.
 */
struct screen {
	struct swc_screen base;                   /**< Publicly exposed screen interface. */
	const struct swc_screen_handler *handler; /**< Event handler for screen-specific logic. */
	void *handler_data;                       /**< User data associated with the handler. */

	struct wl_signal destroy_signal; /**< Signal emitted when the screen is being destroyed. */
	uint8_t id;                      /**< Unique ID used for bitmasking. */
	uint32_t crtc;                   /**< The DRM CRTC ID associated with this screen. */

	struct {
		struct primary_plane primary; /**< The primary hardware plane. */
		struct plane *cursor;         /**< The hardware cursor plane. */
	} planes;

	struct wl_global *global; /**< The Wayland global object for this screen. */
	struct wl_list resources; /**< List of bound wl_resource objects. */

	struct wl_list outputs;   /**< List of physical outputs (connectors) on this screen. */
	struct wl_list modifiers; /**< List of active screen_modifier objects. */
	struct wl_list link;      /**< Link in the global swc.screens list. */
};

/**
 * \brief Initializes the global screen list and requests DRM to create screens.
 * \return true if screens were successfully created and initialized, false
 * otherwise.
 */
bool screens_initialize(void);

/**
 * \brief Destroys all currently active screens and cleans up the screen list.
 */
void screens_finalize(void);

/**
 * \brief Allocates and initializes a new screen.
 * \param crtc The DRM CRTC ID associated with this screen.
 * \param output The primary output tied to this screen.
 * \param cursor_plane The plane to be used for hardware cursors.
 * \return A pointer to the newly allocated screen, or NULL on failure.
 */
struct screen *screen_new(uint32_t crtc, struct output *output, struct plane *cursor_plane);

/**
 * \brief Tears down a screen, cleaning up its planes, outputs, and signals.
 * \param screen The screen to destroy.
 */
void screen_destroy(struct screen *screen);

/**
 * \brief Generates a bitmask for the given screen based on its ID.
 * \param screen The screen to mask.
 * \return A bitmask (e.g., 1 << id).
 */
static inline uint32_t
screen_mask(struct screen *screen)
{
	return 1 << screen->id;
}

/**
 * \brief Recalculates the usable geometry of a screen by applying screen modifiers.
 *
 * This is used to reserve space for panels, docks, or other UI elements that
 * reduce the effective screen area available for standard windows.
 *
 * \param screen The screen whose geometry needs to be updated.
 */
void screen_update_usable_geometry(struct screen *screen);

#endif
