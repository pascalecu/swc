/**
 * \file dmabuf.h
 *
 * \brief Public interface for the compositor-side zwp_linux_dmabuf_v1 protocol.
 *
 * \author Michael Forney
 * \date 2019
 * \copyright MIT
 *
 * This header provides the public API required to initialize and register the
 * linux-dmabuf Wayland protocol extension within the compositor. This extension
 * allows clients to share hardware-backed graphics buffers (DMA-BUFs) directly
 * with the compositor, enabling highly efficient, zero-copy rendering
 * workflows.
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

#ifndef SWC_DMABUF_H
#define SWC_DMABUF_H

struct wl_display;

/**
 * \brief Initializes and registers the zwp_linux_dmabuf_v1 global.
 *
 * This function sets up the linux-dmabuf Wayland protocol extension and binds
 * it to the provided Wayland display. Once registered, connected clients can
 * discover the global, query supported DRM formats and modifiers, and begin
 * negotiating DMA-BUF imports.
 *
 * \param[in] display The core Wayland display instance where the global will
 * be advertised.
 *
 * \return A pointer to the newly created `wl_global` object representing the
 * extension, or `NULL` if resource allocation or registration fails.
 */
struct wl_global *swc_dmabuf_create(struct wl_display *display);

#endif
