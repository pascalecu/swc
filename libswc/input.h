/**
 * \file input.h
 * \brief Input focus and event handling for the swc compositor.
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * This module handles the logic for switching input focus between different
 * views and managing the Wayland resources (keyboard, pointer, touch)
 * associated with those focused clients.
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

#ifndef SWC_INPUT_H
#define SWC_INPUT_H

#include <stdbool.h>
#include <wayland-server.h>

/**
 * \brief Event types for the input focus system.
 */
enum {
	INPUT_FOCUS_EVENT_CHANGED /**< Triggered when focus moves to a different view. */
};

/**
 * \brief Data payload for focus change events.
 */
struct input_focus_event_data {
	struct compositor_view *old; /**< The view losing focus. */
	struct compositor_view *new; /**< The view gaining focus. */
};

/**
 * \brief Callbacks for specific input devices (keyboard, pointer) to
 * handle the transition of focus.
 */
struct input_focus_handler {
	/** \brief Callback triggered when focus enters a view. */
	void (*enter)(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view);
	/** \brief Callback triggered when focus leaves a view. */
	void (*leave)(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view);
};

/**
 * \brief Manages the state of focus for a specific input seat component.
 */
struct input_focus {
	struct wl_client *client;                 /**< The client currently owning the focus. */
	struct compositor_view *view;             /**< The specific view currently owning the focus. */
	struct wl_listener view_destroy_listener; /**< Listener to clear focus if view is destroyed. */

	struct input_focus_handler *handler; /**< The device-specific handler. */
	struct wl_list active;               /**< List of resources for the focused client. */
	struct wl_list inactive;             /**< List of resources for all other clients. */

	struct wl_signal event_signal; /**< Signal emitted on focus changes. */
};

/**
 * \brief Initializes an input focus management structure.
 *
 * \param[in,out] input_focus   The structure to initialize.
 * \param[in]     input_handler The device-specific handler to use.
 * \return true on success, false otherwise.
 */
bool input_focus_initialize(struct input_focus *input_focus, struct input_focus_handler *input_handler);

/**
 * \brief Finalizes and cleans up an input focus structure.
 *
 * \param[in,out] input_focus The structure to finalize.
 */
void input_focus_finalize(struct input_focus *input_focus);

/**
 * \brief Adds a Wayland resource to the focus management lists.
 *
 * \param[in,out] input_focus The focus manager.
 * \param[in,out] resource    The Wayland resource (e.g., wl_keyboard) to add.
 */
void input_focus_add_resource(struct input_focus *input_focus, struct wl_resource *resource);

/**
 * \brief Removes a Wayland resource from focus management.
 * \param[in,out] input_focus The focus manager.
 * \param[in,out] resource    The Wayland resource to remove.
 */
void input_focus_remove_resource(struct input_focus *input_focus, struct wl_resource *resource);

/**
 * \brief Sets the current input focus to a specific view.
 *
 * \param[in,out] input_focus The focus manager.
 * \param[in]     view        The view to focus, or NULL to clear focus.
 */
void input_focus_set(struct input_focus *input_focus, struct compositor_view *view);

/**
 * \brief Represents a physical or logical press event (key or button).
 */
struct press {
	uint32_t value;  /**< Key code or button ID. */
	uint32_t serial; /**< The Wayland serial number of the event. */
	void *data;      /**< Context-specific data. */
};

#endif
