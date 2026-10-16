/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include "osmium_map.h"

G_BEGIN_DECLS

/* Checks a coordinate system definition (EPSG:4326, WKT, PROJ string...). On success name and
 * code are set (code may be NULL) and must be freed with g_free(). */
OSMIUM_API gboolean osmium_crs_describe(const char *definition, char **name, char **code, gboolean *geographic);

/* Datum shifts sometimes need grids that PROJ can download on demand. Off by default. */
OSMIUM_API void osmium_set_proj_network_enabled(gboolean enabled);
OSMIUM_API gboolean osmium_get_proj_network_enabled(void);

G_END_DECLS
