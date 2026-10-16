/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#include "../osmium.h"

typedef struct OsmiumRaster OsmiumRaster;

osmium_error osmium_raster_open(const char *path,
                                const char *user_agent,
                                const char *source_crs,
                                OsmiumRaster **out);

OsmiumRaster *osmium_raster_ref(OsmiumRaster *raster);

void osmium_raster_unref(OsmiumRaster *raster);

/* Extent in WGS 84 degrees. */
void osmium_raster_get_extent(const OsmiumRaster *raster,
                              double *lon_min,
                              double *lat_min,
                              double *lon_max,
                              double *lat_max);

void osmium_raster_get_crs_info(const OsmiumRaster *raster,
                                const char **name,
                                const char **code,
                                int *assumed,
                                int *ballpark,
                                double *accuracy);

/* Band layout of the file and the style a fresh layer starts with (free both). */
OsmiumRasterInfo *osmium_raster_dup_info(const OsmiumRaster *raster);
OsmiumRasterStyle *osmium_raster_dup_default_style(const OsmiumRaster *raster);

/* Display range for a band by the given method. */
osmium_error osmium_raster_compute_stretch(const OsmiumRaster *raster,
                                           int band,
                                           OsmiumRasterStretch mode,
                                           double *min,
                                           double *max);

/* Renders the area (in units of target_crs) at width x height pixels as RGBA. */
osmium_error osmium_raster_render(const OsmiumRaster *raster,
                                  const OsmiumRasterStyle *style,
                                  const char *target_crs,
                                  double min_x,
                                  double min_y,
                                  double max_x,
                                  double max_y,
                                  int width,
                                  int height,
                                  unsigned char **rgba);

/* Reprojects an encoded image (PNG, JPEG, ...) whose bounding box is known, as returned by a map
 * server, into target_crs. Boxes are minx, miny, maxx, maxy in CRS units. Returns RGBA. */
osmium_error osmium_image_reproject(const unsigned char *data,
                                    size_t len,
                                    const char *source_crs,
                                    const double *source_box,
                                    const char *target_crs,
                                    const double *target_box,
                                    int width,
                                    int height,
                                    unsigned char **rgba);
