/**
 * \file pointer.h
 * \brief Management of pointer (mouse) input devices and cursor state.
 * \author Michael Forney
 * \date 2013-2014
 * \copyright MIT
 *
 * Provides structures and functions to handle pointer motion, button presses,
 * and axis events, as well as managing the visual cursor surface and input
 * focus.
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

#ifndef SWC_POINTER_H
#define SWC_POINTER_H

#include "input.h"
#include "view.h"

#include <pixman.h>
#include <wayland-server.h>

/**
 * \brief Represents a single mouse button state.
 */
struct button {
	struct press press;              /**< The underlying press event data. */
	struct pointer_handler *handler; /**< The handler currently capturing this button. */
};

/**
 * \brief Interface for handling pointer events.
 */
struct pointer_handler {
	/** Called on pointer motion events. Return true if handled. */
	bool (*motion)(struct pointer_handler *handler, uint32_t time, wl_fixed_t x, wl_fixed_t y);

	/** Called on button press/release events. Return true if handled. */
	bool (*button)(struct pointer_handler *handler, uint32_t time, struct button *button, uint32_t state);

	/** Called on scroll/axis events. Return true if handled. */
	bool (*axis)(struct pointer_handler *handler, uint32_t time, enum wl_pointer_axis axis, enum wl_pointer_axis_source source, wl_fixed_t value, int value120);

	/** Called at the end of a set of pointer events to commit changes. */
	void (*frame)(struct pointer_handler *handler);

	int pending;         /**< Flag indicating if there are pending events for this handler. */
	struct wl_list link; /**< Link in the pointer's list of handlers. */
};

/**
 * \brief Main pointer device state.
 */
struct pointer {
	struct input_focus focus;                 /**< The current input focus for the pointer. */
	struct input_focus_handler focus_handler; /**< Callbacks for focus changes. */

	struct {
		struct view view;                    /**< The view representing the cursor on screen. */
		struct surface *surface;             /**< The Wayland surface used for the cursor image. */
		struct wl_listener destroy_listener; /**< Listener for cursor surface destruction. */
		struct wld_buffer *buffer;           /**< The hardware/shared buffer for the cursor. */
		struct wld_buffer *internal_buffer;  /**< Internal buffer used for default cursors. */

		struct {
			int32_t x, y; /**< Hotspot coordinates within the cursor image. */
		} hotspot;
	} cursor;

	struct wl_array buttons;                        /**< Array of currently pressed buttons. */
	struct wl_list handlers;                        /**< List of active pointer handlers. */
	struct pointer_handler client_handler;          /**< Default handler for sending events to Wayland clients. */
	enum wl_pointer_axis_source client_axis_source; /**< Current source for axis events. */

	wl_fixed_t x, y;          /**< Current global coordinates of the pointer. */
	pixman_region32_t region; /**< The valid region (screen area) the pointer can move within. */
};

/**
 * \brief Initializes the pointer device and centers the cursor.
 *
 * Sets up the focus handler, client handler, cursor view, and allocates a
 * hardware-compatible cursor buffer. Centers the cursor on the first screen.
 *
 * \param pointer The pointer structure to initialize.
 * \return true on successful initialization, false on failure (e.g., buffer
 * creation).
 */
bool pointer_initialize(struct pointer *pointer);

/**
 * \brief Finalizes the pointer and releases associated resources.
 *
 * Cleans up focus, regions, and other allocated resources.
 *
 * \param pointer The pointer structure to finalize.
 */
void pointer_finalize(struct pointer *pointer);

/**
 * \brief Changes the pointer's input focus to a specific view.
 *
 * Updates the focus to the new view, triggering enter/leave events as needed.
 *
 * \param pointer The pointer instance.
 * \param view The new focused compositor view.
 */
void pointer_set_focus(struct pointer *pointer, struct compositor_view *view);

/**
 * \brief Sets the allowed region for pointer movement.
 *
 * Copies the provided region and clips the current position to it.
 *
 * \param pointer The pointer instance.
 * \param region The new pixman region for clipping.
 */
void pointer_set_region(struct pointer *pointer, pixman_region32_t *region);

/**
 * \brief Sets the cursor to a specific theme-based icon.
 *
 * Loads cursor data from the theme, creates a buffer, and attaches it to the
 * cursor view. Releases any previous internal buffer and detaches
 * client-provided surfaces if present.
 *
 * \param pointer The pointer instance.
 * \param id The ID of the cursor icon.
 */
void pointer_set_cursor(struct pointer *pointer, uint32_t id);

/**
 * \brief Retrieves a button by its serial number.
 *
 * Searches the array of active buttons for a match.
 *
 * \param pointer The pointer instance.
 * \param serial The serial number of the button press.
 * \return The matching button, or NULL if not found.
 */
struct button *pointer_get_button(struct pointer *pointer, uint32_t serial);

/**
 * \brief Creates a new wl_resource for the pointer interface for a client.
 *
 * Binds the pointer interface and adds the resource to the focus list.
 *
 * \param pointer The pointer instance.
 * \param client The Wayland client.
 * \param version The interface version.
 * \param id The resource ID.
 * \return The created resource, or NULL on failure.
 */
struct wl_resource *pointer_bind(struct pointer *pointer, struct wl_client *client, uint32_t version, uint32_t id);

/**
 * \brief Handles button events by routing them to the appropriate handler.
 *
 * On press, finds a handler to capture the event. On release, dispatches to the
 * capturing handler and removes the button from the active list.
 *
 * \param pointer The pointer instance.
 * \param time The event timestamp.
 * \param value The button value (code).
 * \param state The button state (pressed/released).
 */
void pointer_handle_button(struct pointer *pointer, uint32_t time, uint32_t button, uint32_t state);

/**
 * \brief Handles axis events by routing to the first handler that accepts it.
 *
 * \param pointer The pointer instance.
 * \param time The event timestamp.
 * \param axis The axis.
 * \param source The axis source.
 * \param value The axis value.
 * \param value120 The high-res value.
 */
void pointer_handle_axis(struct pointer *pointer, uint32_t time, enum wl_pointer_axis axis, enum wl_pointer_axis_source source, wl_fixed_t value, int value120);

/**
 * \brief Handles relative motion by converting it to absolute motion.
 *
 * Adds the delta to the current position and calls absolute motion handler.
 *
 * \param pointer The pointer instance.
 * \param time The event timestamp.
 * \param dx The relative X change.
 * \param dy The relative Y change.
 */
void pointer_handle_relative_motion(struct pointer *pointer, uint32_t time, wl_fixed_t dx, wl_fixed_t dy);

/**
 * \brief Processes absolute motion and notifies the registered handlers.
 *
 * Clips coordinates, dispatches to the first motion handler, and updates the
 * cursor.
 *
 * \param pointer The pointer instance.
 * \param time The event timestamp.
 * \param x The absolute X coordinate.
 * \param y The absolute Y coordinate.
 */
void pointer_handle_absolute_motion(struct pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);

/**
 * \brief Dispatches accumulated pointer events for the current frame.
 *
 * Calls frame handlers for pending events and resets pending flags. Updates the
 * cursor position after processing.
 *
 * \param pointer The pointer instance.
 */
void pointer_handle_frame(struct pointer *pointer);

#endif
