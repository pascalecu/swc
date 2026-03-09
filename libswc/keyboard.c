/**
 * \file keyboard.c
 * \brief Implementation of Wayland keyboard input and XKB state management.
 *
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * Based in part upon input.c from weston, which is:
 * \copyright © 2013 Intel Corporation
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

#include "keyboard.h"
#include "compositor.h"
#include "internal.h"
#include "surface.h"
#include "swc.h"
#include "util.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static const int repeat_delay = 500;
static const int repeat_rate = 40;

/**
 * \brief Handles keyboard focus entering a surface.
 *
 * \param[in,out] handler The focus handler attached to the keyboard.
 * \param[in] resources List of client resources to notify.
 * \param[in] view The view receiving focus.
 */
static void
enter(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view)
{
	if (!handler || !resources || !view || !view->surface)
		return;

	struct keyboard *keyboard = wl_container_of(handler, keyboard, focus_handler);
	struct keyboard_modifier_state *state = &keyboard->modifier_state;

	uint32_t serial = wl_display_next_serial(swc.display);

	struct wl_resource *resource;
	wl_resource_for_each (resource, resources) {
		wl_keyboard_send_modifiers(resource, serial, state->depressed, state->locked, state->latched, state->group);
		wl_keyboard_send_enter(resource, serial, view->surface->resource, &keyboard->client_keys);
	}
}

/**
 * \brief Handles keyboard focus leaving a surface.
 *
 * \param[in,out] handler The focus handler attached to the keyboard.
 * \param[in] resources List of client resources to notify.
 * \param[in] view The view losing focus.
 */
static void
leave(struct input_focus_handler *handler, struct wl_list *resources, struct compositor_view *view)
{
	if (!handler || !resources || !view || !view->surface)
		return;

	struct wl_resource *resource;
	uint32_t serial = wl_display_next_serial(swc.display);
	wl_resource_for_each (resource, resources) {
		wl_keyboard_send_leave(resource, serial, view->surface->resource);
	}
}

/**
 * \brief Internal helper to update the set of keys currently held by a client.
 */
static bool
update_client_key_state(struct wl_array *keys, uint32_t keycode, uint32_t state)
{
	uint32_t *k;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (!(k = wl_array_add(keys, sizeof(*k))))
			return false;

		*k = keycode;
		return true;
	}

	wl_array_for_each (k, keys) {
		if (*k == keycode) {
			array_remove(keys, k, sizeof(*k));
			return true;
		}
	}

	return true;
}

/**
 * \brief Default client handler for key press/release events.
 *
 * \param[in,out] keyboard The keyboard object.
 * \param[in] time The event timestamp.
 * \param[in] key The key being pressed or released.
 * \param[in] state The key state (WL_KEYBOARD_KEY_STATE_PRESSED/RELEASED).
 * \return True if the event was processed, false on memory failure.
 */
static bool
client_handle_key(struct keyboard *keyboard, uint32_t time, struct key *key, uint32_t state)
{
	struct wl_resource *resource;
	uint32_t keycode = key->press.value;

	/* Sync the logical state of keys held by the client */
	if (!update_client_key_state(&keyboard->client_keys, keycode, state))
		return false;

	/* Dispatch the event to all active resources for this client */
	wl_resource_for_each (resource, &keyboard->focus.active) {
		wl_keyboard_send_key(resource, key->press.serial, time, keycode, state);
	}

	return true;
}

/**
 * \brief Default client handler for modifier state changes.
 *
 * \param[in,out] keyboard The keyboard object.
 * \param[in] state The newly updated modifier state.
 * \return True if modifiers were sent to active clients, false if no clients are active.
 */
static bool
client_handle_modifiers(struct keyboard *keyboard, const struct keyboard_modifier_state *state)
{
	struct wl_resource *resource;

	if (wl_list_empty(&keyboard->focus.active))
		return false;

	uint32_t serial = wl_display_next_serial(swc.display);
	wl_resource_for_each (resource, &keyboard->focus.active)
		wl_keyboard_send_modifiers(resource, serial, state->depressed, state->locked, state->latched, state->group);
	return true;
}

/**
 * \brief Serializes the XKB keymap and creates a shared memory file descriptor.
 *
 * Wayland requires the compositor to send the keymap to clients via an fd.
 * This function generates that fd and mmaps it for reading.
 *
 * \param[in,out] xkb The internal XKB state tracking structure.
 * \return True on success, false on failure.
 */
static bool
update_keymap(struct xkb *xkb)
{
	char keymap_path[PATH_MAX];
	const char *xdg_dir = getenv("XDG_RUNTIME_DIR");
	const char *tmp_dir = xdg_dir ? xdg_dir : "/tmp";

	/* Map modifier names to indices for faster bitmask calculation */
	xkb->indices.ctrl = xkb_keymap_mod_get_index(xkb->keymap.map, XKB_MOD_NAME_CTRL);
	xkb->indices.alt = xkb_keymap_mod_get_index(xkb->keymap.map, XKB_MOD_NAME_ALT);
	xkb->indices.super = xkb_keymap_mod_get_index(xkb->keymap.map, XKB_MOD_NAME_LOGO);
	xkb->indices.shift = xkb_keymap_mod_get_index(xkb->keymap.map, XKB_MOD_NAME_SHIFT);

	char *keymap_string = xkb_keymap_get_as_string(xkb->keymap.map, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!keymap_string) {
		WARNING("XKB: Failed to serialize keymap\n");
		return false;
	}

	if (snprintf(keymap_path, sizeof(keymap_path), "%s/swc-xkb-XXXXXX", tmp_dir) >= (int)sizeof(keymap_path)) {
		goto error_string;
	}

	xkb->keymap.size = strlen(keymap_string) + 1;
	xkb->keymap.fd = mkostemp(keymap_path, O_CLOEXEC);
	if (xkb->keymap.fd == -1) {
		goto error_string;
	}

	unlink(keymap_path);

	if (posix_fallocate(xkb->keymap.fd, 0, xkb->keymap.size) != 0 && ftruncate(xkb->keymap.fd, xkb->keymap.size) != 0) {
		goto error_fd;
	}

	xkb->keymap.area = mmap(NULL, xkb->keymap.size, PROT_READ | PROT_WRITE, MAP_SHARED, xkb->keymap.fd, 0);
	if (xkb->keymap.area == MAP_FAILED) {
		goto error_fd;
	}

	memcpy(xkb->keymap.area, keymap_string, xkb->keymap.size);
	free(keymap_string);
	return true;

error_fd:
	close(xkb->keymap.fd);
error_string:
	free(keymap_string);
	return false;
}

struct keyboard *
keyboard_create(struct xkb_rule_names *names)
{
	struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard)
		return NULL;

	struct xkb *xkb = &keyboard->xkb;

	if (!(xkb->context = xkb_context_new(0))) {
		ERROR("Could not create XKB context\n");
		goto error_kb;
	}

	if (!(xkb->keymap.map = xkb_keymap_new_from_names(xkb->context, names, 0))) {
		ERROR("Could not create XKB keymap\n");
		goto error_context;
	}

	if (!(xkb->state = xkb_state_new(xkb->keymap.map))) {
		ERROR("Could not create XKB state\n");
		goto error_keymap;
	}

	if (!update_keymap(xkb)) {
		ERROR("Could not update XKB keymap\n");
		goto error_state;
	}

	if (!input_focus_initialize(&keyboard->focus, &keyboard->focus_handler)) {
		goto error_state;
	}

	keyboard->focus_handler.enter = enter;
	keyboard->focus_handler.leave = leave;

	keyboard->client_handler.key = client_handle_key;
	keyboard->client_handler.modifiers = client_handle_modifiers;

	wl_array_init(&keyboard->client_keys);
	wl_array_init(&keyboard->keys);
	wl_list_init(&keyboard->handlers);
	wl_list_insert(&keyboard->handlers, &keyboard->client_handler.link);

	return keyboard;

error_state:
	xkb_state_unref(keyboard->xkb.state);
error_keymap:
	xkb_keymap_unref(keyboard->xkb.keymap.map);
error_context:
	xkb_context_unref(keyboard->xkb.context);
error_kb:
	free(keyboard);

	return NULL;
}

void
keyboard_destroy(struct keyboard *keyboard)
{
	if (!keyboard)
		return;

	wl_array_release(&keyboard->client_keys);
	wl_array_release(&keyboard->keys);
	input_focus_finalize(&keyboard->focus);

	if (keyboard->xkb.keymap.area)
		munmap(keyboard->xkb.keymap.area, keyboard->xkb.keymap.size);
	if (keyboard->xkb.keymap.fd != -1)
		close(keyboard->xkb.keymap.fd);

	xkb_state_unref(keyboard->xkb.state);
	xkb_keymap_unref(keyboard->xkb.keymap.map);
	xkb_context_unref(keyboard->xkb.context);

	free(keyboard);
}

bool
keyboard_reset(struct keyboard *keyboard)
{
	struct key *key;
	uint32_t time = get_time();

	/* Send simulated key release events for all current key handlers. */
	wl_array_for_each (key, &keyboard->keys) {
		if (key->handler) {
			key->press.serial = wl_display_next_serial(swc.display);
			key->handler->key(keyboard, time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
			/* Don't bother updating the XKB state because we will be resetting
			 * it later on and it is unlikely that a key handler cares about the
			 * keyboard state for release events. */
		}
	}

	/* We should have removed all the client keys by calling the client key
	 * handler. */
	assert(keyboard->client_keys.size == 0);
	keyboard->keys.size = 0;

	memset(&keyboard->modifier_state, 0, sizeof(keyboard->modifier_state));
	keyboard->modifiers = 0;

	struct xkb_state *new_state = xkb_state_new(keyboard->xkb.keymap.map);
	if (!new_state) {
		ERROR("Failed to allocate new XKB state\n");
		return false;
	}

	xkb_state_unref(keyboard->xkb.state);
	keyboard->xkb.state = new_state;

	return true;
}

/**
 * Sets the focus of the keyboard to the specified surface.
 */
void
keyboard_set_focus(struct keyboard *keyboard, struct compositor_view *view)
{
	input_focus_set(&keyboard->focus, view);
}

static const struct wl_keyboard_interface keyboard_impl = {
	.release = destroy_resource,
};

/**
 * \brief Cleanup callback triggered when a client unbinds the keyboard resource.
 *
 * \param[in,out] resource The Wayland resource being destroyed.
 */
static void
unbind(struct wl_resource *resource)
{
	struct keyboard *keyboard = wl_resource_get_user_data(resource);
	input_focus_remove_resource(&keyboard->focus, resource);
}

struct wl_resource *
keyboard_bind(struct keyboard *keyboard, struct wl_client *client, uint32_t version, uint32_t id)
{
	struct wl_resource *client_resource =
	    wl_resource_create(client, &wl_keyboard_interface, version, id);

	if (!client_resource)
		return NULL;

	wl_resource_set_implementation(client_resource, &keyboard_impl, keyboard, &unbind);

	/* Subtract one to remove terminating NULL character. */
	wl_keyboard_send_keymap(
	    client_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
	    keyboard->xkb.keymap.fd, keyboard->xkb.keymap.size - 1);

	input_focus_add_resource(&keyboard->focus, client_resource);

	if (version >= 4)
		wl_keyboard_send_repeat_info(client_resource, repeat_rate, repeat_delay);

	return client_resource;
}

void
keyboard_handle_key(struct keyboard *keyboard, uint32_t time, uint32_t value, uint32_t state)
{
	struct key *key;
	struct keyboard_modifier_state modifier_state;
	enum xkb_key_direction direction;
	struct xkb *xkb = &keyboard->xkb;
	struct keyboard_handler *handler;

	uint32_t serial = wl_display_next_serial(swc.display);

	/* First handle key release events associated with a particular handler. */
	wl_array_for_each (key, &keyboard->keys) {
		if (key->press.value != value)
			continue;

		/* Ignore repeat events. */
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
			return;

		if (key->handler) {
			key->press.serial = serial;
			key->handler->key(keyboard, time, key, state);
		}

		array_remove(&keyboard->keys, key, sizeof(*key));
		goto update_xkb_state;
	}

	/* If we get a unpaired release event, just ignore it. */
	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	if (!(key = wl_array_add(&keyboard->keys, sizeof(*key))))
		goto update_xkb_state;

	key->press.value = value;
	key->press.serial = serial;
	key->handler = NULL;

	/* Go through handlers to see if any will accept this key event. */
	wl_list_for_each (handler, &keyboard->handlers, link) {
		if (handler->key && handler->key(keyboard, time, key, state)) {
			key->handler = handler;
			break;
		}
	}

	/* Update XKB state. */
update_xkb_state:
	direction = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? XKB_KEY_DOWN : XKB_KEY_UP;
	xkb_state_update_key(xkb->state, XKB_KEY(value), direction);

	modifier_state.depressed = xkb_state_serialize_mods(xkb->state, XKB_STATE_DEPRESSED);
	modifier_state.latched = xkb_state_serialize_mods(xkb->state, XKB_STATE_LATCHED);
	modifier_state.locked = xkb_state_serialize_mods(xkb->state, XKB_STATE_LOCKED);
	modifier_state.group = xkb_state_serialize_layout(xkb->state, XKB_STATE_LAYOUT_EFFECTIVE);

	if (memcmp(&modifier_state, &keyboard->modifier_state, sizeof(modifier_state)) != 0) {
		uint32_t mods_active = modifier_state.depressed | modifier_state.latched;

		/* Update keyboard modifier state. */
		keyboard->modifier_state = modifier_state;
		keyboard->modifiers = 0;

		if (mods_active & (1 << xkb->indices.ctrl))
			keyboard->modifiers |= SWC_MOD_CTRL;

		if (mods_active & (1 << xkb->indices.alt))
			keyboard->modifiers |= SWC_MOD_ALT;

		if (mods_active & (1 << xkb->indices.super))
			keyboard->modifiers |= SWC_MOD_LOGO;

		if (mods_active & (1 << xkb->indices.shift))
			keyboard->modifiers |= SWC_MOD_SHIFT;

		/* Run any modifier handlers. */
		wl_list_for_each (handler, &keyboard->handlers, link) {
			if (handler->modifiers)
				handler->modifiers(keyboard, &modifier_state);
		}
	}
}
