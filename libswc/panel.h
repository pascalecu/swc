/**
 * \file panel.h
 * \brief Panel role for Wayland surfaces.
 *
 * \author Michael Forney
 * \date 2014-2015
 * \copyright MIT
 *
 * This module defines the panel role used by the `swc_panel` protocol.
 * A panel is a surface that can be docked to an edge of a screen
 * (top, bottom, left, or right).
 *
 * Panels are typically used for desktop components such as taskbars,
 * docks, and status bars. When docked, a panel may reserve space along
 * the screen edge so that normal application windows avoid overlapping
 * it.
 *
 * Internally, panels are implemented as compositor views associated with
 * a Wayland surface. The panel object manages the view, updates its
 * position when necessary, and participates in the screen modifier
 * system to adjust the usable screen region.
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

#ifndef SWC_PANEL_H
#define SWC_PANEL_H

#include <stdint.h>

struct surface;
struct wl_client;

/**
 * \brief Create a panel for a surface.
 *
 * Assigns the panel role to the specified surface and creates the
 * associated compositor view.
 *
 * The returned object is owned by the Wayland resource created for the
 * client. It will be destroyed automatically when the client destroys
 * the panel resource or when the underlying surface is destroyed.
 *
 * \param client Wayland client requesting the panel.
 * \param version Version of the `swc_panel` interface.
 * \param id Resource identifier for the new panel object.
 * \param surface Surface that will back the panel view.
 *
 * \return Pointer to the newly created panel on success, or NULL if
 *         allocation or initialization fails.
 */
struct panel *panel_new(struct wl_client *client, uint32_t version, uint32_t id, struct surface *surface);

#endif
