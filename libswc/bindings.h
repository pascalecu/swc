/**
 * \file bindings.h
 * \brief Key and pointer binding definitions for swc
 * \author Michael Forney
 * \date 2013
 * \copyright MIT
 *
 * \details
 * This header defines the structures and functions used to manage
 * key and pointer bindings in swc. It provides access to the
 * keyboard and pointer handlers, and allows initialization and
 * cleanup of the binding system.
 *
 * The bindings system allows associating specific keys or pointer
 * buttons (with optional modifiers) to callback handlers that
 * will be invoked when the corresponding input event occurs.
 */

/*
 * Copyright (c) 2013 Michael Forney
 *
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

#ifndef SWC_BINDINGS_H
#define SWC_BINDINGS_H

#include <stdbool.h>

/**
 * \struct swc_bindings
 * \brief Exposes keyboard and pointer handlers for the seat.
 *
 * \details
 * This struct provides the public interface to the keyboard and pointer
 * binding subsystem. The seat forwards input events to these handlers, which
 * then invoke any registered key or button bindings.
 */
struct swc_bindings {
	struct keyboard_handler *keyboard_handler; /**< Handler for keyboard input events. */
	struct pointer_handler *pointer_handler;   /**< Handler for pointer button events. */
};

/**
 * \brief Initialize the binding subsystem.
 * \return true on success, false on failure.
 *
 * \details
 * This function must be called during compositor initialization before
 * registering any key or pointer bindings. It sets up internal storage
 * for the bindings arrays.
 */
bool bindings_initialize(void);

/**
 * \brief Finalize the binding subsystem.
 *
 * \details
 * Releases internal resources used for storing key and button bindings.
 * After this call, no binding handlers will be invoked.
 */
void bindings_finalize(void);

#endif /* SWC_BINDINGS_H */
