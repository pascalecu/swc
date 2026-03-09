/**
 * \file keyboard.h
 * \brief Wayland keyboard input and XKB state management.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
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

#ifndef SWC_KEYBOARD_H
#define SWC_KEYBOARD_H

#include "input.h"

#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

/** \brief Keycodes are offset by 8 in XKB relative to evdev. */
#define XKB_KEY(key) ((key) + 8)

struct keyboard;
struct wl_client;

/**
 * \brief Represents a single key press event and its associated handler.
 */
struct key {
	struct press press;               /**< The press state metadata. */
	struct keyboard_handler *handler; /**< Logic to execute for this key. */
};

/**
 * \brief Represents the various states of keyboard modifiers.
 */
struct keyboard_modifier_state {
	uint32_t depressed; /**< Modifiers currently physically held down. */
	uint32_t latched;   /**< Modifiers active for the next non-modifier key. */
	uint32_t locked;    /**< Modifiers toggled on (e.g., Caps Lock). */
	uint32_t group;     /**< The current effective layout group. */
};

/**
 * \brief Interface for handling keyboard-specific events.
 */
struct keyboard_handler {
	/** \brief Handle a key event; returns true if consumed. */
	bool (*key)(struct keyboard *keyboard, uint32_t time, struct key *key, uint32_t state);

	/** \brief Handle a change in modifier state; returns true if consumed. */
	bool (*modifiers)(struct keyboard *keyboard, const struct keyboard_modifier_state *state);

	struct wl_list link; /**< Link for the handlers list. */
};

/**
 * \brief Internal XKB context and keymap state.
 */
struct xkb {
	struct xkb_context *context; /**< The library context. */
	struct xkb_state *state;     /**< The current state machine. */

	struct {
		struct xkb_keymap *map; /**< The compiled keymap. */
		int fd;                 /**< File descriptor for sharing keymap with clients. */
		uint32_t size;          /**< Size of the keymap string. */
		char *area;             /**< Mapped memory area of the keymap. */
	} keymap;

	struct {
		uint32_t ctrl, alt, super, shift; /**< Resolved modifier indices. */
	} indices;
};

/**
 * \brief The core keyboard object representing a seat's keyboard capabilities.
 */
struct keyboard {
	struct input_focus focus;                 /**< Current input focus state. */
	struct input_focus_handler focus_handler; /**< Logic for focus transitions. */
	struct xkb xkb;                           /**< XKB-specific state. */

	struct wl_array keys;                   /**< Currently pressed physical keys. */
	struct wl_list handlers;                /**< Registered event handlers. */
	struct keyboard_handler client_handler; /**< Default handler for Wayland clients. */
	struct wl_array client_keys;            /**< Keys currently sent to clients. */

	struct keyboard_modifier_state modifier_state; /**< Detailed XKB modifier state. */
	uint32_t modifiers;                            /**< Serialized modifier mask. */
};

/**
 * \brief Initializes a new keyboard object with a specific keymap.
 *
 * \param[in] names The XKB rule names (layout, variant, etc.) to use.
 * \return A pointer to the initialized keyboard, or NULL on failure.
 */
struct keyboard *keyboard_create(struct xkb_rule_names *names);

/**
 * \brief Frees all resources associated with the keyboard.
 *
 * \param[in,out] keyboard The keyboard object to destroy.
 */
void keyboard_destroy(struct keyboard *keyboard);

/**
 * \brief Resets the keyboard state, clearing pressed keys and modifiers.
 *
 * \param[in,out] keyboard The keyboard to reset.
 * \return True if the reset was successful.
 */
bool keyboard_reset(struct keyboard *keyboard);

/**
 * \brief Changes the input focus to a specific view.
 *
 * \param[in,out] keyboard The keyboard whose focus is changing.
 * \param[in] view The view to receive focus, or NULL to clear focus.
 */
void keyboard_set_focus(struct keyboard *keyboard, struct compositor_view *view);

/**
 * \brief Binds a client to the keyboard resource.
 *
 * \param[in,out] keyboard The keyboard global state.
 * \param[in] client The client binding the resource.
 * \param[in] version The requested protocol version.
 * \param[in] id The ID for the new resource.
 * \return The created Wayland resource.
 */
struct wl_resource *keyboard_bind(struct keyboard *keyboard, struct wl_client *client, uint32_t version, uint32_t id);

/**
 * \brief Processes a raw key event from the input backend.
 *
 * \param[in,out] keyboard The keyboard processing the event.
 * \param[in] time The timestamp of the event.
 * \param[in] value The raw keycode.
 * \param[in] state The press state (e.g., WL_KEYBOARD_KEY_STATE_PRESSED).
 */
void keyboard_handle_key(struct keyboard *keyboard, uint32_t time, uint32_t value, uint32_t state);

#endif
