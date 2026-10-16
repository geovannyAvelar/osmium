/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#include "../osmium.h"

typedef struct OsmiumRaster OsmiumRaster;

osmium_error osmium_raster_open(const char *path, const char *user_agent, OsmiumRaster **out);

OsmiumRaster *osmium_raster_ref(OsmiumRaster *raster);

void osmium_raster_unref(OsmiumRaster *raster);

void osmium_raster_get_extent(const OsmiumRaster *raster,
                              double *min_x,
                              double *min_y,
                              double *max_x,
                              double *max_y);

osmium_error osmium_raster_render(const OsmiumRaster *raster,
                                  double min_x,
                                  double min_y,
                                  double max_x,
                                  double max_y,
                                  int width,
                                  int height,
                                  unsigned char **rgba);
