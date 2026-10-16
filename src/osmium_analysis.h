/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

/* Vector and raster analysis on files. Inputs are paths of files GDAL/OGR can read; outputs are
 * written to a new file whose format follows the extension (.shp, .gpkg, .geojson for vectors, .tif
 * for rasters). An existing output file is replaced. Coordinates of the second input are converted to
 * the coordinate system of the first. */

typedef enum {
  OSMIUM_OVERLAY_CLIP,
  OSMIUM_OVERLAY_INTERSECT,
  OSMIUM_OVERLAY_DIFFERENCE
} OsmiumOverlayOp;

typedef enum {
  OSMIUM_PREDICATE_INTERSECTS,
  OSMIUM_PREDICATE_DISJOINT,
  OSMIUM_PREDICATE_CONTAINS,
  OSMIUM_PREDICATE_WITHIN,
  OSMIUM_PREDICATE_TOUCHES,
  OSMIUM_PREDICATE_CROSSES,
  OSMIUM_PREDICATE_OVERLAPS
} OsmiumPredicate;

typedef enum {
  OSMIUM_DEM_HILLSHADE,
  OSMIUM_DEM_SLOPE,
  OSMIUM_DEM_ASPECT,
  OSMIUM_DEM_ROUGHNESS
} OsmiumDemMode;

/* distance in metres (when in_metres) or in the units of the layer's coordinate system. Geographic
 * layers are buffered on a local azimuthal projection of each feature, so metres are real metres. */
OSMIUM_API gboolean osmium_analysis_buffer(const char *input, const char *output, double distance, gboolean in_metres,
                                           guint segments, gboolean dissolve, GError **error);

OSMIUM_API gboolean osmium_analysis_overlay(const char *input, const char *overlay, const char *output,
                                            OsmiumOverlayOp op, GError **error);

/* group_field may be NULL to dissolve everything into one feature. */
OSMIUM_API gboolean osmium_analysis_dissolve(const char *input, const char *output, const char *group_field,
                                             GError **error);

/* Feature ids of input that satisfy the predicate against at least one feature of selector. */
OSMIUM_API GArray *osmium_analysis_select_by_location(const char *input, const char *selector, OsmiumPredicate predicate,
                                                      GError **error);

/* Copies input and adds the fields of the first feature of join that satisfies the predicate (with
 * prefix) and a "join_n" count of matches. */
OSMIUM_API gboolean osmium_analysis_join_by_location(const char *input, const char *join, const char *output,
                                                     OsmiumPredicate predicate, const char *prefix, GError **error);

OSMIUM_API gboolean osmium_analysis_centroids(const char *input, const char *output, GError **error);

/* Cell by cell expression over up to 8 rasters named A..H (band is the 1-based band of each). Operators
 * + - * / ^ and comparisons (result 1 or 0); functions abs sqrt exp ln log10 sin cos tan min max pow floor ceil
 * round if(cond, a, b). Cells that are no-data in any input become no-data. */
OSMIUM_API gboolean osmium_analysis_raster_calc(const char *const *inputs, const int *bands, guint n_inputs,
                                                const char *expression, const char *output, GError **error);

/* Validates an expression for n_inputs variables; returns an error message or NULL. */
OSMIUM_API char *osmium_analysis_check_expression(const char *expression, guint n_inputs);

/* Adds count, sum, mean, min, max and std of the raster cells under each polygon (names get prefix). */
OSMIUM_API gboolean osmium_analysis_zonal_stats(const char *polygons, const char *raster, int band, const char *prefix,
                                                const char *output, GError **error);

OSMIUM_API gboolean osmium_analysis_dem(const char *raster, int band, OsmiumDemMode mode, double z_factor,
                                        double azimuth, double altitude, const char *output, GError **error);

OSMIUM_API gboolean osmium_analysis_contours(const char *raster, int band, double interval, double offset,
                                             const char *output, GError **error);

G_END_DECLS
