/**
 * \file bindings.c
 * \brief Key and pointer binding implementation for swc
 * \author Michael Forney
 * \date 2013
 * \copyright MIT
 */

/*
 * Copyright (c) 2013 Michael Forney
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

#include "bindings.h"
#include "internal.h"
#include "keyboard.h"
#include "pointer.h"
#include "seat.h"
#include "swc.h"
#include "util.h"

#include <errno.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

/**
 * \brief Represents a single key or button binding.
 *
 * A binding associates a key/button value and a modifier mask with a
 * handler callback. Bindings with modifier set to SWC_MOD_ANY match
 * regardless of the current modifier state.
 */
struct binding {
	uint32_t value;              /**< Key or button value/keysym. */
	uint32_t modifiers;          /**< Modifier mask (e.g., SWC_MOD_ALT). */
	swc_binding_handler handler; /**< Callback invoked on event. */
	void *data;                  /**< Opaque user data passed to the callback. */
};

/* Forward declarations for static/internals. These are not part of the public API. */
static bool handle_key(struct keyboard *keyboard, uint32_t time, struct key *key, uint32_t state);
static bool handle_button(struct pointer_handler *handler, uint32_t time, struct button *button, uint32_t state);

/** Keyboard event handler instance exposed via swc_bindings. */
static struct keyboard_handler key_binding_handler = {
	.key = handle_key,
};

/** Pointer (button) event handler instance exposed via swc_bindings. */
static struct pointer_handler button_binding_handler = {
	.button = handle_button,
};

/** Internal dynamic arrays storing registrations. */
static struct wl_array key_bindings, button_bindings;

/**
 * \brief Bindings exposed to the rest of the program (seat wiring point).
 *
 * The seat uses these handlers to forward key and button events into the
 * binding subsystem.
 */
const struct swc_bindings swc_bindings = {
	.keyboard_handler = &key_binding_handler,
	.pointer_handler = &button_binding_handler,
};

/**
 * \brief Find a binding by value and modifiers in an array.
 *
 * \param[in] bindings Array of \ref binding "bindings".
 * \param[in] modifiers Modifier mask to match (or \ref SWC_MOD_ANY).
 * \param[in] value Key/button value (for keyboard this is a keysym).
 * \return Pointer to a matching binding or \c NULL if none found.
 *
 * \note Matching treats \ref SWC_MOD_ANY as a wildcard for the modifiers field.
 */
static struct binding *
find_binding(struct wl_array *bindings, uint32_t modifiers, uint32_t value)
{
	struct binding *binding;

	wl_array_for_each (binding, bindings) {
		if (binding->value == value && (binding->modifiers == modifiers || binding->modifiers == SWC_MOD_ANY))
			return binding;
	}

	return NULL;
}

/**
 * \brief Find a key binding for the given physical key and modifier mask.
 *
 * Keys are resolved to keysyms using the current xkb state. This function first
 * tries the keysym produced by the current keyboard state (respecting layout
 * and modifiers). If that fails it falls back to the keysym at shift-level 0
 * (useful for matching letter-like key bindings).
 *
 * \param[in] modifiers Modifier mask to match (from seat keyboard state).
 * \param[in] key Physical keycode (as used by the key event).
 * \return Pointer to a matching \ref binding or \c NULL if none found.
 */
static struct binding *
find_key_binding(uint32_t modifiers, uint32_t key)
{
	struct binding *binding;
	struct xkb *xkb = &swc.seat->keyboard->xkb;
	xkb_keysym_t keysym;

	/* Try the keysym the current state produces. */
	keysym = xkb_state_key_get_one_sym(xkb->state, XKB_KEY(key));
	binding = find_binding(&key_bindings, modifiers, keysym);

	if (binding)
		return binding;

	xkb_layout_index_t layout;
	const xkb_keysym_t *keysyms;

	/* Fallback: try keysym at shift-level 0 for the current layout. */
	layout = xkb_state_key_get_layout(xkb->state, XKB_KEY(key));
	xkb_keymap_key_get_syms_by_level(xkb->keymap.map, XKB_KEY(key), layout, 0, &keysyms);

	if (!keysyms)
		return NULL;

	binding = find_binding(&key_bindings, modifiers, keysyms[0]);

	return binding;
}

/**
 * \brief Find a button binding for given modifiers and button code.
 *
 * \param[in] modifiers Modifier mask.
 * \param[in] value Button code.
 * \return Pointer to a matching \ref binding or \c NULL if none found.
 */
static struct binding *
find_button_binding(uint32_t modifiers, uint32_t value)
{
	return find_binding(&button_bindings, modifiers, value);
}

/**
 * \brief Generic handler used by both key and button handlers.
 *
 * \param[in] time Event timestamp (ms).
 * \param[in,out] press The press struct (key/button) whose \ref press.data
 * "data field" may be used to store binding.
 * \param[in] state Non-zero for press, zero for release.
 * \param[in] find_binding Function pointer used to resolve the binding for the
 * press.
 * \return true if the event was handled by a binding, false otherwise.
 *
 * On press, this resolves the binding using \p find_binding with the
 * current keyboard modifier mask (from the seat). If a binding is found it
 * stores it in \c press->data and invokes the handler. On release it retrieves
 * the previously stored binding from \c press->data and invokes the handler.
 */
static bool
handle_binding(uint32_t time, struct press *press, uint32_t state, struct binding *(*find_binding)(uint32_t, uint32_t))
{
	struct binding *binding;

	if (state) {
		/* Press: look up binding using current modifiers and store for release */
		binding = find_binding(swc.seat->keyboard->modifiers, press->value);
		press->data = binding;
	} else {
		/* Release: reuse previously stored binding */
		binding = press->data;
	}

	if (!binding)
		return false;

	binding->handler(binding->data, time, binding->value, state);
	return true;
}

/**
 * \brief Keyboard event handler: dispatch keys to bindings.
 *
 * \param[in] keyboard Keyboard source object (unused).
 * \param[in] time Event timestamp.
 * \param[in] key Key object containing press information.
 * \param[in] state 1 for press, 0 for release.
 * \return true if a binding handled the event.
 */
bool
handle_key(struct keyboard *keyboard, uint32_t time, struct key *key, uint32_t state)
{
	return handle_binding(time, &key->press, state, &find_key_binding);
}

/**
 * \brief Pointer button event handler: dispatch buttons to bindings.
 *
 * \param[in] handler Pointer handler source (unused).
 * \param[in] time Event timestamp.
 * \param[in] button Button object containing press information.
 * \param[in] state 1 for press, 0 for release.
 * \return true if a binding handled the event.
 */
bool
handle_button(struct pointer_handler *handler, uint32_t time, struct button *button, uint32_t state)
{
	(void)handler;
	return handle_binding(time, &button->press, state, &find_button_binding);
}

/**
 * \brief Register a new binding.
 *
 * \param[in] type Binding type: \c SWC_BINDING_KEY or \c SWC_BINDING_BUTTON.
 * \param[in] modifiers Modifier mask required for the binding (or \c SWC_MOD_ANY).
 * \param[in] value Key keysym or button code.
 * \param[in] handler Callback invoked when the binding triggers.
 * \param[in] data Opaque user-provided pointer passed to \p handler.
 * \return 0 on success, -EINVAL for invalid \p type, or -ENOMEM on allocation failure.
 *
 * \note
 * The binding data is stored in a wl_array internal to this module;
 * callers should not free \p data (it is provided by the caller).
 */
EXPORT int
swc_add_binding(enum swc_binding_type type, uint32_t modifiers, uint32_t value, swc_binding_handler handler, void *data)
{
	struct binding *binding;
	struct wl_array *bindings;

	switch (type) {
	case SWC_BINDING_KEY:
		bindings = &key_bindings;
		break;
	case SWC_BINDING_BUTTON:
		bindings = &button_bindings;
		break;
	default:
		return -EINVAL;
	}

	binding = wl_array_add(bindings, sizeof(*binding));
	if (!binding)
		return -ENOMEM;

	binding->value = value;
	binding->modifiers = modifiers;
	binding->handler = handler;
	binding->data = data;

	return 0;
}

/**
 * \brief Initialize the binding subsystem.
 * \return true on success, false on failure.
 *
 * This function must be called during compositor initialization before
 * registering any key or pointer bindings. It sets up internal storage
 * for the bindings arrays.
 */
bool
bindings_initialize(void)
{
	wl_array_init(&key_bindings);
	wl_array_init(&button_bindings);

	return true;
}

/**
 * \brief Finalize the binding subsystem.
 *
 * Releases internal resources used for storing key and button bindings.
 * After this call, no binding handlers will be invoked.
 */
void
bindings_finalize(void)
{
	wl_array_release(&key_bindings);
	wl_array_release(&button_bindings);
}
