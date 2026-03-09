/**
 * \file input.c
 * \brief Implementation of the input focus management system.
 *
 * This module manages the lifecycle of input focus, handling the transition of
 * focus between different compositor views and ensuring that the appropriate
 * Wayland events are dispatched to client resources.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
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

#include "input.h"
#include "compositor.h"
#include "event.h"
#include "surface.h"
#include "util.h"

/**
 * \brief Internal helper to grant focus to a view.
 *
 * This moves all input resources belonging to the view's client from the
 * inactive list to the active list and triggers the device-specific 'enter'
 * handler.
 *
 * \param[in,out] input_focus The focus manager state.
 * \param[in]     view        The view gaining focus.
 */
static void
focus(struct input_focus *input_focus, struct compositor_view *view)
{
	if (!view || !view->surface->resource)
		return;

	struct wl_client *client = wl_resource_get_client(view->surface->resource);
	input_focus->client = client;
	input_focus->view = view;

	struct wl_resource *resource, *tmp;

	wl_resource_for_each_safe(resource, tmp, &input_focus->inactive)
	{
		if (wl_resource_get_client(resource) == client) {
			struct wl_list *link = wl_resource_get_link(resource);
			wl_list_remove(link);
			wl_list_insert(&input_focus->active, link);
		}
	}

	wl_signal_add(&view->destroy_signal, &input_focus->view_destroy_listener);
	input_focus->handler->enter(input_focus->handler, &input_focus->active, view);
}

/**
 * \brief Internal helper to revoke focus from the current view.
 *
 * Notifies the client via the 'leave' handler and returns all active resources
 * to the inactive pool.
 *
 * \param[in,out] input_focus The focus manager state.
 */
static void
unfocus(struct input_focus *input_focus)
{
	if (!input_focus || !input_focus->view)
		return;

	wl_list_remove(&input_focus->view_destroy_listener.link);
	input_focus->handler->leave(input_focus->handler, &input_focus->active, input_focus->view);
	wl_list_insert_list(&input_focus->inactive, &input_focus->active);
	wl_list_init(&input_focus->active);

	input_focus->client = NULL;
	input_focus->view = NULL;
}

/**
 * \brief Callback for when a focused view is destroyed by the client or compositor.
 */
static void
handle_focus_view_destroy(struct wl_listener *listener, void *data)
{
	struct input_focus *input_focus = wl_container_of(listener, input_focus, view_destroy_listener);

	/* XXX: Should this call unfocus? */
	wl_list_insert_list(&input_focus->inactive, &input_focus->active);
	wl_list_init(&input_focus->active);
	input_focus->client = NULL;
	input_focus->view = NULL;
}

/**
 * \brief Initializes an input focus management structure.
 *
 * \param[in,out] input_focus   The structure to initialize.
 * \param[in]     input_handler The device-specific handler to use.
 * \return true on success, false otherwise.
 */
bool
input_focus_initialize(struct input_focus *input_focus, struct input_focus_handler *handler)
{
	if (!input_focus || !handler)
		return false;

	input_focus->client = NULL;
	input_focus->view = NULL;
	input_focus->view_destroy_listener.notify = &handle_focus_view_destroy;
	input_focus->handler = handler;

	wl_list_init(&input_focus->active);
	wl_list_init(&input_focus->inactive);
	wl_signal_init(&input_focus->event_signal);

	return true;
}

/**
 * \brief Finalizes and cleans up an input focus structure.
 *
 * \param[in,out] input_focus The structure to finalize.
 */
void
input_focus_finalize(struct input_focus *input_focus)
{
	if (input_focus->view) {
		wl_list_remove(&input_focus->view_destroy_listener.link);
	}
}

/**
 * \brief Adds a Wayland resource to the focus management lists.
 *
 * \param[in,out] input_focus The focus manager.
 * \param[in,out] resource    The Wayland resource (e.g., wl_keyboard) to add.
 */
void
input_focus_add_resource(struct input_focus *input_focus, struct wl_resource *resource)
{
	struct wl_list *link = wl_resource_get_link(resource);
	if (input_focus->client && wl_resource_get_client(resource) == input_focus->client) {
		wl_list_insert(&input_focus->active, link);

		struct wl_list temp_list;
		wl_list_init(&temp_list);
		wl_list_insert(&temp_list, link);
		input_focus->handler->enter(input_focus->handler, &temp_list, input_focus->view);

		/* Re-attach to the main active list after the specific notification */
		wl_list_remove(link);
		wl_list_insert(&input_focus->active, link);
	} else {
		wl_list_insert(&input_focus->inactive, link);
	}
}

/**
 * \brief Removes a Wayland resource from focus management.
 * \param[in,out] input_focus The focus manager.
 * \param[in,out] resource    The Wayland resource to remove.
 */
void
input_focus_remove_resource(struct input_focus *input_focus, struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

/**
 * \brief Sets the current input focus to a specific view.
 *
 * \param[in,out] input_focus The focus manager.
 * \param[in]     view        The view to focus, or NULL to clear focus.
 */
void
input_focus_set(struct input_focus *input_focus, struct compositor_view *view)
{
	if (view == input_focus->view)
		return;

	struct input_focus_event_data event_data = {
		.old = input_focus->view,
		.new = view
	};

	unfocus(input_focus);
	focus(input_focus, view);

	send_event(&input_focus->event_signal, INPUT_FOCUS_EVENT_CHANGED, &event_data);
}
