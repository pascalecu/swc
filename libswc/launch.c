/**
 * \file launch.c
 * \brief Implementation of the launch helper IPC used by the compositor.
 *
 * \author Michael Forney
 * \date 2013--2014
 * \copyright MIT
 *
 * The compositor communicates with an external helper over a pre-opened socket
 * whose file descriptor is provided via the environment variable
 * `SWC_LAUNCH_SOCKET_ENV`. This helper provides privileged operations such as
 * opening device nodes and activating virtual terminals; responses and events
 * are exchanged using a small RPC/event protocol defined in launch/protocol.h.
 *
 * The implementation below registers the socket with the compositor's Wayland
 * event loop and provides synchronous helpers that send a request and wait for
 * the corresponding response while handling any asynchronous events the helper
 * might emit.
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

#include "launch.h"
#include "event.h"
#include "internal.h"
#include "launch/protocol.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wayland-server.h>

static struct {
	int socket;                     /**< launch socket fd, -1 if unused */
	struct wl_event_source *source; /**< Wayland event source for the socket */
	uint32_t next_serial;           /**< serial counter for requests */
} launch = { .socket = -1, .source = NULL, .next_serial = 0 };

/**
 * \brief Handle asynchronous events emitted by the helper.
 *
 * Processes event types defined by the protocol; known asynchronous events
 * (activate/deactivate) are forwarded to compositor helpers. Unknown events are
 * ignored and false is returned.
 *
 * \param event Pointer to the received event.
 * \return true if the event was handled, false otherwise.
 */
static bool
handle_event(struct swc_launch_event *event)
{
	switch (event->type) {
	case SWC_LAUNCH_EVENT_ACTIVATE:
		swc_activate();
		return true;
	case SWC_LAUNCH_EVENT_DEACTIVATE:
		swc_deactivate();
		return true;
	default:
		return false;
	}
}

/**
 * \brief Wayland event-loop callback for incoming data on the launch socket.
 *
 * Reads a single event message (and any passed file descriptors) and dispatches
 * it via handle_event(). The callback returns 1 to keep the source active.
 *
 * \return 1 to keep the wl_event_source registered, or 0 to remove it.
 */
static int
handle_data(int fd, uint32_t mask, void *data)
{
	(void)mask;
	(void)data;

	struct swc_launch_event event;
	struct iovec iov = {
		.iov_base = &event,
		.iov_len = sizeof(event),
	};

	int ret = receive_fd(fd, NULL, &iov, 1);

	if (ret <= 0) {
		/* Ignore transient errors */
		if (ret < 0 && errno == EINTR)
			return 1;

		/* Socket closed or fatal error */
		return 0;
	}

	handle_event(&event);
	return 1;
}

static uint32_t
next_serial(void)
{
	if (++launch.next_serial == 0)
		launch.next_serial = 1;

	return launch.next_serial;
}

/**
 * \brief Send a request and wait for its response while processing other events.
 *
 * This function performs a synchronous request/response exchange with the
 * helper. While waiting it will continue to read and dispatch asynchronous
 * events (e.g. ACTIVATE/DEACTIVATE) until it receives the response whose serial
 * matches the request.
 *
 * \param request Pointer to the request to send (type and payload fields).
 * \param data Optional pointer to contiguous additional payload data (e.g. a
 * NUL-terminated path). If size > 0, data must be non-NULL.
 * \param size Size of \p data in bytes.
 * \param event Output pointer to receive the response event structure.
 * \param out_fd File descriptor to send with the request, or -1 if none.
 * \param in_fd Pointer to an int to receive an fd from the helper, or NULL.
 * \return true on success (matching response received), false on error.
 */
static bool
send_request(struct swc_launch_request *request, const void *data, size_t size, struct swc_launch_event *event, int out_fd, int *in_fd)
{
	if (launch.socket < 0)
		return false;

	if (size > 0 && data == NULL)
		return false;

	struct iovec request_iov[2];
	int iov_count = 1;

	request->serial = next_serial();

	request_iov[0].iov_base = request;
	request_iov[0].iov_len = sizeof(*request);

	if (size) {
		request_iov[1].iov_base = (void *)data;
		request_iov[1].iov_len = size;
		iov_count = 2;
	}

	if (send_fd(launch.socket, out_fd, request_iov, iov_count) < 0)
		return false;

	struct iovec response_iov = {
		.iov_base = event,
		.iov_len = sizeof(*event),
	};

	for (;;) {
		int ret = receive_fd(launch.socket, in_fd, &response_iov, 1);

		if (ret <= 0) {
			if (ret < 0 && errno == EINTR)
				continue;
			return false;
		}

		if (event->type == SWC_LAUNCH_EVENT_RESPONSE && event->serial == request->serial)
			return true;

		handle_event(event);
	}

	return false;
}

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
bool
launch_initialize(void)
{
	if (launch.socket >= 0)
		return true;

	char *socket_string = getenv(SWC_LAUNCH_SOCKET_ENV);
	if (!socket_string)
		return false;

	char *end;
	errno = 0;

	long fd_val = strtol(socket_string, &end, 10);
	if (errno || *end != '\0' || fd_val < 0 || fd_val > INT_MAX)
		return false;

	launch.socket = (int)fd_val;
	unsetenv(SWC_LAUNCH_SOCKET_ENV);

	int flags = fcntl(launch.socket, F_GETFD);
	if (flags < 0)
		goto fail;

	if (fcntl(launch.socket, F_SETFD, flags | FD_CLOEXEC) < 0)
		goto fail;

	launch.source = wl_event_loop_add_fd(
	    swc.event_loop,
	    launch.socket,
	    WL_EVENT_READABLE,
	    handle_data,
	    NULL);

	if (!launch.source)
		goto fail;

	return true;

fail:
	close(launch.socket);
	launch.socket = -1;
	return false;
}

/**
 * \brief Shut down the launch helper interface.
 *
 * Removes the event loop source and closes the launch socket.
 */
void
launch_finalize(void)
{
	if (launch.source) {
		wl_event_source_remove(launch.source);
		launch.source = NULL;
	}

	if (launch.socket >= 0) {
		close(launch.socket);
		launch.socket = -1;
	}
}

/**
 * \brief Request that the helper open a device node.
 *
 * \param path Path of the device to open.
 * \param flags Flags passed to open(2).
 *
 * \return A file descriptor on success, or -1 on failure.
 *
 * The descriptor is created by the helper and transferred over the launch
 * socket. The caller is responsible for closing it.
 */
int
launch_open_device(const char *path, int flags)
{
	if (!path)
		return -1;

	size_t len = strlen(path) + 1;

	struct swc_launch_request request = {
		.type = SWC_LAUNCH_REQUEST_OPEN_DEVICE,
		.flags = flags,
	};

	struct swc_launch_event response;
	int fd = -1;
	if (!send_request(&request, path, len, &response, -1, &fd))
		return -1;

	return fd;
}

/**
 * \brief Request activation of a virtual terminal.
 *
 * \param vt Virtual terminal number.
 *
 * \return true if the helper successfully activated the VT, false otherwise.
 *
 * This call blocks until the helper replies.
 */
bool
launch_activate_vt(unsigned vt)
{
	struct swc_launch_request request = {
		.type = SWC_LAUNCH_REQUEST_ACTIVATE_VT,
		.vt = vt,
	};

	struct swc_launch_event response;

	if (!send_request(&request, NULL, 0, &response, -1, NULL))
		return false;

	return response.success;
}
