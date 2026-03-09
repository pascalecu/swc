/**
 * \file launch.h
 * \brief Helpers for communicating with the external launch helper.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * This module provides a small interface for communicating with an external
 * launch helper process. The helper performs privileged operations on behalf
 * of the compositor, such as opening device nodes or switching virtual
 * terminals.
 *
 * Communication occurs over a socket whose file descriptor is provided via
 * the `SWC_LAUNCH_SOCKET_ENV` environment variable.
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

#ifndef SWC_LAUNCH_H
#define SWC_LAUNCH_H

#include <stdbool.h>

/**
 * \brief Initialize the launch helper interface.
 *
 * Reads the launch socket descriptor from `SWC_LAUNCH_SOCKET_ENV`, sets
 * FD_CLOEXEC, and registers the socket with the compositor's Wayland event
 * loop.
 *
 * \return true on success, false if the helper is unavailable or initialization
 *         failed.
 */
bool launch_initialize(void);

/**
 * \brief Shut down the launch helper interface.
 *
 * Removes the event loop source and closes the launch socket.
 */
void launch_finalize(void);

/**
 * \brief Request that the helper open a device node.
 *
 * \param path Path of the device to open.
 * \param flags Flags passed to \c open(2).
 *
 * \return A file descriptor on success, or -1 on failure.
 *
 * The descriptor is created by the helper and transferred over the launch
 * socket. The caller is responsible for closing it.
 */
int launch_open_device(const char *path, int flags);

/**
 * \brief Request activation of a virtual terminal.
 *
 * \param vt Virtual terminal number.
 *
 * \return true if the helper successfully activated the VT, false otherwise.
 *
 * This call blocks until the helper replies.
 */
bool launch_activate_vt(unsigned vt);

#endif
