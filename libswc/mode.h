/**
 * \file mode.h
 * \brief Display mode representation and helpers.
 * \author Michael Forney
 * \date 2013
 * \copyright MIT
 *
 * This module provides a lightweight wrapper around DRM display modes
 * (`drmModeModeInfo`). It exposes commonly used properties such as
 * resolution and refresh rate while preserving the original DRM mode
 * structure.
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

#ifndef SWC_MODE_H
#define SWC_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xf86drmMode.h>

/**
 * \brief Display mode description.
 *
 * Stores basic mode parameters derived from a DRM mode together with the
 * original `drmModeModeInfo`.
 */
struct mode {
	uint16_t width;       /**< Horizontal resolution in pixels */
	uint16_t height;      /**< Vertical resolution in pixels */
	uint32_t refresh;     /**< Refresh rate in mHz */
	bool preferred;       /**< Whether this mode is marked as preferred */
	drmModeModeInfo info; /**< Original DRM mode */
};

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
bool mode_initialize(struct mode *mode, const drmModeModeInfo *mode_info);

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
bool mode_equal(const struct mode *mode1, const struct mode *mode2);

#endif
