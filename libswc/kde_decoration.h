/**
 * \file kde_decoration.h
 * \brief KDE server-side decoration manager for swc.
 *
 * \author Michael Forney
 * \date 2020
 * \copyright MIT
 *
 * This header declares the creation function for the KDE server-side decoration
 * manager global. The manager advertises support for the
 * org_kde_kwin_server_decoration_manager protocol to Wayland clients.
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

#ifndef SWC_KDE_DECORATION_H
#define SWC_KDE_DECORATION_H

struct wl_display;

/**
 * \brief Creates and registers the KDE server-side decoration manager global.
 *
 * Advertises the org_kde_kwin_server_decoration_manager protocol to Wayland
 * clients.
 *
 * \param[in] display The Wayland display to attach the global to.
 * \return A pointer to the created wl_global, or NULL on failure.
 */
struct wl_global *kde_decoration_manager_create(struct wl_display *display);

#endif
