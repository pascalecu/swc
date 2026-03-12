/**
 * \file region.h
 * \brief Wayland region resource implementation.
 * \author Michael Forney
 * \date 2013–2020
 * \copyright MIT
 *
 * This module provides a minimal implementation of the Wayland
 * \c wl_region interface used by the compositor.
 *
 * A region represents a set of rectangles that can be used for input regions,
 * opaque regions, clipping and damage tracking.
 *
 * Internally, regions are backed by \c pixman_region32_t structures. Each \c
 * wl_region resource created through this module stores a pixman region which
 * is modified through the Wayland protocol requests \c add and \c subtract.
 *
 * The compositor can retrieve the underlying pixman region through \c
 * wl_resource_get_user_data().
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

#ifndef SWC_REGION_H
#define SWC_REGION_H

#include <stdint.h>

struct wl_client;

/**
 * \brief Create a new Wayland region resource.
 *
 * Allocates and initializes a \c pixman_region32_t and attaches it to a
 * new \c wl_region resource.
 *
 * \param client The Wayland client creating the region.
 * \param version The protocol version supported by the client.
 * \param id The object ID for the new resource.
 *
 * \return The newly created \c wl_resource, or NULL on failure.
 */
struct wl_resource *region_new(struct wl_client *client, uint32_t version, uint32_t id);

#endif
