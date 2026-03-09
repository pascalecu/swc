/**
 * \file drm.h
 *
 * \brief DRM/KMS backend initialization and management.
 *
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * This module manages the Direct Rendering Manager (DRM) file descriptors,
 * hardware-accelerated rendering contexts via wld, and the conversion of
 * generic buffers into hardware-backed framebuffers.
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

#ifndef SWC_DRM_H
#define SWC_DRM_H

#include <stdbool.h>
#include <stdint.h>

struct wl_list;
struct wld_buffer;

/**
 * \brief Interface for handling asynchronous DRM events.
 *
 * This structure acts as a callback table for DRM-specific events, such as
 * VSync-aligned page flips.
 */
struct drm_handler {
	/**
	 * \brief Invoked when a page flip has completed.
	 *
	 * \param[in,out] handler The handler instance receiving the event.
	 * \param[in]     time    Timestamp of the flip completion (in milliseconds).
	 */
	void (*page_flip)(struct drm_handler *handler, uint32_t time);
};

/**
 * \brief Main DRM backend state container.
 *
 * Encapsulates the hardware connection and the associated wld rendering
 * infrastructure.
 */
struct swc_drm {
	int fd;                        /**< Open file descriptor for the DRM device. */
	uint32_t cursor_w, cursor_h;   /**< Supported hardware cursor dimensions. */
	struct wld_context *context;   /**< The wld drawing context for this device. */
	struct wld_renderer *renderer; /**< The wld renderer assigned to the DRM device. */
};

/**
 * \brief Initializes the DRM subsystem.
 *
 * Locates a suitable DRM device, opens the file descriptor, and initializes
 * the wld rendering context.
 *
 * \return true if initialization was successful, false otherwise.
 */
bool drm_initialize(void);

/**
 * \brief Tears down the DRM subsystem.
 *
 * Releases all hardware resources, closes file descriptors, and destroys
 * the wld context.
 */
void drm_finalize(void);

/**
 * \brief Probes hardware for available connectors and creates screen objects.
 *
 * Scans the DRM device for active displays and populates the provided list
 * with initialized screen structures.
 *
 * \param[in,out] screens A pointer to an initialized Wayland list to be
 * populated with `struct screen` objects.
 *
 * \return true if at least one screen was successfully initialized.
 */
bool drm_create_screens(struct wl_list *screens);

/**
 * \brief Maps a wld_buffer to a DRM framebuffer ID.
 *
 * If the buffer does not already have an associated hardware framebuffer,
 * this function will attempt to create one via the DRM KMS API.
 *
 * \param[in] buffer The wld buffer to be mapped to the hardware.
 *
 * \return A non-zero DRM framebuffer ID (FBID) on success, or 0 on failure.
 */
uint32_t drm_get_framebuffer(struct wld_buffer *buffer);

#endif
