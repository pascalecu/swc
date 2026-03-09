/**
 * \file compositor.c
 * \brief Core compositor implementation: rendering, view management, and updates
 * \author Michael Forney
 * \date 2013--2020
 * \copyright MIT
 *
 * \details
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
 * \details
 * A \c target holds the wld surface used for rendering to a particular
 * screen, the current and next buffers for page-flip handling, and a link
 * into the view handler system so the compositor can be notified of frame
 * completion for that screen.
 */
struct target {
	/**
	 * \brief The underlying \c wld surface for this target.
	 * \details
	 * This is the primary drawing surface where the compositor
	 * renders the final scene for this specific screen.
	 */
	struct wld_surface *surface;

	/**
	 * \brief The buffer currently being displayed on the screen.
	 * \details
	 * This buffer is owned by the hardware/display
	 * controller until a page flip completes.
	 */
	struct wld_buffer *current_buffer;

	/**
	 * \brief The buffer being prepared for the next frame.
	 * \details
	 * Once rendering is complete, this buffer is submitted to the DRM subsystem
	 * for the next page flip.
	 */
	struct wld_buffer *next_buffer;

	/**
	 * \brief The hardware plane or screen view associated with this target.
	 * \details Usually points to the primary plane of a screen.
	 */
	struct view *view;

	/**
	 * \brief Handler for frame completion events.
	 * \details
	 * Linked into the view's handler list to receive notifications when a frame
	 * is finished (e.g., after a DRM page flip).
	 */
	struct view_handler view_handler;

	/**
	 * \brief Bitmask identifying the screen(s) this target represents.
	 * \details
	 * Used to match views to specific outputs during the compositor's
	 * visibility and damage calculations.
	 */
	uint32_t mask;

	/**
	 * \brief Cleanup listener for screen destruction.
	 * \details
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
	(void)data;
	struct target *target = wl_container_of(listener, target, screen_destroy_listener);

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
	struct wl_listener *listener = wl_signal_get(&screen->destroy_signal, &handle_screen_destroy);
	struct target *target;

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

	wl_list_for_each (view, &compositor.views, link) {
		if (view->visible && view->base.screens & target->mask)
			view_frame(&view->base, time);
	}

	if (target->current_buffer)
		wld_surface_release(target->surface, target->current_buffer);

	target->current_buffer = target->next_buffer;

	/* If we had scheduled updates that couldn't run because we were waiting on a
	 * page flip, run them now. If the compositor is currently updating, then the
	 * frame finished immediately, and we can be sure that there are no pending
	 * updates. */
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
	target->next_buffer = wld_surface_take(target->surface);
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

	if (!(target = malloc(sizeof(*target))))
		goto error0;

	target->surface = wld_create_surface(swc.drm->context, geom->width, geom->height, WLD_FORMAT_XRGB8888, WLD_DRM_FLAG_SCANOUT);

	if (!target->surface)
		goto error1;

	target->view = &screen->planes.primary.view;
	target->view_handler.impl = &screen_view_handler;
	wl_list_insert(&target->view->handlers, &target->view_handler.link);
	target->current_buffer = NULL;
	target->mask = screen_mask(screen);

	target->screen_destroy_listener.notify = &handle_screen_destroy;
	wl_signal_add(&screen->destroy_signal, &target->screen_destroy_listener);

	return target;

error1:
	free(target);
error0:
	return NULL;
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
	pixman_region32_t view_region, view_damage, border_damage;
	const struct swc_rectangle *geom = &view->base.geometry, *target_geom = &target->view->geometry;

	if (!view->base.buffer)
		return;

	pixman_region32_init_rect(&view_region, geom->x, geom->y, geom->width, geom->height);
	pixman_region32_init_with_extents(&view_damage, &view->extents);
	pixman_region32_init(&border_damage);

	pixman_region32_intersect(&view_damage, &view_damage, damage);
	pixman_region32_subtract(&view_damage, &view_damage, &view->clip);
	pixman_region32_subtract(&border_damage, &view_damage, &view_region);
	pixman_region32_intersect(&view_damage, &view_damage, &view_region);

	pixman_region32_fini(&view_region);

	if (pixman_region32_not_empty(&view_damage)) {
		pixman_region32_translate(&view_damage, -geom->x, -geom->y);
		wld_copy_region(swc.drm->renderer, view->buffer, geom->x - target_geom->x, geom->y - target_geom->y, &view_damage);
	}

	pixman_region32_fini(&view_damage);

	/* Draw border */
	if (pixman_region32_not_empty(&border_damage)) {
		pixman_region32_translate(&border_damage, -target_geom->x, -target_geom->y);
		wld_fill_region(swc.drm->renderer, view->border.color, &border_damage);
	}

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

	DEBUG("Rendering to target { x: %d, y: %d, w: %u, h: %u }\n",
	      target->view->geometry.x, target->view->geometry.y,
	      target->view->geometry.width, target->view->geometry.height);

	wld_set_target_surface(swc.drm->renderer, target->surface);

	/* Paint base damage black. */
	if (pixman_region32_not_empty(base_damage)) {
		pixman_region32_translate(base_damage, -target->view->geometry.x, -target->view->geometry.y);
		wld_fill_region(swc.drm->renderer, 0xff000000, base_damage);
	}

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
	struct wld_buffer *buffer;
	bool was_proxy = view->buffer != view->base.buffer;
	bool needs_proxy = client_buffer && !(wld_capabilities(swc.drm->renderer, client_buffer) & WLD_CAPABILITY_READ);
	bool resized = view->buffer && client_buffer && (view->buffer->width != client_buffer->width || view->buffer->height != client_buffer->height);

	if (client_buffer) {
		/* Create a proxy buffer if necessary (for example a hardware buffer backing
		 * a SHM buffer). */
		if (needs_proxy) {
			if (!was_proxy || resized) {
				DEBUG("Creating a proxy buffer\n");
				buffer = wld_create_buffer(swc.drm->context, client_buffer->width, client_buffer->height, client_buffer->format, WLD_FLAG_MAP);

				if (!buffer)
					return -ENOMEM;
			} else {
				/* Otherwise we can keep the original proxy buffer. */
				buffer = view->buffer;
			}
		} else {
			buffer = client_buffer;
		}
	} else {
		buffer = NULL;
	}

	/* If we no longer need a proxy buffer, or the original buffer is of a
	 * different size, destroy the old proxy image. */
	if (view->buffer && ((!needs_proxy && was_proxy) || (needs_proxy && resized)))
		wld_buffer_unreference(view->buffer);

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
	if (view->buffer == view->base.buffer)
		return;

	wld_set_target_buffer(swc.shm->renderer, view->buffer);
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
	pixman_region32_t damage_below;

	pixman_region32_init_with_extents(&damage_below, &view->extents);
	pixman_region32_subtract(&damage_below, &damage_below, &view->clip);
	pixman_region32_union(&compositor.damage, &compositor.damage, &damage_below);
	pixman_region32_fini(&damage_below);
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
	view->extents.x1 = view->base.geometry.x - view->border.width;
	view->extents.y1 = view->base.geometry.y - view->border.width;
	view->extents.x2 = view->base.geometry.x + view->base.geometry.width + view->border.width;
	view->extents.y2 = view->base.geometry.y + view->base.geometry.height + view->border.width;

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
	if (compositor.scheduled_updates == 0)
		wl_event_loop_add_idle(swc.event_loop, &perform_update, NULL);

	if (screens == ~0U) {
		struct screen *screen;

		screens = 0;
		wl_list_for_each (screen, &swc.screens, link)
			screens |= screen_mask(screen);
	}

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
	struct compositor_view *view = (void *)base;

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
	struct compositor_view *view = (void *)base;
	pixman_box32_t old_extents;
	pixman_region32_t old, new, both;
	int ret;

	if ((ret = renderer_attach(view, buffer)) < 0)
		return ret;

	/* Schedule updates on the screens the view was previously
	 * visible on. */
	update(&view->base);

	if (view_set_size_from_buffer(&view->base, buffer)) {
		/* The view was resized. */
		old_extents = view->extents;
		update_extents(view);

		if (view->visible) {
			/* Damage the region that was newly uncovered
			 * or covered, minus the clip region. */
			pixman_region32_init_with_extents(&old, &old_extents);
			pixman_region32_init_with_extents(&new, &view->extents);
			pixman_region32_init(&both);
			pixman_region32_intersect(&both, &old, &new);
			pixman_region32_union(&new, &old, &new);
			pixman_region32_subtract(&new, &new, &both);
			pixman_region32_subtract(&new, &new, &view->clip);
			pixman_region32_union(&compositor.damage, &compositor.damage, &new);
			pixman_region32_fini(&old);
			pixman_region32_fini(&new);
			pixman_region32_fini(&both);

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
	struct compositor_view *view = (void *)base;

	if (view->visible) {
		damage_below_view(view);
		update(&view->base);
	}

	if (view_set_position(&view->base, x, y)) {
		update_extents(view);

		if (view->visible) {
			/* Assume worst-case no clipping until we draw the next frame (in case the
			 * surface gets moved again before that). */
			pixman_region32_init(&view->clip);

			view_update_screens(&view->base);
			damage_below_view(view);
			update(&view->base);
		}
	}

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

	view = malloc(sizeof(*view));

	if (!view)
		return NULL;

	view_initialize(&view->base, &view_impl);
	view->surface = surface;
	view->buffer = NULL;
	view->window = NULL;
	view->parent = NULL;
	view->visible = false;
	view->extents.x1 = 0;
	view->extents.y1 = 0;
	view->extents.x2 = 0;
	view->extents.y2 = 0;
	view->border.width = 0;
	view->border.color = 0x000000;
	view->border.damaged = false;
	pixman_region32_init(&view->clip);
	wl_signal_init(&view->destroy_signal);
	surface_set_view(surface, &view->base);
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
	return view->impl == &view_impl ? (struct compositor_view *)view : NULL;
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

	if (parent->visible)
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
	struct compositor_view *other;

	if (view->visible)
		return;

	view->visible = true;
	view_update_screens(&view->base);

	/* Assume worst-case no clipping until we draw the next frame (in case the
	 * surface gets moved before that. */
	pixman_region32_clear(&view->clip);
	damage_view(view);
	update(&view->base);

	wl_list_for_each (other, &compositor.views, link) {
		if (other->parent == view)
			compositor_view_show(other);
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
	struct compositor_view *other;

	if (!view->visible)
		return;

	/* Update all the screens the view was on. */
	update(&view->base);
	damage_below_view(view);

	view_set_screens(&view->base, 0);
	view->visible = false;

	wl_list_for_each (other, &compositor.views, link) {
		if (other->parent == view)
			compositor_view_hide(other);
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
 * \details
 * Walks the view stack top-down, computes per-view clip regions, accumulates
 * surface damage into the global compositor damage region, and handles border
 * damage.
 */
static void
calculate_damage(void)
{
	struct compositor_view *view;
	struct swc_rectangle *geom;
	pixman_region32_t surface_opaque, *surface_damage;

	pixman_region32_clear(&compositor.opaque);
	pixman_region32_init(&surface_opaque);

	/* Go through views top-down to calculate clipping regions. */
	wl_list_for_each (view, &compositor.views, link) {
		if (!view->visible)
			continue;

		geom = &view->base.geometry;

		/* Clip the surface by the opaque region covering it. */
		pixman_region32_copy(&view->clip, &compositor.opaque);

		/* Translate the opaque region to global coordinates. */
		pixman_region32_copy(&surface_opaque, &view->surface->state.opaque);
		pixman_region32_translate(&surface_opaque, geom->x, geom->y);

		/* Add the surface's opaque region to the accumulated opaque region. */
		pixman_region32_union(&compositor.opaque, &compositor.opaque, &surface_opaque);

		surface_damage = &view->surface->state.damage;

		if (pixman_region32_not_empty(surface_damage)) {
			renderer_flush_view(view);

			/* Translate surface damage to global coordinates. */
			pixman_region32_translate(surface_damage, geom->x, geom->y);

			/* Add the surface damage to the compositor damage. */
			pixman_region32_union(&compositor.damage, &compositor.damage, surface_damage);
			pixman_region32_clear(surface_damage);
		}

		if (view->border.damaged) {
			pixman_region32_t border_region, view_region;

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
	struct target *target;
	const struct swc_rectangle *geom = &screen->base.geometry;
	pixman_region32_t damage, *total_damage;

	if (!(compositor.scheduled_updates & screen_mask(screen)))
		return;

	if (!(target = target_get(screen)))
		return;

	pixman_region32_init(&damage);
	pixman_region32_intersect_rect(&damage, &compositor.damage, geom->x, geom->y, geom->width, geom->height);
	pixman_region32_translate(&damage, -geom->x, -geom->y);
	total_damage = wld_surface_damage(target->surface, &damage);

	/* Don't repaint the screen if it is waiting for a page flip. */
	if (compositor.pending_flips & screen_mask(screen)) {
		pixman_region32_fini(&damage);
		return;
	}

	pixman_region32_t base_damage;
	pixman_region32_copy(&damage, total_damage);
	pixman_region32_translate(&damage, geom->x, geom->y);
	pixman_region32_init(&base_damage);
	pixman_region32_subtract(&base_damage, &damage, &compositor.opaque);
	renderer_repaint(target, &damage, &base_damage, &compositor.views);
	pixman_region32_fini(&damage);
	pixman_region32_fini(&base_damage);

	switch (target_swap_buffers(target)) {
	case -EACCES:
		/* If we get an EACCES, it is because this session is being deactivated, but
		 * we haven't yet received the deactivate signal from swc-launch. */
		swc_deactivate();
		break;
	case 0:
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
	struct screen *screen;
	uint32_t updates = compositor.scheduled_updates & ~compositor.pending_flips;

	if (!swc.active || !updates)
		return;

	DEBUG("Performing update\n");

	compositor.updating = true;
	calculate_damage();

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
	struct compositor_view *view;
	bool found = false;
	int32_t x = wl_fixed_to_int(fx), y = wl_fixed_to_int(fy);
	struct swc_rectangle *geom;

	/* If buttons are pressed, don't change pointer focus. */
	if (swc.seat->pointer->buttons.size > 0)
		return false;

	wl_list_for_each (view, &compositor.views, link) {
		if (!view->visible)
			continue;
		geom = &view->base.geometry;
		if (rectangle_contains_point(geom, x, y)) {
			if (pixman_region32_contains_point(&view->surface->state.input, x - geom->x, y - geom->y, NULL)) {
				found = true;
				break;
			}
		}
	}

	pointer_set_focus(swc.seat->pointer, found ? view : NULL);

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

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
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
	uint8_t vt = value - XKB_KEY_XF86Switch_VT_1 + 1;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
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
	struct event *event = data;

	switch (event->type) {
	case SWC_EVENT_ACTIVATED:
		schedule_updates(-1);
		break;
	case SWC_EVENT_DEACTIVATED:
		compositor.scheduled_updates = 0;
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

	compositor.scheduled_updates = 0;
	compositor.pending_flips = 0;
	compositor.updating = false;
	pixman_region32_init(&compositor.damage);
	pixman_region32_init(&compositor.opaque);
	wl_list_init(&compositor.views);
	wl_signal_init(&swc_compositor.signal.new_surface);
	compositor.swc_listener.notify = &handle_swc_event;
	wl_signal_add(&swc.event_signal, &compositor.swc_listener);

	wl_list_for_each (screen, &swc.screens, link)
		target_new(screen);
	if (swc.active)
		schedule_updates(-1);

	swc_add_binding(SWC_BINDING_KEY, SWC_MOD_CTRL | SWC_MOD_ALT, XKB_KEY_BackSpace, &handle_terminate, NULL);

	for (keysym = XKB_KEY_XF86Switch_VT_1; keysym <= XKB_KEY_XF86Switch_VT_12; ++keysym)
		swc_add_binding(SWC_BINDING_KEY, SWC_MOD_ANY, keysym, &handle_switch_vt, NULL);

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