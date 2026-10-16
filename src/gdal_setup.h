/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/* Registers the GDAL drivers once for the whole library. Registration is not safe to run from two
 * threads at once, so every caller goes through this single guard. */
void osmium_gdal_init(void);
