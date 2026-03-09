/**
 * \file mode.c
 * \brief Display mode helpers.
 *
 * \author Michael Forney
 * \date 2013
 * \copyright MIT
 *
 * Implementation of helpers for working with DRM display modes.
 * Provides initialization and comparison utilities for the
 * \ref mode structure defined in mode.h.
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

#include "mode.h"

/**
 * \brief Initialize a mode from a DRM mode description.
 *
 * \param mode Destination mode structure.
 * \param mode_info Source DRM mode.
 *
 * Copies relevant fields from \p mode_info into \p mode.
 *
 * \return true on success.
 */
bool
mode_initialize(struct mode *mode, const drmModeModeInfo *mode_info)
{
	mode->width = mode_info->hdisplay;
	mode->height = mode_info->vdisplay;
	mode->refresh = mode_info->vrefresh * 1000;
	mode->preferred = mode_info->type & DRM_MODE_TYPE_PREFERRED;
	mode->info = *mode_info;
	return true;
}

/**
 * \brief Compare two modes for equality.
 *
 * Two modes are considered equal if their resolution and refresh rate match.
 *
 * \param mode1 First mode.
 * \param mode2 Second mode.
 *
 * \return true if the modes are equivalent.
 */
bool
mode_equal(const struct mode *mode1, const struct mode *mode2)
{
	return mode1->width == mode2->width
	       && mode1->height == mode2->height
	       && mode1->refresh == mode2->refresh;
}
