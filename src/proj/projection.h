/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

/* A coordinate reference system the map can be drawn in.
 *
 * Positions are exchanged as WGS 84 longitude/latitude in degrees and as "normalized" coordinates:
 * the CRS is scaled so its nominal world fills a square of side 1, with y growing downwards. A
 * normalized coordinate times 256 * 2^zoom is a world pixel. EPSG:3857 matches slippy-map tiles. */
typedef struct OsmiumProjection OsmiumProjection;

typedef struct {
  char *name;
  char *code;
  int geographic;
  int ballpark;
} OsmiumCrsInfo;

OsmiumProjection *osmium_projection_new(const char *definition, char *message, size_t message_size);
OsmiumProjection *osmium_projection_ref(OsmiumProjection *projection);
void osmium_projection_unref(OsmiumProjection *projection);

const char *osmium_projection_get_definition(const OsmiumProjection *projection);
const char *osmium_projection_get_name(const OsmiumProjection *projection);
/* "EPSG:4326" style identifier, or NULL for a CRS without an authority code */
const char *osmium_projection_get_code(const OsmiumProjection *projection);
int osmium_projection_is_geographic(const OsmiumProjection *projection);
int osmium_projection_is_web_mercator(const OsmiumProjection *projection);

/* Converts normalized coordinates to CRS units and back. */
void osmium_projection_normalized_to_units(const OsmiumProjection *projection, double nx, double ny, double *x, double *y);
void osmium_projection_units_to_normalized(const OsmiumProjection *projection, double x, double y, double *nx, double *ny);

/* lon/lat (degrees) <-> normalized. Returns 0 when a position cannot be converted. */
int osmium_projection_forward(const OsmiumProjection *projection, double lon, double lat, double *nx, double *ny);
int osmium_projection_inverse(const OsmiumProjection *projection, double nx, double ny, double *lon, double *lat);

/* In place conversion of n interleaved lon/lat pairs to normalized x/y. Positions that fail are
 * set to NaN. Returns the number of positions that failed. */
size_t osmium_projection_forward_many(const OsmiumProjection *projection, double *xy, size_t n);

/* Datum aware conversion between two arbitrary CRSs, picking the best PROJ operation for the area
 * (west, south, east, north in degrees; pass NULL when unknown). */
typedef struct OsmiumTransform OsmiumTransform;

OsmiumTransform *osmium_transform_new(const char *source,
                                      const char *target,
                                      const double *area,
                                      char *message,
                                      size_t message_size);
void osmium_transform_free(OsmiumTransform *transform);

/* 1 when only an approximate (ballpark) datum shift is available */
int osmium_transform_is_ballpark(const OsmiumTransform *transform);
/* Estimated accuracy in metres, or -1 when unknown */
double osmium_transform_accuracy(const OsmiumTransform *transform);

/* In place conversion of n interleaved x/y pairs in the order of the CRS "as usual for maps"
 * (longitude/easting first). Failed positions become NaN; returns how many failed. */
size_t osmium_transform_apply(const OsmiumTransform *transform, double *xy, size_t n);

/* Describes a CRS definition (name, code, geographic?). Returns 0 when it is not valid. */
int osmium_crs_describe_internal(const char *definition, OsmiumCrsInfo *info);
void osmium_crs_info_clear(OsmiumCrsInfo *info);

/* Lets PROJ download missing datum shift grids. Off by default. */
void osmium_proj_set_network_enabled(int enabled);
int osmium_proj_get_network_enabled(void);

/* 1 when the authority axis order of the CRS lists north (latitude/northing) before east, which
 * WMS 1.3.0 requires to be honoured in bounding boxes. */
int osmium_crs_axis_swapped(const char *definition);
