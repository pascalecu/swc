/**
 * \file dmabuf.c
 *
 * \brief Compositor-side implementation of the zwp_linux_dmabuf_v1 protocol.
 *
 * \author Michael Forney
 * \date 2019
 * \copyright MIT
 *
 * This module implements DMA-BUF import capabilities via the unstable
 * zwp_linux_dmabuf_v1 Wayland protocol. It handles the lifecycle of buffer
 * parameter objects (zwp_linux_buffer_params_v1), accepts DMA-BUF file
 * descriptors from clients, and securely imports them into the compositor's
 * buffer backend (wld).
 *
 * The current implementation supports single-plane XRGB/ARGB formats and
 * advertises baseline format support to connected clients. Multi-plane and
 * advanced modifier support are designed to be integrated in future iterations.
 *
 * \see https://wayland.app/protocols/linux-dmabuf-v1
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

#include "dmabuf.h"
#include "drm.h"
#include "internal.h"
#include "util.h"
#include "wayland_buffer.h"

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <wld/drm.h>
#include <wld/wld.h>

/** Maximum number of memory planes supported per DMA-BUF buffer. */
#define MAX_PLANES 4

/**
 * \brief Encapsulates the state for a zwp_linux_buffer_params_v1 object.
 *
 * This structure tracks the configuration of a pending DMA-BUF buffer before
 * its instantiation. It maintains file descriptors, memory offsets, strides,
 * and DRM modifiers for each plane, alongside a flag to prevent duplicate
 * buffer creation from a single parameters object.
 */
struct params {
	struct wl_resource *resource;  /**< The Wayland resource tracking this params object. */
	int fd[MAX_PLANES];            /**< DMA-BUF file descriptors for each individual plane. */
	uint32_t offset[MAX_PLANES];   /**< Byte offsets to the start of data for each plane. */
	uint32_t stride[MAX_PLANES];   /**< Distance in bytes between consecutive rows for each plane. */
	uint64_t modifier[MAX_PLANES]; /**< DRM format modifiers specifying memory layout per plane. */
	bool created;                  /**< Sentinel flag indicating if a buffer has already been realized. */
};

/**
 * \brief Appends a DMA-BUF plane to the buffer parameters.
 *
 * This function handles the `zwp_linux_buffer_params_v1.add` request. It
 * validates the target plane index, ensures the params object has not already
 * been finalized, and verifies that the specific plane slot is empty.
 *
 * \note On failure, this function assumes responsibility for closing the
 * provided file descriptor before dispatching the appropriate Wayland protocol
 * error.
 *
 * \param[in] client      The Wayland client issuing the request.
 * \param[in] resource    The params object resource.
 * \param[in] fd          File descriptor for the plane. Ownership is transferred to the compositor.
 * \param[in] plane_idx   Zero-based index of the plane being added.
 * \param[in] offset      Starting byte offset within the DMA-BUF.
 * \param[in] stride      Number of bytes between start of consecutive rows.
 * \param[in] modifier_hi The upper 32 bits of the 64-bit DRM modifier.
 * \param[in] modifier_lo The lower 32 bits of the 64-bit DRM modifier.
 *
 * \see https://wayland.app/protocols/linux-dmabuf-v1#zwp_linux_buffer_params_v1:request:add
 */
static void
add(struct wl_client *client, struct wl_resource *resource, int32_t fd, uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo)
{
	(void)client;

	struct params *params = wl_resource_get_user_data(resource);
	if (!params) {
		/* Nothing we can do; close provided fd and report incomplete params. */
		close(fd);
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
		                       "invalid params object");
		return;
	}

	if (params->created) {
		close(fd);
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
		                       "buffer already created");
		return;
	}

	if (plane_idx >= MAX_PLANES) {
		close(fd);
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
		                       "plane index too large");
		return;
	}

	if (params->fd[plane_idx] != -1) {
		close(fd);
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
		                       "buffer plane already set");
		return;
	}

	params->fd[plane_idx] = fd;
	params->offset[plane_idx] = offset;
	params->stride[plane_idx] = stride;
	params->modifier[plane_idx] = (uint64_t)modifier_hi << 32 | modifier_lo;
}

/**
 * \brief Immediately constructs a Wayland buffer from the parameter state.
 *
 * Handles the `zwp_linux_buffer_params_v1.create_immed` request. This function
 * verifies the completeness of the provided planes against the requested DRM
 * format, delegates the import process to the `wld` backend, and creates a
 * corresponding `wl_buffer` resource.
 *
 * \param[in] client   The Wayland client requesting buffer creation.
 * \param[in] resource The params object resource containing plane configurations.
 * \param[in] id       Requested resource ID for the new buffer (0 indicates
 * implicit creation).
 * \param[in] width    Target width of the buffer in pixels.
 * \param[in] height   Target height of the buffer in pixels.
 * \param[in] format   The requested DRM fourcc format code.
 * \param[in] flags    Bitfield of buffer creation flags (currently unused).
 */
static void
create_immed(struct wl_client *client, struct wl_resource *resource, uint32_t id,
             int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	(void)flags;

	struct params *params = wl_resource_get_user_data(resource);
	if (!params) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
		                       "invalid params object");
		return;
	}

	if (params->created) {
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
		                       "buffer already created");
		return;
	}
	params->created = true;

	int num_planes = 0;

	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		num_planes = 1;
		break;
	default:
		wl_resource_post_error(resource,
		                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
		                       "unsupported format %#" PRIx32, format);
		return;
	}

	int i;

	/* Validate required planes are provided */
	for (i = 0; i < num_planes; ++i) {
		if (params->fd[i] == -1)
			wl_resource_post_error(resource,
			                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			                       "missing plane %d", i);
	}

	/* Ensure no unexpected extra planes were provided. */
	for (; i < MAX_PLANES; ++i) {
		if (params->fd[i] != -1)
			wl_resource_post_error(resource,
			                       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			                       "too many planes");
	}

	struct wl_resource *buffer_resource = NULL;
	union wld_object object;

	/* Import the buffer via wld using PRIME FD for plane 0 (single-plane for
	   now). */
	object.i = params->fd[0];
	struct wld_buffer *buffer =
	    wld_import_buffer(swc.drm->context, WLD_DRM_OBJECT_PRIME_FD, object,
	                      width, height, format, params->stride[0]);

	/* Close plane fds now that we've handed them to wld (or will error out). */
	for (i = 0; i < num_planes; ++i) {
		if (params->fd[i] == -1)
			continue;

		close(params->fd[i]);
		params->fd[i] = -1;
	}

	if (!buffer) {
		zwp_linux_buffer_params_v1_send_failed(resource);
		return;
	}

	buffer_resource = wayland_buffer_create_resource(client, 1, id, buffer);
	if (!buffer_resource) {
		wld_buffer_unreference(buffer);
		wl_resource_post_no_memory(resource);
		return;
	}

	/* If a zero id (create with implicit id) was requested, notify created. */
	if (id == 0)
		zwp_linux_buffer_params_v1_send_created(resource, buffer_resource);
}

/**
 * \brief Deferred buffer creation request (Legacy wrapper).
 *
 * Wraps `create_immed` to fulfill the standard `zwp_linux_buffer_params_v1.create`
 * request by passing an implicit resource ID of 0.
 *
 * \param[in] client   The Wayland client.
 * \param[in] resource The params object resource.
 * \param[in] width    Target width of the buffer.
 * \param[in] height   Target height of the buffer.
 * \param[in] format   DRM fourcc format code.
 * \param[in] flags    Buffer creation flags.
 */
static void
create(struct wl_client *client, struct wl_resource *resource,
       int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	create_immed(client, resource, 0, width, height, format, flags);
}

/** \brief Interface mapping for the zwp_linux_buffer_params_v1 protocol. */
static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = destroy_resource,
	.add = add,
	.create = create,
	.create_immed = create_immed,
};

/**
 * \brief Destructor for the params object.
 *
 * Invoked automatically when the Wayland resource is destroyed. It performs
 * cleanup by closing any dangling file descriptors that were not consumed
 * during buffer creation, and freeing the underlying memory.
 *
 * \param[in] resource The Wayland resource mapped to the params struct.
 */
static void
params_destroy(struct wl_resource *resource)
{
	struct params *params = wl_resource_get_user_data(resource);

	if (!params)
		return;

	for (int i = 0; i < MAX_PLANES; ++i) {
		if (params->fd[i] == -1)
			continue;

		close(params->fd[i]);
		params->fd[i] = -1;
	}
	free(params);
}

/**
 * \brief Initializes a new parameter negotiation object.
 *
 * Handles the `zwp_linux_dmabuf_v1.create_params` request by allocating and
 * zeroing out a new `struct params` context to stage a future DMA-BUF import.
 *
 * \param[in] client   The Wayland client issuing the request.
 * \param[in] resource The factory resource (zwp_linux_dmabuf_v1).
 * \param[in] id       The client-allocated ID for the new params object.
 *
 * \see https://wayland.app/protocols/linux-dmabuf-v1#zwp_linux_dmabuf_v1:request:create_params
 */
static void
create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct params *params = calloc(1, sizeof(*params));
	if (!params)
		goto error0;

	/* Mark all fds as unset and clear created flag*/
	params->created = false;
	for (int i = 0; i < MAX_PLANES; ++i)
		params->fd[i] = -1;

	const uint32_t version = wl_resource_get_version(resource);
	params->resource =
	    wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
	                       version, id);

	if (!params->resource)
		goto error1;

	wl_resource_set_implementation(params->resource, &params_impl, params, params_destroy);
	return;

error1:
	free(params);
error0:
	wl_resource_post_no_memory(resource);
}

/** \brief Interface mapping for the core zwp_linux_dmabuf_v1 protocol. */
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = destroy_resource,
	.create_params = create_params,
};

/**
 * \brief Global bind handler for zwp_linux_dmabuf_v1.
 *
 * Invoked when a client binds to the dmabuf global. This handler is responsible
 * for iterating and transmitting the supported DRM formats (and corresponding
 * modifiers, depending on the negotiated protocol version) back to the client.
 *
 * \param[in] client  The client establishing the bind.
 * \param[in] data    Contextual user data passed during global creation (unused).
 * \param[in] version The maximum protocol version mutually supported.
 * \param[in] id      The ID assigned to the new resource.
 */
static void
bind_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;

	static const uint32_t formats[] = {
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_ARGB8888,
	};
	const uint64_t modifier = DRM_FORMAT_MOD_INVALID;

	struct wl_resource *resource =
	    wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &dmabuf_impl, NULL, NULL);
	for (size_t i = 0; i < ARRAY_LENGTH(formats); ++i) {
		if (version >= 3) {
			/* TODO: query supported modifiers from wld/drm and advertise them */
			zwp_linux_dmabuf_v1_send_modifier(resource, formats[i],
			                                  (uint32_t)(modifier >> 32),
			                                  (uint32_t)(modifier & 0xffffffff));
		} else {
			zwp_linux_dmabuf_v1_send_format(resource, formats[i]);
		}
	}
}

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
struct wl_global *
swc_dmabuf_create(struct wl_display *display)
{
	/* Advertise version 3 (supports modifiers) */
	return wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 3, NULL, bind_dmabuf);
}
