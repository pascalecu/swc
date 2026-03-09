/**
 * \file compositor.c
 * \brief Core compositor implementation: rendering, view management, and updates
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * This file implements the compositor core used by swc.  It manages the set
 * of compositor views, calculates damage and clipping regions, schedules and
 * performs updates, handles rendering to screen targets, and wires compositor
 * surfaces into Wayland globals.  It also provides simple input focus logic
 * for pointer motion and a few built-in key bindings (terminate, switch VT).
 */

/*
 * Based in part upon compositor.c from weston, which is:
 *
 *     Copyright © 2010-2011 Intel Corporation
 *     Copyright © 2008-2011 Kristian Høgsberg
 *     Copyright © 2012 Collabora, Ltd.
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

#include "compositor.h"
#include "data_device_manager.h"
#include "drm.h"
#include "event.h"
#include "internal.h"
#include "launch.h"
#include "output.h"
#include "pointer.h"
#include "region.h"
#include "screen.h"
#include "seat.h"
#include "shm.h"
#include "surface.h"
#include "swc.h"
#include "util.h"
#include "view.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <wld/drm.h>
#include <wld/wld.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/**
 * \struct target
 * \brief Per-screen rendering target and its associated view state.
 *
 * A \c target holds the wld surface used for rendering to a particular
 * screen, the current and next buffers for page-flip handling, and a link
 * into the view handler system so the compositor can be notified of frame
 * completion for that screen.
 */
struct target {
	/**
	 * \brief The underlying \c wld surface for this target.
	 *
	 * This is the primary drawing surface where the compositor
	 * renders the final scene for this specific screen.
	 */
	struct wld_surface *surface;

	/**
	 * \brief The buffer currently being displayed on the screen.
	 *
	 * This buffer is owned by the hardware/display
	 * controller until a page flip completes.
	 */
	struct wld_buffer *current_buffer;

	/**
	 * \brief The buffer being prepared for the next frame.
	 *
	 * Once rendering is complete, this buffer is submitted to the DRM subsystem
	 * for the next page flip.
	 */
	struct wld_buffer *next_buffer;

	/**
	 * \brief The hardware plane or screen view associated with this target.
	 *  Usually points to the primary plane of a screen.
	 */
	struct view *view;

	/**
	 * \brief Handler for frame completion events.
	 *
	 * Linked into the view's handler list to receive notifications when a frame
	 * is finished (e.g., after a DRM page flip).
	 */
	struct view_handler view_handler;

	/**
	 * \brief Bitmask identifying the screen(s) this target represents.
	 *
	 * Used to match views to specific outputs during the compositor's
	 * visibility and damage calculations.
	 */
	uint32_t mask;

	/**
	 * \brief Cleanup listener for screen destruction.
	 *
	 * Ensures the target resources are freed if the associated screen is
	 * disconnected or removed.
	 */
	struct wl_listener screen_destroy_listener;
};

static bool handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void perform_update(void *data);

static struct pointer_handler pointer_handler = {
	.motion = handle_motion,
};

/**
 * \brief Global compositor state structure.
 */
static struct {
	/**
	 * \brief List of all compositor views
	 */
	struct wl_list views;

	/**
	 * \brief Accumulated opaque region
	 */
	pixman_region32_t opaque;

	/**
	 * \brief Accumulated damage region
	 */
	pixman_region32_t damage;

	/**
	 * \brief Listener for core swc events
	 */
	struct wl_listener swc_listener;

	/**
	 * \brief A mask of screens that have been repainted but are waiting on a page flip.
	 *
	 */
	uint32_t pending_flips;

	/**
	 * \brief A mask of screens that are scheduled to be repainted on the next idle.
	 */
	uint32_t scheduled_updates;

	/**
	 * \brief Flag indicating if an update is currently in progress
	 */

	bool updating;
	/**
	 * \brief The Wayland global interface for the compositor
	 */
	struct wl_global *global;
} compositor;

struct swc_compositor swc_compositor = {
	.pointer_handler = &pointer_handler,
};

/**
 * \brief Handle screen destruction by cleaning up its target.
 *
 * \param[in]     listener The listener embedded in the \c target.
 * \param[in,out] data     Unused auxiliary data.
 */
static void
handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct target *target = wl_container_of(listener, target, screen_destroy_listener);
	(void)data;

	wld_destroy_surface(target->surface);
	free(target);
}

/**
 * \brief Return the compositor \c target associated with a screen, or NULL.
 *
 * \param[in] screen The screen to look up.
 * \return A pointer to the \c target or NULL if none exists.
 */
static struct target *
target_get(struct screen *screen)
{
	struct wl_listener *listener;
	struct target *target;

	listener = wl_signal_get(&screen->destroy_signal, handle_screen_destroy);
	return listener ? wl_container_of(listener, target, screen_destroy_listener) : NULL;
}

/**
 * \brief Called when a screen's frame completes; perform finalization for that target.
 *
 * \param[in,out] handler View handler which embeds the target.
 * \param[in]     time    Frame timestamp in milliseconds.
 */
static void
handle_screen_frame(struct view_handler *handler, uint32_t time)
{
	struct target *target = wl_container_of(handler, target, view_handler);
	struct compositor_view *view;

	compositor.pending_flips &= ~target->mask;

	/* Notify all views visible on this screen that a frame has passed. */
	wl_list_for_each (view, &compositor.views, link) {
		if (view->visible && view->base.screens & target->mask)
			view_frame(&view->base, time);
	}

	/* Cycle buffers: release the previous front buffer and promote the back
	buffer. */
	if (target->current_buffer)
		wld_surface_release(target->surface, target->current_buffer);

	target->current_buffer = target->next_buffer;

	/* Trigger updates if they were deferred while waiting for this flip. */
	if (compositor.scheduled_updates && !compositor.updating)
		perform_update(NULL);
}

static const struct view_handler_impl screen_view_handler = {
	.frame = handle_screen_frame,
};

/**
 * \brief Swap buffers for a target (take the next buffer).
 *
 * \param[in,out] target The rendering target to swap buffers for.
 * \return 0 on success, or a negative error code (propagated to caller).
 */
static int
target_swap_buffers(struct target *target)
{
	struct wld_buffer *buffer;
	if (!(buffer = wld_surface_take(target->surface)))
		return -ENOMEM;

	target->next_buffer = buffer;
	return view_attach(target->view, target->next_buffer);
}

/**
 * \brief Create and initialize a new \c target for \p screen.
 *
 * \param[in,out] screen The screen to create a target for.
 * \return Pointer to the newly allocated \c target, or NULL on failure.
 */
static struct target *
target_new(struct screen *screen)
{
	struct target *target;
	struct swc_rectangle *geom = &screen->base.geometry;

	if (!(target = calloc(1, sizeof(*target))))
		return NULL;

	target->surface = wld_create_surface(swc.drm->context, geom->width, geom->height, WLD_FORMAT_XRGB8888, WLD_DRM_FLAG_SCANOUT);

	if (!target->surface) {
		free(target);
		return NULL;
	}

	target->view = &screen->planes.primary.view;
	target->view_handler.impl = &screen_view_handler;
	wl_list_insert(&target->view->handlers, &target->view_handler.link);

	target->mask = screen_mask(screen);
	target->screen_destroy_listener.notify = &handle_screen_destroy;
	wl_signal_add(&screen->destroy_signal, &target->screen_destroy_listener);

	return target;
}

/**
 * \brief Repaint a single compositor view onto a given target using \p damage.
 *
 * \param[in,out] target The rendering target.
 * \param[in]     view   The compositor view to paint.
 * \param[in]     damage The damage region (in global coordinates) to be painted.
 */
static void
repaint_view(struct target *target, struct compositor_view *view, pixman_region32_t *damage)
{
	pixman_region32_t view_damage, border_damage, view_rect_region;
	const struct swc_rectangle *geom = &view->base.geometry;
	const struct swc_rectangle *target_geom = &target->view->geometry;

	/* If there is no buffer or the view is fully clipped, there's nothing to do. */
	if (!view->base.buffer || pixman_region32_not_empty((pixman_region32_t *)&view->clip) == 0)
		return;

	/* Calculate the total area of the view (including borders) that needs
	 * repainting. This is: (view_extents ∩ global_damage) - view_clip */
	pixman_region32_init_with_extents(&view_damage, &view->extents);
	pixman_region32_intersect(&view_damage, &view_damage, damage);
	pixman_region32_subtract(&view_damage, &view_damage, (pixman_region32_t *)&view->clip);

	if (!pixman_region32_not_empty(&view_damage)) {
		pixman_region32_fini(&view_damage);
		return;
	}

	/* Prepare the view's content rectangle as a region. */
	pixman_region32_init_rect(&view_rect_region, geom->x, geom->y, geom->width, geom->height);

	/* Extract border damage: (total_view_damage - view_rect_region). */
	pixman_region32_init(&border_damage);
	pixman_region32_subtract(&border_damage, &view_damage, &view_rect_region);

	/* Extract content damage: (total_view_damage ∩ view_rect_region). */
	pixman_region32_intersect(&view_damage, &view_damage, &view_rect_region);

	/* Render content */
	if (pixman_region32_not_empty(&view_damage)) {
		pixman_region32_translate(&view_damage, -geom->x, -geom->y);
		wld_copy_region(swc.drm->renderer, view->buffer,
		                geom->x - target_geom->x,
		                geom->y - target_geom->y,
		                &view_damage);
	}

	/* Render border */
	if (pixman_region32_not_empty(&border_damage)) {
		pixman_region32_translate(&border_damage, -target_geom->x, -target_geom->y);
		wld_fill_region(swc.drm->renderer, view->border.color, &border_damage);
	}

	/* Cleanup */
	pixman_region32_fini(&view_rect_region);
	pixman_region32_fini(&view_damage);
	pixman_region32_fini(&border_damage);
}

/**
 * \brief Composite the list of views into the target surface.
 *
 * \param[in,out] target      Rendering target.
 * \param[in]     damage      Damage region in target-local coordinates.
 * \param[in]     base_damage Damage region representing background (opaque holes).
 * \param[in]     views       List of compositor views to paint.
 */
static void
renderer_repaint(struct target *target, pixman_region32_t *damage, pixman_region32_t *base_damage, struct wl_list *views)
{
	struct compositor_view *view;
	const struct swc_rectangle *geom = &target->view->geometry;

	DEBUG("Rendering to target { x: %d, y: %d, w: %u, h: %u }\n",
	      geom->x, geom->y, geom->width, geom->height);

	wld_set_target_surface(swc.drm->renderer, target->surface);

	/* Paint base damage (background) black for any "holes" in the view stack. */
	if (pixman_region32_not_empty(base_damage)) {
		pixman_region32_translate(base_damage, -geom->x, -geom->y);
		wld_fill_region(swc.drm->renderer, 0xff000000, base_damage);
	}

	/* Paint views back-to-front. */
	wl_list_for_each_reverse (view, views, link) {
		if (view->visible && view->base.screens & target->mask)
			repaint_view(target, view, damage);
	}

	wld_flush(swc.drm->renderer);
}

/**
 * \brief Attach a client buffer to a compositor view, creating proxy buffers as needed.
 *
 * \param[in,out] view          The compositor view to update.
 * \param[in]     client_buffer Client-provided buffer (may be NULL).
 * \return 0 on success or a negative errno on failure.
 */
static int
renderer_attach(struct compositor_view *view, struct wld_buffer *client_buffer)
{
	struct wld_buffer *old = view->buffer;
	struct wld_buffer *buffer = NULL;
	bool needs_proxy = false;
	bool resized = false;

	if (client_buffer) {
		const uint32_t caps = wld_capabilities(swc.drm->renderer, client_buffer);
		needs_proxy = !(caps & WLD_CAPABILITY_READ);

		if (old)
			resized = old->width != client_buffer->width || old->height != client_buffer->height;
	}

	if (!client_buffer) {
		buffer = NULL;
	} else if (!needs_proxy) {
		/* Renderer can read the client buffer directly. */
		buffer = client_buffer;
	} else if (old && old != view->base.buffer && !resized) {
		/* Reuse existing proxy */
		buffer = old;
	} else {
		DEBUG("Creating a proxy buffer\n");
		buffer = wld_create_buffer(swc.drm->context, client_buffer->width, client_buffer->height, client_buffer->format, WLD_FLAG_MAP);

		if (!buffer)
			return -ENOMEM;
	}

	/* Tear down the old proxy when:
	 *  - we had a proxy but no longer need one, or
	 *  - we need a proxy but the size changed (we created a new one).
	 */
	if (old && old != view->base.buffer && (!needs_proxy || resized))
		wld_buffer_unreference(old);

	view->buffer = buffer;

	return 0;
}

/**
 * \brief Flush any pending copy-to-SHM operations for the view.
 *
 * \param[in, out] view The view to flush.
 */
static void
renderer_flush_view(struct compositor_view *view)
{
	struct wld_buffer *proxy = view->buffer;

	/* Nothing to do if we are rendering directly to the client buffer. */
	if (!proxy || proxy == view->base.buffer)
		return;

	wld_set_target_buffer(swc.shm->renderer, proxy);
	wld_copy_region(swc.shm->renderer, view->base.buffer, 0, 0, &view->surface->state.damage);
	wld_flush(swc.shm->renderer);
}

/**
 * \brief Add the region below a view (excluding its clip) to compositor damage.
 *
 * \param[in,out] view The compositor view whose "below" region to damage.
 */
static void
damage_below_view(struct compositor_view *view)
{
	pixman_region32_t below;

	pixman_region32_init_with_extents(&below, &view->extents);
	pixman_region32_subtract(&below, &below, &view->clip);
	pixman_region32_union(&compositor.damage, &compositor.damage, &below);
	pixman_region32_fini(&below);
}

/**
 * \brief Mark the surface and its border completely damaged.
 *
 * \param[in,out] view The compositor view to damage.
 */
static void
damage_view(struct compositor_view *view)
{
	damage_below_view(view);
	view->border.damaged = true;
}

/**
 * \brief Recompute and mark extents for a view after position/size change.
 *
 * \param[in,out] view The compositor view to update.
 */
static void
update_extents(struct compositor_view *view)
{
	const struct swc_rectangle *g = &view->base.geometry;
	int bw = view->border.width;

	view->extents.x1 = g->x - bw;
	view->extents.y1 = g->y - bw;
	view->extents.x2 = g->x + g->width + bw;
	view->extents.y2 = g->y + g->height + bw;

	/* Damage border. */
	view->border.damaged = true;
}

/**
 * \brief Schedule compositor updates for the given screens.
 *
 * \param[in] screens Bitmask of screens to schedule, or ~0U to schedule all screens.
 */
static void
schedule_updates(uint32_t screens)
{
	/* If this is the first scheduled update, register the idle callback */
	if (compositor.scheduled_updates == 0)
		wl_event_loop_add_idle(swc.event_loop, &perform_update, NULL);

	/* If all screens are requested, compute the mask of all active screens */
	if (screens == ~0U) {
		uint32_t all_mask = 0;
		struct screen *screen;

		wl_list_for_each (screen, &swc.screens, link)
			all_mask |= screen_mask(screen);

		screens = all_mask;
	}

	/* Add requested screens to the scheduled set */
	compositor.scheduled_updates |= screens;
}

/**
 * \brief Notify that a view needs updating (schedules update if visible).
 *
 * \param[in,out] base The base view.
 * \return true if an update was scheduled, false otherwise.
 */
static bool
update(struct view *base)
{
	struct compositor_view *view = (struct compositor_view *)base;

	if (!swc.active || !view->visible)
		return false;

	schedule_updates(view->base.screens);
	return true;
}

/**
 * \brief Attach a buffer to a view (called when a client commits a buffer).
 *
 * \param[in,out] base   Base view pointer.
 * \param[in]     buffer Client buffer to attach.
 * \return 0 on success, negative errno on failure.
 */
static int
attach(struct view *base, struct wld_buffer *buffer)
{
	struct compositor_view *view = (struct compositor_view *)base;
	pixman_box32_t prev_extents;
	pixman_region32_t old_region, new_region, changed_region;
	int ret;

	/* Attach the client buffer (create proxy if needed) */
	ret = renderer_attach(view, buffer);
	if (ret < 0)
		return ret;

	/* Schedule updates for screens the view is currently on */
	update(&view->base);

	/* Update view size if the buffer changed dimensions */
	if (view_set_size_from_buffer(&view->base, buffer)) {
		prev_extents = view->extents;
		update_extents(view); /* Recompute extents including borders */

		if (view->visible) {
			/* Compute newly exposed or covered region (excluding clip) */
			pixman_region32_init_with_extents(&old_region, &prev_extents);
			pixman_region32_init_with_extents(&new_region, &view->extents);
			pixman_region32_init(&changed_region);

			pixman_region32_intersect(&changed_region, &old_region, &new_region);
			pixman_region32_union(&new_region, &old_region, &new_region);
			pixman_region32_subtract(&new_region, &new_region, &changed_region);
			pixman_region32_subtract(&new_region, &new_region, &view->clip);

			pixman_region32_union(&compositor.damage, &compositor.damage, &new_region);

			pixman_region32_fini(&old_region);
			pixman_region32_fini(&new_region);
			pixman_region32_fini(&changed_region);

			/* Update the screens this view covers */
			view_update_screens(&view->base);
			update(&view->base);
		}
	}

	return 0;
}

/**
 * \brief Move a view to a new position, updating damage and screens.
 *
 * \param[in,out] base Base view pointer.
 * \param[in]     x    New X coordinate.
 * \param[in]     y    New Y coordinate.
 * \return true always (handler indicates success).
 */
static bool
move(struct view *base, int32_t x, int32_t y)
{
	struct compositor_view *view = (struct compositor_view *)base;

	/* Damage old position if visible */
	if (view->visible) {
		damage_below_view(view);
		update(&view->base);
	}

	/* Update position; recompute extents if changed */
	if (!view_set_position(&view->base, x, y))
		return true;

	update_extents(view);

	if (!view->visible)
		return true;

	/* Reset clipping (worst-case until next frame) */
	pixman_region32_clear(&view->clip);

	/* Update affected screens and mark damage */
	view_update_screens(&view->base);
	damage_below_view(view);
	update(&view->base);

	return true;
}

static const struct view_impl view_impl = {
	.update = update,
	.attach = attach,
	.move = move,
};

/**
 * \brief Create a compositor_view for the given surface.
 *
 * \param[in,out] surface The surface to wrap.
 * \return Pointer to the newly created compositor_view, or NULL on allocation failure.
 */
struct compositor_view *
compositor_create_view(struct surface *surface)
{
	struct compositor_view *view;

	view = calloc(1, sizeof(*view));
	if (!view)
		return NULL;

	view_initialize(&view->base, &view_impl);

	view->surface = surface;
	surface_set_view(surface, &view->base);

	view->border.width = 0;
	view->border.color = 0x000000;
	view->border.damaged = false;

	view->visible = false;
	pixman_region32_init(&view->clip);

	wl_signal_init(&view->destroy_signal);

	wl_list_insert(&compositor.views, &view->link);

	return view;
}

/**
 * \brief Destroy a compositor view and free resources.
 *
 * \param[in,out] view The view to destroy.
 */
void
compositor_view_destroy(struct compositor_view *view)
{
	wl_signal_emit(&view->destroy_signal, NULL);
	compositor_view_hide(view);
	surface_set_view(view->surface, NULL);
	view_finalize(&view->base);
	pixman_region32_fini(&view->clip);
	wl_list_remove(&view->link);
	free(view);
}

/**
 * \brief Helper to cast a base view to a compositor_view when appropriate.
 *
 * \param[in] view Base view pointer.
 * \return Pointer to compositor_view or NULL if base is not a compositor_view.
 */
struct compositor_view *
compositor_view(struct view *view)
{
	if (!view)
		return NULL;

	return (view->impl == &view_impl) ? (struct compositor_view *)view : NULL;
}

/**
 * \brief Set the parent of a compositor view.
 *
 * \param[in,out] view   Compositor view whose parent will be set.
 * \param[in]     parent New parent view.
 */
void
compositor_view_set_parent(struct compositor_view *view, struct compositor_view *parent)
{
	view->parent = view;

	if (parent && parent->visible)
		compositor_view_show(view);
	else
		compositor_view_hide(view);
}

/**
 * \brief Show (map) a compositor view and recursively show children.
 *
 * \param[in,out] view The view to show.
 */
void
compositor_view_show(struct compositor_view *view)
{
	struct compositor_view *child;

	if (view->visible)
		return;

	view->visible = true;
	view_update_screens(&view->base);

	/* Clear clip region; worst-case no clipping until next frame */
	pixman_region32_clear(&view->clip);
	damage_view(view);
	update(&view->base);

	/* Recursively show child views */
	wl_list_for_each (child, &compositor.views, link) {
		if (child->parent == view)
			compositor_view_show(child);
	}
}

/**
 * \brief Hide (unmap) a compositor view and recursively hide children.
 *
 * \param[in,out] view The view to hide.
 */
void
compositor_view_hide(struct compositor_view *view)
{
	struct compositor_view *child;

	if (!view->visible)
		return;

	/* Update damage for currently visible screens */
	update(&view->base);
	damage_below_view(view);

	view_set_screens(&view->base, 0);
	view->visible = false;

	wl_list_for_each (child, &compositor.views, link) {
		if (child->parent == view)
			compositor_view_hide(child);
	}
}

/**
 * \brief Set a view's border width and mark for damage if changed.
 *
 * \param[in,out] view  The view to modify.
 * \param[in]     width New border width in pixels.
 */
void
compositor_view_set_border_width(struct compositor_view *view, uint32_t width)
{
	if (view->border.width == width)
		return;

	view->border.width = width;
	view->border.damaged = true;

	/* XXX: Damage above surface for transparent surfaces? */

	update_extents(view);
	update(&view->base);
}

/**
 * \brief Set a view's border color and mark for damage if changed.
 *
 * \param[in,out] view  The view to modify.
 * \param[in]     color Border color in ARGB.
 */
void
compositor_view_set_border_color(struct compositor_view *view, uint32_t color)
{
	if (view->border.color == color)
		return;

	view->border.color = color;
	view->border.damaged = true;

	/* XXX: Damage above surface for transparent surfaces? */

	update(&view->base);
}

/**
 * \brief Recalculate compositor damage and opaque regions from visible views.
 *
 * Walks the view stack top-down, computes per-view clip regions, accumulates
 * surface damage into the global compositor damage region, and handles border
 * damage.
 */
static void
calculate_damage(void)
{
	struct compositor_view *view;
	struct swc_rectangle *geom;
	pixman_region32_t surface_opaque;
	pixman_region32_t *surface_damage;

	pixman_region32_clear(&compositor.opaque);
	pixman_region32_init(&surface_opaque);

	/* Process all visible views in stacking order */
	wl_list_for_each (view, &compositor.views, link) {
		if (!view->visible)
			continue;

		geom = &view->base.geometry;

		/* Clip the view by the current opaque region */
		pixman_region32_copy(&view->clip, &compositor.opaque);

		/* Accumulate surface opaque region into global compositor opaque region */
		pixman_region32_copy(&surface_opaque, &view->surface->state.opaque);
		pixman_region32_translate(&surface_opaque, geom->x, geom->y);
		pixman_region32_union(&compositor.opaque, &compositor.opaque, &surface_opaque);

		surface_damage = &view->surface->state.damage;

		if (pixman_region32_not_empty(surface_damage)) {
			/* Flush pending rendering and translate damage to global coordinates */
			renderer_flush_view(view);
			pixman_region32_translate(surface_damage, geom->x, geom->y);
			pixman_region32_union(&compositor.damage, &compositor.damage, surface_damage);
			pixman_region32_clear(surface_damage);
		}

		if (view->border.damaged) {
			pixman_region32_t border_region, view_region;

			/* Damage the border area outside the surface */
			pixman_region32_init_with_extents(&border_region, &view->extents);
			pixman_region32_init_rect(&view_region, geom->x, geom->y, geom->width, geom->height);
			pixman_region32_subtract(&border_region, &border_region, &view_region);
			pixman_region32_union(&compositor.damage, &compositor.damage, &border_region);

			pixman_region32_fini(&border_region);
			pixman_region32_fini(&view_region);

			view->border.damaged = false;
		}
	}

	pixman_region32_fini(&surface_opaque);
}

/**
 * \brief Update a particular screen if it has scheduled updates.
 *
 * \param[in,out] screen The screen to repaint.
 */
static void
update_screen(struct screen *screen)
{
	if (!(compositor.scheduled_updates & screen_mask(screen)))
		return;

	struct target *target = target_get(screen);
	if (!target)
		return;

	const struct swc_rectangle *geom = &screen->base.geometry;

	/* Compute the intersection of compositor damage with this screen */
	pixman_region32_t screen_damage;
	pixman_region32_init(&screen_damage);
	pixman_region32_intersect_rect(&screen_damage, &compositor.damage,
	                               geom->x, geom->y, geom->width, geom->height);

	/* Translate damage to screen-local coordinates */
	pixman_region32_translate(&screen_damage, -geom->x, -geom->y);

	/* Query surface for total damage (e.g., including double-buffering) */
	pixman_region32_t *total_damage = wld_surface_damage(target->surface, &screen_damage);

	/* Skip repaint if the screen is waiting for a page flip */
	if (compositor.pending_flips & screen_mask(screen)) {
		pixman_region32_fini(&screen_damage);
		return;
	}

	/* Prepare the damage regions for rendering */
	pixman_region32_t damage, opaque_mask;
	pixman_region32_init(&damage);
	pixman_region32_init(&opaque_mask);

	pixman_region32_copy(&damage, total_damage);
	pixman_region32_translate(&damage, geom->x, geom->y);

	pixman_region32_subtract(&opaque_mask, &damage, &compositor.opaque);

	/* Repaint the screen */
	renderer_repaint(target, &damage, &opaque_mask, &compositor.views);

	pixman_region32_fini(&screen_damage);
	pixman_region32_fini(&damage);
	pixman_region32_fini(&opaque_mask);

	/* Swap buffers and handle special cases */
	switch (target_swap_buffers(target)) {
	case -EACCES:
		/* Session is being deactivated, but we haven't yet received the
		 * deactivate signal from swc-launch. */
		swc_deactivate();
		break;
	case 0:
		/* Mark screen as pending flip */
		compositor.pending_flips |= screen_mask(screen);
		break;
	}
}

/**
 * \brief Idle callback invoked to perform scheduled updates.
 *
 * \param data Unused.
 */
static void
perform_update(void *data)
{
	(void)data;

	if (!swc.active)
		return;

	/* Only update screens that are scheduled and not pending a flip */
	uint32_t updates = compositor.scheduled_updates & ~compositor.pending_flips;
	if (!updates)
		return;

	DEBUG("Performing update\n");

	compositor.updating = true;

	/* Recalculate global damage and opaque regions from visible views */
	calculate_damage();

	/* Update each screen */
	struct screen *screen;
	wl_list_for_each (screen, &swc.screens, link)
		update_screen(screen);

	/* XXX: Should assert that all damage was covered by some output */
	pixman_region32_clear(&compositor.damage);
	compositor.scheduled_updates &= ~updates;
	compositor.updating = false;
}

/**
 * \brief Handle pointer motion to update pointer focus.
 *
 * \param[in,out] handler Pointer handler (unused).
 * \param[in]     time    Event timestamp.
 * \param[in]     fx      Fixed-point X coordinate.
 * \param[in]     fy      Fixed-point Y coordinate.
 * \return false always (no input grab performed here).
 */
bool
handle_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx, wl_fixed_t fy)
{
	(void)handler;
	(void)time;

	/* If buttons are pressed, don't change pointer focus. */
	if (swc.seat->pointer->buttons.size > 0)
		return false;

	int32_t x = wl_fixed_to_int(fx);
	int32_t y = wl_fixed_to_int(fy);

	struct compositor_view *focus = NULL;

	/* Walk visible views top-down to find pointer target */
	struct compositor_view *view;
	wl_list_for_each (view, &compositor.views, link) {
		if (!view->visible)
			continue;

		struct swc_rectangle *geom = &view->base.geometry;
		if (!rectangle_contains_point(geom, x, y))
			continue;

		if (pixman_region32_contains_point(&view->surface->state.input,
		                                   x - geom->x, y - geom->y, NULL)) {
			focus = view;
			break;
		}
	}

	pointer_set_focus(swc.seat->pointer, focus);
	return false;
}

/**
 * \brief Handle terminate key binding.
 *
 * \param[in,out] data  User data (unused).
 * \param[in]     time  Event timestamp.
 * \param[in]     value Key value.
 * \param[in]     state Key state (pressed/released).
 */
static void
handle_terminate(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	(void)data;
	(void)time;
	(void)value;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	wl_display_terminate(swc.display);
}

/**
 * \brief Key binding: switch virtual terminal.
 *
 * \param[in,out] data  User data (unused).
 * \param[in]     time  Event timestamp.
 * \param[in]     value Key value (XF86Switch_VT_N).
 * \param[in]     state Key state (pressed/released).
 */
static void
handle_switch_vt(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	(void)data;
	(void)time;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	uint8_t vt = value - XKB_KEY_XF86Switch_VT_1 + 1;
	launch_activate_vt(vt);
}

/**
 * \brief Respond to high-level swc events (activation/deactivation).
 *
 * \param[in,out] listener Listener receiving the event.
 * \param[in]     data     The event data (struct event *).
 */
static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct event *event = data;

	switch (event->type) {
	case SWC_EVENT_ACTIVATED:
		schedule_updates(~0U); /* Schedule updates for all screens */
		break;
	case SWC_EVENT_DEACTIVATED:
		compositor.scheduled_updates = 0;
		break;
	default:
		break;
	}
}

/**
 * \brief wl_compositor.create_surface implementation.
 *
 * \param[in,out] client   The requesting client.
 * \param[in,out] resource The compositor resource.
 * \param[in]     id       New object id.
 */
static void
create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct surface *surface;

	/* Initialize surface. */
	surface = surface_new(client, wl_resource_get_version(resource), id);

	if (!surface) {
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_signal_emit(&swc_compositor.signal.new_surface, surface);
}

/**
 * \brief wl_compositor.create_region implementation.
 *
 * \param[in,out] client   The requesting client.
 * \param[in,out] resource The compositor resource.
 * \param[in]     id       New object id.
 */
static void
create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	if (!region_new(client, wl_resource_get_version(resource), id))
		wl_resource_post_no_memory(resource);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = create_surface,
	.create_region = create_region,
};

/**
 * \brief Bind handler for the wl_compositor global.
 *
 * \param[in,out] client  The client binding the global.
 * \param[in,out] data    User data (unused).
 * \param[in]     version Interface version requested.
 * \param[in]     id      New object id.
 */
static void
bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_compositor_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
}

/**
 * \brief Initialize the compositor subsystem, create targets and register globals.
 *
 * \return true on success, false on failure.
 */
bool
compositor_initialize(void)
{
	struct screen *screen;
	uint32_t keysym;

	compositor.global = wl_global_create(swc.display, &wl_compositor_interface, 4, NULL, &bind_compositor);

	if (!compositor.global)
		return false;

	/* Initialize compositor state */
	compositor.scheduled_updates = 0;
	compositor.pending_flips = 0;
	compositor.updating = false;
	wl_list_init(&compositor.views);

	pixman_region32_init(&compositor.damage);
	pixman_region32_init(&compositor.opaque);
	wl_signal_init(&swc_compositor.signal.new_surface);

	/* Listen for swc events */
	compositor.swc_listener.notify = &handle_swc_event;
	wl_signal_add(&swc.event_signal, &compositor.swc_listener);

	/* Initialize rendering targets for all screens */
	wl_list_for_each (screen, &swc.screens, link)
		target_new(screen);

	/* Schedule full updates if the compositor is active */
	if (swc.active)
		schedule_updates(~0U);

	/* Keybindings */
	swc_add_binding(SWC_BINDING_KEY, SWC_MOD_CTRL | SWC_MOD_ALT, XKB_KEY_BackSpace, &handle_terminate, NULL);

	for (keysym = XKB_KEY_XF86Switch_VT_1; keysym <= XKB_KEY_XF86Switch_VT_12; ++keysym)
		swc_add_binding(SWC_BINDING_KEY, SWC_MOD_ANY, keysym,
		                &handle_switch_vt, NULL);

	return true;
}

/**
 * \brief Finalize the compositor subsystem and release resources.
 */
void
compositor_finalize(void)
{
	pixman_region32_fini(&compositor.damage);
	pixman_region32_fini(&compositor.opaque);
	wl_global_destroy(compositor.global);
}