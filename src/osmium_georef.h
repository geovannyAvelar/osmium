/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

/* A ground control point: a pixel of the image (col from the left, row from the top, in image
 * pixels) and the position it has in the target coordinate system. */
typedef struct {
  double col;
  double row;
  double x;
  double y;
  gboolean enabled;
} OsmiumGcp;

typedef enum {
  OSMIUM_GEOREF_LINEAR,
  OSMIUM_GEOREF_POLYNOMIAL_2,
  OSMIUM_GEOREF_POLYNOMIAL_3,
  OSMIUM_GEOREF_THIN_PLATE_SPLINE
} OsmiumGeorefMethod;

typedef enum {
  OSMIUM_GEOREF_NEAREST,
  OSMIUM_GEOREF_BILINEAR,
  OSMIUM_GEOREF_CUBIC
} OsmiumGeorefResampling;

/* Points an image needs for the method (the enabled ones are counted). */
OSMIUM_API guint osmium_georef_minimum_points(OsmiumGeorefMethod method);

OSMIUM_API gboolean osmium_georef_image_size(const char *path, guint *width, guint *height, GError **error);

/* Distance, in target CRS units, between each enabled point and where the fitted transformation
 * puts it; disabled points get a negative value. rms is the root mean square of the enabled ones. */
OSMIUM_API gboolean osmium_georef_residuals(const OsmiumGcp *points, guint n_points, OsmiumGeorefMethod method,
                                            double *residuals, double *rms, GError **error);

/* Warps the image into a GeoTIFF (compressed, with transparency outside the image). */
OSMIUM_API gboolean osmium_georef_run(const char *input,
                                      const char *output,
                                      const OsmiumGcp *points,
                                      guint n_points,
                                      OsmiumGeorefMethod method,
                                      const char *target_crs,
                                      OsmiumGeorefResampling resampling,
                                      GError **error);

/* GCP files in the QGIS .points layout: a "#CRS:" line, then mapX,mapY,sourceX,sourceY,enable,... with
 * the source Y negative. */
OSMIUM_API gboolean osmium_georef_save_points(const char *path, const OsmiumGcp *points, guint n_points,
                                              const char *crs, GError **error);
OSMIUM_API GArray *osmium_georef_load_points(const char *path, char **crs, GError **error);

/* Converts a position between two coordinate systems (x/y as lon/lat or easting/northing). */
OSMIUM_API gboolean osmium_crs_transform_point(const char *source, const char *target, double x, double y,
                                               double *out_x, double *out_y);

G_END_DECLS
