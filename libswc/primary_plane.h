/**
 * \file primary_plane.h
 * \brief DRM primary plane abstraction.
 * \author Michael Forney
 * \date 2013, 2016
 * \copyright MIT
 *
 * This module defines the primary plane abstraction used by the compositor to
 * present framebuffers on a DRM CRTC. A primary plane represents the fullscreen
 * scanout surface for an output and integrates with the compositor's view
 * system.
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

#ifndef SWC_PRIMARY_PLANE_H
#define SWC_PRIMARY_PLANE_H

#include "drm.h"
#include "mode.h"
#include "view.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

/**
 * \brief Represents a DRM primary plane attached to a CRTC.
 *
 * The primary plane encapsulates all state required to display framebuffers
 * on a DRM CRTC. It owns a \ref view which acts as the rendering target for
 * the compositor.
 *
 * A primary plane typically corresponds to the fullscreen scanout buffer
 * for an output device.
 */
struct primary_plane {

	/** DRM CRTC identifier controlled by this primary plane. */
	uint32_t crtc;

	/**
	 * Saved CRTC state from before initialization.
	 *
	 * This state is restored during \ref primary_plane_finalize() so that
	 * the original display configuration is preserved when the compositor
	 * shuts down or releases the device.
	 */
	drmModeCrtcPtr original_crtc_state;

	/** Display mode used when performing a modeset on the CRTC. */
	struct mode mode;

	/**
	 * View associated with this primary plane.
	 *
	 * The compositor attaches buffers to this view which are then
	 * presented on the CRTC using page flips or modesets.
	 */
	struct view view;

	/**
	 * Array of connector IDs attached to this CRTC.
	 *
	 * Stored as a Wayland dynamic array containing `uint32_t` connector IDs.
	 */
	struct wl_array connectors;

	/**
	 * Indicates whether the next buffer attachment must perform a full modeset.
	 *
	 * This is typically set when the compositor is activated or when the
	 * display configuration needs to be re-applied.
	 */
	bool need_modeset;

	/**
	 * DRM event handler used for page flip completion events.
	 *
	 * The handler notifies the compositor when a page flip finishes so that
	 * the next frame can be scheduled.
	 */
	struct drm_handler drm_handler;

	/**
	 * Listener for compositor events.
	 *
	 * Used to receive events such as activation so the plane can trigger
	 * a modeset when required.
	 */
	struct wl_listener swc_listener;
};

/**
 * \brief Initialize a primary plane structure.
 *
 * This sets up a primary plane that will control the specified CRTC and
 * connectors. The original CRTC state is stored so it can be restored in
 * \ref primary_plane_finalize().
 *
 * \param plane Output pointer to the primary_plane structure to initialize.
 * \param crtc The CRTC id this plane will drive.
 * \param mode The desired mode to use on modeset.
 * \param connectors Array of connector ids to attach to the CRTC.
 * \param num_connectors Number of entries in \p connectors.
 * \return true on success, false on failure.
 */
bool primary_plane_initialize(struct primary_plane *plane, uint32_t crtc, struct mode *mode, uint32_t *connectors, uint32_t num_connectors);

/**
 * \brief Finalize a primary plane and restore the original CRTC configuration.
 *
 * This function attempts to restore the CRTC to the state it had when the plane
 * was initialized. Any resources allocated by primary_plane_initialize() are
 * released.
 *
 * \param plane The primary_plane to finalize (must not be NULL).
 */
void primary_plane_finalize(struct primary_plane *plane);

#endif
