/**
 * \file event.h
 * \brief Internal event notification system for swc.
 * \author Michael Forney
 * \date 2013--2015
 * \copyright MIT
 *
 * This module provides a structured wrapper around Wayland's \c wl_signal
 * mechanism, allowing for type-safe event emission and unified event handling
 * across the compositor's internal components.
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

#ifndef SWC_EVENT_H
#define SWC_EVENT_H

#include <stdint.h>
#include <wayland-server.h>

/**
 * \brief Structured event data passed to signal listeners.
 *
 * Encapsulates an event type and its associated payload. This structure is used
 * as the \c data parameter in \c wl_listener.notify callbacks.
 */
struct event {
	/**
	 * \brief The specific type of event being triggered.
	 *
	 * The interpretation of this value is context-dependent, defined by the
	 * object emitting the signal.
	 */
	uint32_t type;

	/**
	 * \brief Event-specific metadata.
	 *
	 * A pointer to additional information relevant to the event type. Refer to
	 * the documentation of the specific event type to determine what this
	 * pointer targets.
	 */
	void *data;
};

/**
 * \brief Helper to emit a structured event through a Wayland signal.
 *
 * This function handles the boilerplate of initializing an event and calling \c
 * wl_signal_emit.
 *
 * \param[in,out] signal The Wayland signal to emit.
 * \param[in] type The compositor-specific event type ID.
 * \param[in] event_data The data payload to attach to the event.
 */
static inline void
send_event(struct wl_signal *signal, uint32_t type, void *event_data)
{
	struct event event = { .type = type, .data = event_data };
	wl_signal_emit(signal, &event);
}

#endif
