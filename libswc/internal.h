/**
 * \file internal.h
 * \brief Global state and core lifecycle management for swc.
 *
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * This header defines the main context structure for the compositor, providing
 * access to all subsystems and global Wayland objects.
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

#ifndef SWC_INTERNAL_H
#define SWC_INTERNAL_H

#include <stdbool.h>
#include <wayland-server.h>

/**
 * \brief Global swc event types.
 */
enum {
	SWC_EVENT_ACTIVATED,   /**< Compositor has gained control (e.g., VT switch in). */
	SWC_EVENT_DEACTIVATED, /**< Compositor has lost control (e.g., VT switch out). */
};

/**
 * \brief The central state structure for the swc compositor.
 *
 * This structure acts as the primary registry for all components of the
 * compositor, including the Wayland display, event loop, and hardware backends.
 */
struct swc {
	struct wl_display *display;        /**< The Wayland display server instance. */
	struct wl_event_loop *event_loop;  /**< The main event loop for polling and timers. */
	const struct swc_manager *manager; /**< User-provided manager for external logic. */
	struct wl_signal event_signal;     /**< Signal for global swc events (activated/deactivated). */
	bool active;                       /**< True if the compositor is currently active/visible. */

	struct swc_seat *seat;                     /**< The primary input seat (keyboard, pointer, touch). */
	const struct swc_bindings *const bindings; /**< Key and button binding registry. */
	struct wl_list screens;                    /**< List of initialized output screens (struct screen). */
	struct swc_compositor *const compositor;   /**< The core Wayland compositor logic. */
	struct swc_shm *shm;                       /**< Shared memory buffer management. */
	struct swc_drm *const drm;                 /**< Direct Rendering Manager (KMS/GBM) backend. */

	/** \name Global Interfaces
	 * Standard and vendor-specific Wayland protocol globals.
	 * @{ */
	struct wl_global *data_device_manager;
	struct wl_global *kde_decoration_manager;
	struct wl_global *panel_manager;
	struct wl_global *shell;
	struct wl_global *subcompositor;
	struct wl_global *xdg_decoration_manager;
	struct wl_global *xdg_shell;
	/** @} */

#ifdef ENABLE_XWAYLAND
	const struct swc_xserver *const xserver; /**< XWayland integration state. */
#endif
};

/**
 * \brief Global swc instance.
 *
 * This singleton is accessed throughout the codebase to reach various
 * compositor subsystems.
 */
extern struct swc swc;

/**
 * \brief Transitions the compositor to an active state.
 *
 * Updates the global \c active flag and notifies all subsystems via \c
 * swc.event_signal and the \c swc.manager callback.
 */
void swc_activate(void);

/**
 * \brief Transitions the compositor to an inactive state.
 *
 * Updates the global \c active flag and notifies all subsystems via \c
 * swc.event_signal and the \c swc.manager callback.
 */
void swc_deactivate(void);

#endif
