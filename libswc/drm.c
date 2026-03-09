/**
 * \file drm.c
 *
 * \brief Core implementation of the DRM/KMS backend.
 *
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * This module implements the Direct Rendering Manager (DRM) backend,
 * handling device discovery, CRTC/Connector mapping, and the legacy
 * wl_drm protocol. It also manages the lifecycle of hardware framebuffers
 * created from wld buffers.
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

#include "drm.h"
#include "dmabuf.h"
#include "event.h"
#include "internal.h"
#include "launch.h"
#include "output.h"
#include "plane.h"
#include "screen.h"
#include "util.h"
#include "wayland_buffer.h"

#include "wayland-drm-server-protocol.h"
#include <dirent.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/drm.h>
#include <wld/wld.h>
#include <xf86drm.h>

/** Global DRM state accessible across the compositor. */
struct swc_drm swc_drm;

/** Private module state for DRM globals and event management. */
static struct {
	char *path;                           /**< Path to the DRM render node. */
	struct wl_global *global;             /**< wl_drm global (legacy). */
	struct wl_global *dmabuf;             /**< linux_dmabuf global. */
	struct wl_event_source *event_source; /**< Event source for the DRM FD. */
} drm;

/**
 * \brief Handles wl_drm.authenticate requests.
 *
 * Authentication is treated as a no-op in this implementation, as we
 * enforce the use of PRIME file descriptors over legacy GEM names.
 *
 * \param client The Wayland client making the request.
 * \param resource The wl_drm resource.
 * \param magic The DRM magic token.
 */
static void
authenticate(struct wl_client *client, struct wl_resource *resource, uint32_t magic)
{
	(void)client;
	(void)magic;
	wl_drm_send_authenticated(resource);
}

/**
 * \brief Rejects legacy wl_drm.create_buffer requests.
 *
 * \note GEM names are deprecated. Clients must use PRIME file descriptors.
 */
static void
create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
              uint32_t name, int32_t width, int32_t height, uint32_t stride, uint32_t format)
{
	(void)client;
	(void)id;
	(void)name;
	(void)width;
	(void)height;
	(void)stride;
	(void)format;
	wl_resource_post_error(resource,
	                       WL_DRM_ERROR_INVALID_NAME,
	                       "GEM names are not supported, use a PRIME fd instead");
}

/**
 * \brief Rejects wl_drm.create_planar_buffer requests.
 *
 * \note Planar buffers are currently unsupported via the legacy wl_drm path.
 */
static void
create_planar_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                     uint32_t name, int32_t width, int32_t height, uint32_t format,
                     int32_t offset0, int32_t stride0,
                     int32_t offset1, int32_t stride1,
                     int32_t offset2, int32_t stride2)
{
	(void)client;
	(void)id;
	(void)name;
	(void)width;
	(void)height;
	(void)format;
	(void)offset0;
	(void)stride0;
	(void)offset1;
	(void)stride1;
	(void)offset2;
	(void)stride2;

	wl_resource_post_error(resource,
	                       WL_DRM_ERROR_INVALID_FORMAT,
	                       "planar buffers are not supported\n");
}

/**
 * \brief Creates a Wayland buffer resource from a PRIME file descriptor.
 *
 * Imports the provided PRIME FD into the compositor's DRM context.
 *
 * \param client The Wayland client making the request.
 * \param resource The wl_drm resource.
 * \param id The new ID for the buffer resource.
 * \param fd File descriptor of the PRIME buffer (takes ownership and closes).
 * \param width Width of the buffer.
 * \param height Height of the buffer.
 * \param format DRM format code.
 * \param offset0 Offset for the first plane.
 * \param stride0 Stride for the first plane.
 */
static void
create_prime_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                    int32_t fd, int32_t width, int32_t height, uint32_t format,
                    int32_t offset0, int32_t stride0,
                    int32_t offset1, int32_t stride1,
                    int32_t offset2, int32_t stride2)
{
	(void)offset0;
	(void)offset1;
	(void)stride1;
	(void)offset2;
	(void)stride2;

	union wld_object object = { .i = fd };
	struct wld_buffer *buffer =
	    wld_import_buffer(swc.drm->context,
	                      WLD_DRM_OBJECT_PRIME_FD,
	                      object, width, height, format, stride0);
	close(fd);

	if (!buffer) {
		wl_resource_post_no_memory(resource);
		return;
	}

	struct wl_resource *buffer_resource =
	    wayland_buffer_create_resource(client, wl_resource_get_version(resource),
	                                   id, buffer);

	if (!buffer_resource) {
		wld_buffer_unreference(buffer);
		wl_resource_post_no_memory(resource);
	}
}

/** Implementation structure for the legacy wl_drm Wayland interface. */
static const struct wl_drm_interface drm_impl = {
	.authenticate = authenticate,
	.create_buffer = create_buffer,
	.create_planar_buffer = create_planar_buffer,
	.create_prime_buffer = create_prime_buffer,
};

/** \brief Filter function for scandir to find DRM card nodes (e.g., card0, card1). */
static int
select_card(const struct dirent *entry)
{
	unsigned num;
	return sscanf(entry->d_name, "card%u", &num) == 1;
}

/**
 * \brief Scans for the primary DRM device.
 *
 * Prefers the device marked as `boot_vga` in sysfs. If no boot VGA device
 * is identified, it falls back to the first discovered DRM card.
 *
 * \param[out] path Buffer to store the resulting device path.
 * \param[in] size Maximum size of the path buffer.
 * \return true if a suitable device was found, false otherwise.
 */
static bool
find_primary_drm_device(char *path, size_t size)
{
	struct dirent **cards;
	int num_cards;
	bool found = false;

	num_cards = scandir("/dev/dri", &cards, &select_card, &alphasort);
	if (num_cards == -1)
		return false;

	for (int i = 0; i < num_cards; ++i) {
		char sys_path[PATH_MAX];
		snprintf(sys_path, sizeof(sys_path), "/sys/class/drm/%s/device/boot_vga", cards[i]->d_name);

		FILE *file = fopen(sys_path, "r");
		if (file) {
			unsigned char boot_vga;
			if (fscanf(file, "%hhu", &boot_vga) == 1 && boot_vga) {
				snprintf(path, size, "/dev/dri/%s", cards[i]->d_name);
				found = true;
			}
			fclose(file);
		}

		/* Fallback: use the first card found if no boot_vga is identified yet */
		if (!found && i == 0) {
			snprintf(path, size, "/dev/dri/%s", cards[i]->d_name);
		}
		free(cards[i]);
	}

	free(cards);
	return found || path[0] != '\0';
}

/**
 * \brief Searches for an unassigned CRTC compatible with a specific connector.
 *
 * \param[in] resources The DRM resources block for the device.
 * \param[in] connector The target connector needing a CRTC.
 * \param[in] taken_crtcs Bitmask representing CRTC indices already in use.
 * \param[out] crtc_index Pointer to store the found CRTC index.
 * \return true if a compatible, available CRTC was found, false otherwise.
 */
static bool
find_available_crtc(drmModeRes *resources, drmModeConnector *connector, uint32_t taken_crtcs, int *crtc_index)
{
	const int encoder_count = connector->count_encoders;

	for (int i = 0; i < encoder_count; ++i) {
		drmModeEncoder *encoder = drmModeGetEncoder(swc.drm->fd, connector->encoders[i]);
		if (!encoder)
			continue;

		uint32_t possible_crtcs = encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);

		for (int j = 0, mask = 1 << j; j < resources->count_crtcs; ++j) {
			if ((possible_crtcs & mask) && !(taken_crtcs & mask)) {
				*crtc_index = j;
				return true;
			}
		}
	}
	return false;
}

/** \brief No-op vblank handler. */
static void
handle_vblank(int fd, unsigned int sequence, unsigned int sec, unsigned int usec, void *data)
{
	(void)fd;
	(void)sequence;
	(void)sec;
	(void)usec;
	(void)data;
}

/** \brief Routes hardware page flip events back to the responsible DRM handler. */
static void
handle_page_flip(int fd, unsigned int sequence, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	(void)fd;
	(void)sequence;
	(void)crtc_id;
	struct drm_handler *handler = data;
	handler->page_flip(handler, sec * 1000 + usec / 1000);
}

/** DRM event context specifying callback handlers. */
static drmEventContext event_context = {
	.version = DRM_EVENT_CONTEXT_VERSION,
	.vblank_handler = handle_vblank,
	.page_flip_handler2 = handle_page_flip,
};

/** \brief Dispatches DRM events when the file descriptor is readable. */
static int
handle_data(int fd, uint32_t mask, void *data)
{
	(void)mask;
	(void)data;
	drmHandleEvent(fd, &event_context);
	return 1;
}

/** \brief Binds a client to the wl_drm interface and sends capabilities. */
static void
bind_drm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource =
	    wl_resource_create(client, &wl_drm_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &drm_impl, NULL, NULL);

	if (version >= 2)
		wl_drm_send_capabilities(resource, WL_DRM_CAPABILITY_PRIME);

	wl_drm_send_device(resource, drm.path);
	wl_drm_send_format(resource, WL_DRM_FORMAT_XRGB8888);
	wl_drm_send_format(resource, WL_DRM_FORMAT_ARGB8888);
}

/**
 * \brief Initializes the DRM subsystem.
 *
 * Locates a suitable DRM device, opens the file descriptor, and initializes
 * the wld rendering context.
 *
 * \return true if initialization was successful, false otherwise.
 */
bool
drm_initialize(void)
{
	uint64_t val;
	char primary[PATH_MAX];

	if (!find_primary_drm_device(primary, sizeof(primary))) {
		ERROR("Could not find DRM device\n");
		goto error0;
	}

	swc.drm->fd = launch_open_device(primary, O_RDWR | O_CLOEXEC);
	if (swc.drm->fd == -1) {
		ERROR("Could not open DRM device at %s\n", primary);
		goto error0;
	}
	if (drmSetClientCap(swc.drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
		ERROR("Could not enable DRM universal planes\n");
		goto error1;
	}
	if (drmGetCap(swc.drm->fd, DRM_CAP_CURSOR_WIDTH, &val) < 0)
		val = 64;
	swc.drm->cursor_w = val;
	if (drmGetCap(swc.drm->fd, DRM_CAP_CURSOR_HEIGHT, &val) < 0)
		val = 64;
	swc.drm->cursor_h = val;

	drm.path = drmGetRenderDeviceNameFromFd(swc.drm->fd);
	if (!drm.path) {
		ERROR("Could not determine render node path\n");
		goto error1;
	}

	if (!(swc.drm->context = wld_drm_create_context(swc.drm->fd))) {
		ERROR("Could not create WLD DRM context\n");
		goto error1;
	}

	if (!(swc.drm->renderer = wld_create_renderer(swc.drm->context))) {
		ERROR("Could not create WLD DRM renderer\n");
		goto error2;
	}

	drm.event_source = wl_event_loop_add_fd(swc.event_loop, swc.drm->fd, WL_EVENT_READABLE, &handle_data, NULL);

	if (!drm.event_source) {
		ERROR("Could not create DRM event source\n");
		goto error3;
	}

	if (!wld_drm_is_dumb(swc.drm->context)) {
		drm.global = wl_global_create(swc.display, &wl_drm_interface, 2, NULL, &bind_drm);
		if (!drm.global) {
			ERROR("Could not create wl_drm global\n");
			goto error4;
		}

		drm.dmabuf = swc_dmabuf_create(swc.display);
		if (!drm.dmabuf) {
			WARNING("Could not create wp_linux_dmabuf global\n");
		}
	}

	return true;

error4:
	wl_event_source_remove(drm.event_source);
error3:
	wld_destroy_renderer(swc.drm->renderer);
error2:
	wld_destroy_context(swc.drm->context);
error1:
	close(swc.drm->fd);
error0:
	return false;
}

/**
 * \brief Tears down the DRM subsystem.
 *
 * Releases all hardware resources, closes file descriptors, and destroys
 * the wld context.
 */
void
drm_finalize(void)
{
	if (drm.global)
		wl_global_destroy(drm.global);
	if (drm.dmabuf)
		wl_global_destroy(drm.dmabuf);
	wl_event_source_remove(drm.event_source);
	wld_destroy_renderer(swc.drm->renderer);
	wld_destroy_context(swc.drm->context);
	free(drm.path);
	close(swc.drm->fd);
}

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
bool
drm_create_screens(struct wl_list *screens)
{
	drmModePlaneRes *plane_ids;
	drmModeRes *resources;
	drmModeConnector *connector;
	struct plane *plane, *cursor_plane;
	struct output *output;
	uint32_t i, taken_crtcs = 0;
	struct wl_list planes;

	plane_ids = drmModeGetPlaneResources(swc.drm->fd);
	if (!plane_ids) {
		ERROR("Could not get DRM plane resources\n");
		return false;
	}
	wl_list_init(&planes);
	for (i = 0; i < plane_ids->count_planes; ++i) {
		plane = plane_new(plane_ids->planes[i]);
		if (plane)
			wl_list_insert(&planes, &plane->link);
	}
	drmModeFreePlaneResources(plane_ids);

	resources = drmModeGetResources(swc.drm->fd);
	if (!resources) {
		ERROR("Could not get DRM resources\n");
		return false;
	}
	for (i = 0; i < resources->count_connectors; ++i, drmModeFreeConnector(connector)) {
		connector = drmModeGetConnector(swc.drm->fd, resources->connectors[i]);

		if (connector->connection == DRM_MODE_CONNECTED) {
			int crtc_index;

			if (!find_available_crtc(resources, connector, taken_crtcs, &crtc_index)) {
				WARNING("Could not find CRTC for connector %d\n", i);
				continue;
			}

			cursor_plane = NULL;
			wl_list_for_each (plane, &planes, link) {
				if (plane->type == DRM_PLANE_TYPE_CURSOR && plane->possible_crtcs & 1 << crtc_index) {
					wl_list_remove(&plane->link);
					cursor_plane = plane;
					break;
				}
			}
			if (!cursor_plane) {
				WARNING("Could not find cursor plane for CRTC %d\n", crtc_index);
			}

			if (!(output = output_new(connector)))
				continue;

			output->screen = screen_new(resources->crtcs[crtc_index], output, cursor_plane);
			output->screen->id = crtc_index;
			taken_crtcs |= 1 << crtc_index;

			wl_list_insert(screens, &output->screen->link);
		}
	}
	drmModeFreeResources(resources);

	return true;
}

enum {
	WLD_USER_OBJECT_FRAMEBUFFER = WLD_USER_ID
};

/** \brief Internal structure for tracking a DRM FB associated with a buffer. */
struct framebuffer {
	struct wld_exporter exporter;
	struct wld_destructor destructor;
	uint32_t id;
};

/** \brief Custom wld exporter for returning the cached DRM framebuffer ID. */
static bool
framebuffer_export(struct wld_exporter *exporter, struct wld_buffer *buffer, uint32_t type, union wld_object *object)
{
	(void)buffer;

	if (type != WLD_USER_OBJECT_FRAMEBUFFER)
		return false;

	struct framebuffer *fb = wl_container_of(exporter, fb, exporter);
	object->u32 = fb->id;
	return true;
}

/** \brief Destroys the hardware framebuffer when the wld buffer is freed. */
static void
framebuffer_destroy(struct wld_destructor *destructor)
{
	struct framebuffer *fb = wl_container_of(destructor, fb, destructor);
	drmModeRmFB(swc.drm->fd, fb->id);
	free(fb);
}

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
uint32_t
drm_get_framebuffer(struct wld_buffer *buffer)
{
	if (!buffer)
		return 0;

	union wld_object obj;

	/* Return cached framebuffer if it exists */
	if (wld_export(buffer, WLD_USER_OBJECT_FRAMEBUFFER, &obj))
		return obj.u32;

	/* Get hardware handle for the buffer */
	if (!wld_export(buffer, WLD_DRM_OBJECT_HANDLE, &obj)) {
		ERROR("Could not get buffer handle\n");
		return 0;
	}

	struct framebuffer *fb = malloc(sizeof(*fb));
	if (!fb)
		return 0;

	uint32_t handles[4] = { obj.u32 }, pitches[4] = { buffer->pitch }, offsets[4] = { 0 };
	int ret = drmModeAddFB2(swc.drm->fd, buffer->width, buffer->height, buffer->format,
	                        handles, pitches, offsets, &fb->id, 0);
	if (ret < 0) {
		free(fb);
		return 0;
	}

	/* Attach exporter and destructor to the buffer to cache the fb id */
	fb->exporter.export = framebuffer_export;
	wld_buffer_add_exporter(buffer, &fb->exporter);
	fb->destructor.destroy = framebuffer_destroy;
	wld_buffer_add_destructor(buffer, &fb->destructor);

	return fb->id;
}
