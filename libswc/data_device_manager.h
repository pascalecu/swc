/**
 * \file data_device_manager.h
 *
 * \brief Wayland data device manager interface for swc.
 *
 * \author Michael Forney
 * \date 2013–2019
 * \copyright MIT
 *
 * \details
 * This module exposes the compositor-side implementation of the Wayland \c
 * wl_data_device_manager global interface.
 *
 * The data device manager is responsible for coordinating data transfer
 * mechanisms between Wayland clients, including:
 *
 * - Clipboard (selection) handling
 * - Drag-and-drop data exchange
 *
 * The interface is advertised to clients through the Wayland registry as a
 * global object. Clients bind to this global during initialization and may
 * subsequently:
 *
 * - Create \c wl_data_source objects representing transferable data.
 * - Obtain \c wl_data_device objects associated with a specific \c wl_seat.
 *
 * Each seat in the compositor maintains a data device responsible for handling
 * selection ownership and drag-and-drop interactions between clients.
 *
 * The compositor initializes this functionality by creating a single data
 * device manager global using ::data_device_manager_create().
 *
 * \see https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_device_manager
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

#ifndef SWC_DATA_DEVICE_MANAGER_H
#define SWC_DATA_DEVICE_MANAGER_H

struct wl_display;

/**
 * \brief Create the wl_data_device_manager global.
 * \details
 * Registers the wl_data_device_manager interface with the Wayland display so
 * that clients can bind to it through the registry.
 *
 * The global is advertised with protocol version 2.
 *
 * \param[in] display Wayland display used to register the global.
 * \return The created wl_global object.
 */
struct wl_global *data_device_manager_create(struct wl_display *display);

#endif
