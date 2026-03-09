/**
 * \file panel_manager.h
 * \brief Wayland panel manager global.
 * \author Michael Forney
 * \date 2014–-2019
 * \copyright MIT
 *
 * This module exposes the compositor's panel management interface to
 * Wayland clients through the `swc_panel_manager` global. Clients use
 * this interface to create panel surfaces associated with existing
 * Wayland surfaces.
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

#ifndef SWC_PANEL_MANAGER_H
#define SWC_PANEL_MANAGER_H

struct wl_display;

/**
 * \brief Create the panel manager global.
 *
 * Registers the `swc_panel_manager` global on the given Wayland display,
 * allowing clients to request panel objects.
 *
 * \param display Wayland display to register the global on.
 * \return The created wl_global object, or NULL on failure.
 */
struct wl_global *panel_manager_create(struct wl_display *display);

#endif
