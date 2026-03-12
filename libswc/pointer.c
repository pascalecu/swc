/**
 * \file pointer.c
 * \brief Implementation of pointer input logic and cursor rendering.
 * \author Michael Forney
 * \date 2013-2020
 * \copyright MIT
 *
 * Handles the logic for coordinate clipping, cursor surface updates,
 * event dispatching to registered handlers, and Wayland protocol communication.
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
#include "pointer.h"
#include "compositor.h"
#include "cursor/cursor_data.h"
#include "event.h"
#include "internal.h"
#include "plane.h"
#include "screen.h"
#include "shm.h"
#include "surface.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <wld/wld.h>

/**
 * \brief Sends a wl_pointer.enter event to the client owning the focused view.
 *
 * This notifies the client that the pointer has entered the surface of the
 * given view. If no resources are available, it falls back to setting a default
 * cursor.
 *
 * \param handler The input focus handler.
 * \param resources List of Wayland resources for the pointer.
 * \param view The compositor view being entered.
 */
static void
enter(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view)
{
	struct pointer *pointer = wl_container_of(handler, pointer, focus_handler);
	if (wl_list_empty(resources)) {
		pointer_set_cursor(pointer, cursor_left_ptr);
		return;
	}

	uint32_t serial = wl_display_next_serial(swc.display);
	wl_fixed_t surface_x = pointer->x - wl_fixed_from_int(view->base.geometry.x);
	wl_fixed_t surface_y = pointer->y - wl_fixed_from_int(view->base.geometry.y);

	struct wl_resource *resource;
	wl_resource_for_each (resource, resources)
		wl_pointer_send_enter(resource, serial, view->surface->resource, surface_x, surface_y);
}

/**
 * \brief Sends a wl_pointer.leave event to the previously focused client.
 *
 * This notifies the client that the pointer has left the surface.
 *
 * \param handler The input focus handler.
 * \param resources List of Wayland resources for the pointer.
 * \param view The compositor view being left (unused in this implementation).
 */
static void
leave(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view)
{
	uint32_t serial = wl_display_next_serial(swc.display);

	struct wl_resource *resource;
	wl_resource_for_each (resource, resources)
		wl_pointer_send_leave(resource, serial, view->surface->resource);
}

/**
 * \brief Callback triggered when the cursor's wl_surface is destroyed by the
 * client.
 *
 * Detaches the surface from the cursor view and clears the cursor surface
 * reference.
 *
 * \param listener The listener object embedded in the pointer struct.
 * \param data Pointer to the destroyed wl_resource (unused).
 */
static void
handle_cursor_surface_destroy(struct wl_listener *listener, void *data)
{
	struct pointer *pointer = wl_container_of(listener, pointer, cursor.destroy_listener);

	view_attach(&pointer->cursor.view, NULL);
	pointer->cursor.surface = NULL;
}

/**
 * \brief View update callback. Triggers a frame event for the cursor view.
 *
 * This ensures the cursor view is updated with the current time.
 *
 * \param view The cursor view.
 * \return true to indicate the update was handled successfully.
 */
static bool
update(struct view *view)
{
	view_frame(view, get_time());
	return true;
}

/**
 * \brief Updates the hardware cursor planes across all screens.
 *
 * Attaches the provided buffer to the cursor plane, clearing the background if
 * needed. Handles damage regions and updates screen planes accordingly.
 *
 * \param view The cursor view.
 * \param buffer The buffer to be attached to the cursor plane.
 * \return 0 on success, or an error code if attachment fails.
 */
static int
attach(struct view *view, struct wld_buffer *buffer)
{
	struct pointer *pointer = wl_container_of(view, pointer, cursor.view);
	struct surface *cursor_surface = pointer->cursor.surface;

	if (cursor_surface && !pixman_region32_not_empty(&cursor_surface->state.damage))
		return 0;

	wld_set_target_buffer(swc.shm->renderer, pointer->cursor.buffer);
	wld_fill_rectangle(swc.shm->renderer, 0x00000000, 0, 0, pointer->cursor.buffer->width, pointer->cursor.buffer->height);

	if (buffer)
		wld_copy_rectangle(swc.shm->renderer, buffer, 0, 0, 0, 0, buffer->width, buffer->height);

	wld_flush(swc.shm->renderer);

	/* TODO: Send an early release to the buffer */

	if (cursor_surface)
		pixman_region32_clear(&cursor_surface->state.damage);

	if (view_set_size_from_buffer(view, buffer))
		view_update_screens(view);

	struct screen *screen;
	wl_list_for_each (screen, &swc.screens, link) {
		if (screen->planes.cursor)
			view_attach(&screen->planes.cursor->view,
			            buffer ? pointer->cursor.buffer : NULL);
		view_update(&screen->planes.cursor->view);
	}

	return 0;
}

/**
 * \brief Moves the cursor view and synchronizes hardware planes.
 *
 * Updates the position of the cursor view and propagates the move to hardware
 * cursor planes on all screens. This ensures consistent cursor positioning
 * across multiple displays.
 *
 * \param view The cursor view.
 * \param x Global X coordinate.
 * \param y Global Y coordinate.
 * \return true if the move was successful, false otherwise.
 */
static bool
move(struct view *view, int32_t x, int32_t y)
{
	if (view_set_position(view, x, y))
		view_update_screens(view);

	struct screen *screen;
	wl_list_for_each (screen, &swc.screens, link) {
		view_move(&screen->planes.cursor->view, view->geometry.x, view->geometry.y);
		view_update(&screen->planes.cursor->view);
	}

	return true;
}

static const struct view_impl view_impl = {
	.update = update,
	.attach = attach,
	.move = move,
};

/**
 * \brief Updates the visual cursor position and hotspot.
 *
 * Adjusts the cursor view position based on the current pointer coordinates and
 * the cursor hotspot offset.
 *
 * \param pointer The pointer device to update.
 */
static inline void
update_cursor(struct pointer *pointer)
{
	const int32_t x = wl_fixed_to_int(pointer->x) - pointer->cursor.hotspot.x;
	const int32_t y = wl_fixed_to_int(pointer->y) - pointer->cursor.hotspot.y;

	view_move(&pointer->cursor.view, x, y);
}

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
void
pointer_set_cursor(struct pointer *pointer, uint32_t id)
{
	struct cursor *cursor = &cursor_metadata[id];
	union wld_object object = { .ptr = &cursor_data[cursor->offset] };
	struct wld_buffer *buffer;

	if (pointer->cursor.internal_buffer)
		wld_buffer_unreference(pointer->cursor.internal_buffer);

	if (pointer->cursor.surface) {
		surface_set_view(pointer->cursor.surface, NULL);
		wl_list_remove(&pointer->cursor.destroy_listener.link);
		pointer->cursor.surface = NULL;
	}

	buffer = wld_import_buffer(swc.shm->context, WLD_OBJECT_DATA, object,
	                           cursor->width, cursor->height, WLD_FORMAT_ARGB8888, cursor->width * 4);

	if (!buffer)
		WARNING("Failed to create cursor buffer\n");

	pointer->cursor.internal_buffer = buffer;
	pointer->cursor.hotspot.x = cursor->hotspot_x;
	pointer->cursor.hotspot.y = cursor->hotspot_y;

	update_cursor(pointer);
	view_attach(&pointer->cursor.view, buffer);
}

/**
 * \brief Default handler for motion events intended for Wayland clients.
 *
 * Translates global compositor coordinates into surface-local coordinates
 * before sending motion events to the focused client.
 *
 * \param handler The pointer handler.
 * \param time The event timestamp.
 * \param x The fixed-point X coordinate.
 * \param y The fixed-point Y coordinate.
 * \return true if handled, false otherwise.
 */
static bool
client_handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	if (wl_list_empty(&pointer->focus.active))
		return false;

	wl_fixed_t sx = x - wl_fixed_from_int(pointer->focus.view->base.geometry.x);
	wl_fixed_t sy = y - wl_fixed_from_int(pointer->focus.view->base.geometry.y);

	struct wl_resource *resource;
	wl_resource_for_each (resource, &pointer->focus.active)
		wl_pointer_send_motion(resource, time, sx, sy);

	return true;
}

/**
 * \brief Default handler for button events intended for Wayland clients.
 *
 * Sends button press/release events to the focused client.
 *
 * \param handler The pointer handler.
 * \param time The event timestamp.
 * \param button The button structure.
 * \param state The button state (pressed/released).
 * \return true if handled, false otherwise.
 */
static bool
client_handle_button(struct pointer_handler *handler, uint32_t time, struct button *button, uint32_t state)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	if (wl_list_empty(&pointer->focus.active))
		return false;

	struct wl_resource *resource;
	wl_resource_for_each (resource, &pointer->focus.active)
		wl_pointer_send_button(resource, button->press.serial, time,
		                       button->press.value, state);

	return true;
}

/**
 * \brief Default handler for axis (scroll) events.
 *
 * Handles protocol versioning for axis sources, high-resolution values
 * (value120), discrete steps, and stop events.
 *
 * \param handler The pointer handler.
 * \param time The event timestamp.
 * \param axis The axis (vertical/horizontal).
 * \param source The axis source.
 * \param value The fixed-point axis value.
 * \param value120 The high-resolution axis value.
 * \return true if handled, false otherwise.
 */
static bool
client_handle_axis(struct pointer_handler *handler, uint32_t time, enum wl_pointer_axis axis, enum wl_pointer_axis_source source, wl_fixed_t value, int value120)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;
	int ver;

	if (wl_list_empty(&pointer->focus.active))
		return false;

	if (pointer->client_axis_source != -1) {
		assert(pointer->client_axis_source == source);
		source = -1;
	} else {
		pointer->client_axis_source = source;
	}

	wl_resource_for_each (resource, &pointer->focus.active) {
		ver = wl_resource_get_version(resource);

		if (source != -1 && ver >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
			wl_pointer_send_axis_source(resource, source);

		if (value120) {
			if (ver >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
				wl_pointer_send_axis_value120(resource, axis, value120);
			else if (ver >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
				wl_pointer_send_axis_discrete(resource, axis, value120 / 120);
		}

		if (value)
			wl_pointer_send_axis(resource, time, axis, value);
		else if (ver >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
			wl_pointer_send_axis_stop(resource, time, axis);
	}

	return true;
}

/**
 * \brief Default handler for frame events.
 *
 * Sends frame events to clients supporting the frame protocol version and
 * resets the axis source.
 *
 * \param handler The pointer handler.
 */
static void
client_handle_frame(struct pointer_handler *handler)
{
	struct pointer *pointer = wl_container_of(handler, pointer, client_handler);
	struct wl_resource *resource;

	wl_resource_for_each (resource, &pointer->focus.active) {
		if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(resource);
	}

	pointer->client_axis_source = -1;
}

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
bool
pointer_initialize(struct pointer *pointer)
{
	if (!pointer)
		return false;

	struct screen *screen = wl_container_of(swc.screens.next, screen, link);
	struct swc_rectangle *geometry = &screen->base.geometry;

	/* Center cursor in the geometry of the first screen. */
	pointer->x = wl_fixed_from_int(geometry->x + geometry->width / 2);
	pointer->y = wl_fixed_from_int(geometry->y + geometry->height / 2);

	pointer->focus_handler.enter = enter;
	pointer->focus_handler.leave = leave;

	pointer->client_handler.motion = client_handle_motion;
	pointer->client_handler.button = client_handle_button;
	pointer->client_handler.axis = client_handle_axis;
	pointer->client_handler.frame = client_handle_frame;
	pointer->client_handler.pending = false;
	pointer->client_axis_source = -1;

	wl_list_init(&pointer->handlers);
	wl_list_insert(&pointer->handlers, &pointer->client_handler.link);
	wl_array_init(&pointer->buttons);

	view_initialize(&pointer->cursor.view, &view_impl);
	pointer->cursor.surface = NULL;
	pointer->cursor.destroy_listener.notify = &handle_cursor_surface_destroy;
	pointer->cursor.buffer = wld_create_buffer(
	    swc.drm->context,
	    swc.drm->cursor_w,
	    swc.drm->cursor_h,
	    WLD_FORMAT_ARGB8888,
	    WLD_FLAG_MAP | WLD_FLAG_CURSOR);
	pointer->cursor.internal_buffer = NULL;

	if (!pointer->cursor.buffer) {
		WARNING("Failed to create cursor buffer\n");
		return false;
	}

	pointer_set_cursor(pointer, cursor_left_ptr);

	wl_list_for_each (screen, &swc.screens, link)
		if (screen->planes.cursor)
			view_attach(&screen->planes.cursor->view, pointer->cursor.buffer);

	input_focus_initialize(&pointer->focus, &pointer->focus_handler);
	pixman_region32_init(&pointer->region);

	return true;
}

/**
 * \brief Finalizes the pointer and releases associated resources.
 *
 * Cleans up focus, regions, and other allocated resources.
 *
 * \param pointer The pointer structure to finalize.
 */
void
pointer_finalize(struct pointer *pointer)
{
	if (!pointer)
		return;

	input_focus_finalize(&pointer->focus);
	pixman_region32_fini(&pointer->region);
	wl_array_release(&pointer->buttons);

	if (pointer->cursor.internal_buffer) {
		wld_buffer_unreference(pointer->cursor.internal_buffer);
		pointer->cursor.internal_buffer = NULL;
	}

	if (pointer->cursor.buffer) {
		wld_buffer_unreference(pointer->cursor.buffer);
		pointer->cursor.buffer = NULL;
	}
}

/**
 * \brief Changes the pointer's input focus to a specific view.
 *
 * Updates the focus to the new view, triggering enter/leave events as needed.
 *
 * \param pointer The pointer instance.
 * \param view The new focused compositor view.
 */
void
pointer_set_focus(struct pointer *pointer, struct compositor_view *view)
{
	input_focus_set(&pointer->focus, view);
}

/**
 * \brief Clips the pointer coordinates to the defined visible region.
 *
 * Adjusts the pointer position to stay within the allowed region. If outside,
 * clips to the nearest boundary or resets to (0,0) if no valid box is found.
 *
 * \param pointer The pointer instance.
 * \param fx The proposed fixed-point X coordinate.
 * \param fy The proposed fixed-point Y coordinate.
 */
static void
clip_position(struct pointer *pointer, wl_fixed_t fx, wl_fixed_t fy)
{
	int32_t x = wl_fixed_to_int(fx);
	int32_t y = wl_fixed_to_int(fy);
	int32_t last_x = wl_fixed_to_int(pointer->x);
	int32_t last_y = wl_fixed_to_int(pointer->y);

	if (pixman_region32_contains_point(&pointer->region, x, y, NULL)) {
		pointer->x = fx;
		pointer->y = fy;
		return;
	}

	pixman_box32_t box;
	if (!pixman_region32_contains_point(&pointer->region, last_x, last_y, &box)) {
		WARNING("cursor is not in the visible screen area\n");
		pointer->x = wl_fixed_from_int(0);
		pointer->y = wl_fixed_from_int(0);
		return;
	}

	int32_t clipped_x = MAX(MIN(x, box.x2 - 1), box.x1);
	int32_t clipped_y = MAX(MIN(y, box.y2 - 1), box.y1);

	pointer->x = wl_fixed_from_int(clipped_x);
	pointer->y = wl_fixed_from_int(clipped_y);
}

/**
 * \brief Sets the allowed region for pointer movement.
 *
 * Copies the provided region and clips the current position to it.
 *
 * \param pointer The pointer instance.
 * \param region The new pixman region for clipping.
 */
void
pointer_set_region(struct pointer *pointer, pixman_region32_t *region)
{
	pixman_region32_copy(&pointer->region, region);
	clip_position(pointer, pointer->x, pointer->y);
}

/**
 * \brief Wayland protocol implementation for wl_pointer.set_cursor.
 *
 * Allows a focused client to set a custom cursor surface and hotspot. Ignores
 * requests from non-focused clients.
 *
 * \param client The Wayland client.
 * \param resource The pointer resource.
 * \param serial The event serial (unused).
 * \param surface_resource The surface resource for the cursor.
 * \param hotspot_x The X hotspot offset.
 * \param hotspot_y The Y hotspot offset.
 */
static void
set_cursor(struct wl_client *client, struct wl_resource *resource,
           uint32_t serial, struct wl_resource *surface_resource, int32_t hotspot_x, int32_t hotspot_y)
{
	struct pointer *pointer = wl_resource_get_user_data(resource);

	if (client != pointer->focus.client)
		return;

	if (pointer->cursor.surface) {
		surface_set_view(pointer->cursor.surface, NULL);
		if (!wl_list_empty(&pointer->cursor.destroy_listener.link))
			wl_list_remove(&pointer->cursor.destroy_listener.link);
		pointer->cursor.surface = NULL;
	}

	struct surface *surface = surface_resource
	                              ? wl_resource_get_user_data(surface_resource)
	                              : NULL;
	pointer->cursor.surface = surface;

	pointer->cursor.hotspot.x = hotspot_x;
	pointer->cursor.hotspot.y = hotspot_y;

	if (surface) {
		surface_set_view(surface, &pointer->cursor.view);
		wl_resource_add_destroy_listener(surface->resource,
		                                 &pointer->cursor.destroy_listener);
		update_cursor(pointer);
	}
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = set_cursor,
	.release = destroy_resource,
};

/**
 * \brief Callback for when a client unbinds from the wl_pointer interface.
 *
 * Removes the resource from the focus list.
 *
 * \param resource The unbound pointer resource.
 */
static void
unbind(struct wl_resource *resource)
{
	struct pointer *pointer = wl_resource_get_user_data(resource);
	input_focus_remove_resource(&pointer->focus, resource);
}

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
struct wl_resource *
pointer_bind(struct pointer *pointer, struct wl_client *client, uint32_t version, uint32_t id)
{
	struct wl_resource *client_resource;

	client_resource = wl_resource_create(client, &wl_pointer_interface, version, id);
	if (!client_resource)
		return NULL;

	wl_resource_set_implementation(client_resource, &pointer_impl, pointer, &unbind);
	input_focus_add_resource(&pointer->focus, client_resource);

	return client_resource;
}

/**
 * \brief Retrieves a button by its serial number.
 *
 * Searches the array of active buttons for a match.
 *
 * \param pointer The pointer instance.
 * \param serial The serial number of the button press.
 * \return The matching button, or NULL if not found.
 */
struct button *
pointer_get_button(struct pointer *pointer, uint32_t serial)
{
	struct button *button;

	wl_array_for_each (button, &pointer->buttons) {
		if (button->press.serial == serial)
			return button;
	}

	return NULL;
}

/**
 * \brief Handles a button press event.
 */
static void
handle_button_press(struct pointer *pointer, uint32_t time,
                    uint32_t button_code, uint32_t serial)
{
	struct button *b = wl_array_add(&pointer->buttons, sizeof(*b));
	if (!b)
		return;

	b->press.value = button_code;
	b->press.serial = serial;
	b->handler = NULL;

	/* Assign the first handler that accepts this button */
	struct pointer_handler *handler;
	wl_list_for_each (handler, &pointer->handlers, link) {
		if (handler->button && handler->button(handler, time, b, WL_POINTER_BUTTON_STATE_PRESSED)) {
			b->handler = handler;
			handler->pending = true;
			break;
		}
	}
}

/**
 * \brief Handles a button release event.
 */
static void
handle_button_release(struct pointer *pointer, uint32_t time,
                      uint32_t button_code, uint32_t serial)
{
	size_t count = pointer->buttons.size / sizeof(struct button);
	struct button *buttons = pointer->buttons.data;

	if (count == 0)
		return;

	for (size_t i = 0; i < count; ++i) {
		struct button *b = &buttons[i];

		if (b->press.value == button_code) {
			if (b->handler) {
				b->press.serial = serial;
				b->handler->button(b->handler, time, b, WL_POINTER_BUTTON_STATE_RELEASED);
				b->handler->pending = true;
			}

			array_remove(&pointer->buttons, b, sizeof(*b));
			count--;
			buttons = pointer->buttons.data;
			i--;
			break;
		}
	}
}

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
void
pointer_handle_button(struct pointer *pointer, uint32_t time, uint32_t value, uint32_t state)
{
	uint32_t serial = wl_display_next_serial(swc.display);

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		handle_button_release(pointer, time, value, serial);
	} else {
		handle_button_press(pointer, time, value, serial);
	}
}

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
void
pointer_handle_axis(struct pointer *pointer, uint32_t time, enum wl_pointer_axis axis, enum wl_pointer_axis_source source, wl_fixed_t value, int value120)
{
	struct pointer_handler *handler;

	wl_list_for_each (handler, &pointer->handlers, link) {
		if (handler->axis && handler->axis(handler, time, axis, source, value, value120)) {
			handler->pending = true;
			break;
		}
	}
}

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
void
pointer_handle_relative_motion(struct pointer *pointer, uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
	pointer_handle_absolute_motion(pointer, time, pointer->x + dx, pointer->y + dy);
}

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
void
pointer_handle_absolute_motion(struct pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct pointer_handler *handler;

	clip_position(pointer, x, y);

	wl_list_for_each (handler, &pointer->handlers, link) {
		if (handler->motion && handler->motion(handler, time, pointer->x, pointer->y)) {
			handler->pending = true;
			break;
		}
	}

	update_cursor(pointer);
}

/**
 * \brief Dispatches accumulated pointer events for the current frame.
 *
 * Calls frame handlers for pending events and resets pending flags. Updates the
 * cursor position after processing.
 *
 * \param pointer The pointer instance.
 */
void
pointer_handle_frame(struct pointer *pointer)
{
	struct pointer_handler *handler;

	wl_list_for_each (handler, &pointer->handlers, link) {
		if (handler->pending && handler->frame) {
			handler->frame(handler);
			handler->pending = false;
		}
	}

	update_cursor(pointer);
}
