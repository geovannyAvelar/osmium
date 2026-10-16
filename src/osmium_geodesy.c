/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_geodesy.h"

#include <geodesic.h>

#define WGS84_A 6378137.0
#define WGS84_F (1.0 / 298.257223563)
#define METRES_PER_FOOT 0.3048
#define FEET_PER_MILE 5280.0
#define SQUARE_FEET_PER_ACRE 43560.0
#define ACRES_PER_SQUARE_MILE 640.0

static void init_geodesic(struct geod_geodesic *g) {
  geod_init(g, WGS84_A, WGS84_F);
}

double osmium_geodesic_distance(double lat1, double lon1, double lat2, double lon2) {
  struct geod_geodesic g;
  double metres = 0.0;

  init_geodesic(&g);
  geod_inverse(&g, lat1, lon1, lat2, lon2, &metres, NULL, NULL);
  return metres;
}

double osmium_geodesic_path_length(const double *latlon, guint n_points) {
  double total = 0.0;

  g_return_val_if_fail(latlon != NULL || n_points == 0, 0.0);

  for (guint i = 1; i < n_points; i++) {
    total += osmium_geodesic_distance(latlon[2 * (i - 1)], latlon[2 * (i - 1) + 1], latlon[2 * i], latlon[2 * i + 1]);
  }
  return total;
}

double osmium_geodesic_polygon_area(const double *latlon, guint n_points, double *perimeter) {
  struct geod_geodesic g;
  double area = 0.0;
  double ring = 0.0;

  if (perimeter != NULL) {
    *perimeter = 0.0;
  }
  if (n_points < 3) {
    if (perimeter != NULL && n_points == 2) {
      *perimeter = 2.0 * osmium_geodesic_path_length(latlon, n_points);
    }
    return 0.0;
  }

  g_autofree double *lats = g_new(double, n_points);
  g_autofree double *lons = g_new(double, n_points);

  for (guint i = 0; i < n_points; i++) {
    lats[i] = latlon[2 * i];
    lons[i] = latlon[2 * i + 1];
  }

  init_geodesic(&g);
  geod_polygonarea(&g, lats, lons, (int)n_points, &area, &ring);
  if (perimeter != NULL) {
    *perimeter = ring;
  }
  return area < 0.0 ? -area : area;
}

char *osmium_format_distance(double metres, OsmiumUnitSystem units) {
  if (units == OSMIUM_UNITS_IMPERIAL) {
    const double feet = metres / METRES_PER_FOOT;

    return feet < FEET_PER_MILE ? g_strdup_printf("%.1f ft", feet) : g_strdup_printf("%.3f mi", feet / FEET_PER_MILE);
  }
  return metres < 1000.0 ? g_strdup_printf("%.1f m", metres) : g_strdup_printf("%.3f km", metres / 1000.0);
}

char *osmium_format_area(double square_metres, OsmiumUnitSystem units) {
  if (units == OSMIUM_UNITS_IMPERIAL) {
    const double square_feet = square_metres / (METRES_PER_FOOT * METRES_PER_FOOT);
    const double acres = square_feet / SQUARE_FEET_PER_ACRE;

    if (square_feet < SQUARE_FEET_PER_ACRE) {
      return g_strdup_printf("%.0f ft²", square_feet);
    }
    return acres < ACRES_PER_SQUARE_MILE ? g_strdup_printf("%.2f ac", acres)
                                         : g_strdup_printf("%.3f mi²", acres / ACRES_PER_SQUARE_MILE);
  }
  if (square_metres < 10000.0) {
    return g_strdup_printf("%.1f m²", square_metres);
  }
  return square_metres < 1000000.0 ? g_strdup_printf("%.2f ha", square_metres / 10000.0)
                                   : g_strdup_printf("%.3f km²", square_metres / 1000000.0);
}
