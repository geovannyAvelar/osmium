/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

#include "osmium_map.h"

G_BEGIN_DECLS

typedef enum {
  OSMIUM_UNITS_METRIC,
  OSMIUM_UNITS_IMPERIAL
} OsmiumUnitSystem;

/* Distances and areas on the WGS 84 ellipsoid. Positions are latitude/longitude in degrees; results
 * are in metres and square metres. Paths are interleaved lat/lon pairs. */
OSMIUM_API double osmium_geodesic_distance(double lat1, double lon1, double lat2, double lon2);
OSMIUM_API double osmium_geodesic_path_length(const double *latlon, guint n_points);

/* Area of the polygon closed from the first to the last point, always positive. When perimeter is
 * not NULL it receives the length of the closed ring. */
OSMIUM_API double osmium_geodesic_polygon_area(const double *latlon, guint n_points, double *perimeter);

/* Human readable text such as "1.250 km" or "3.20 ha". Free the result with g_free(). */
OSMIUM_API char *osmium_format_distance(double metres, OsmiumUnitSystem units);
OSMIUM_API char *osmium_format_area(double square_metres, OsmiumUnitSystem units);

G_END_DECLS
