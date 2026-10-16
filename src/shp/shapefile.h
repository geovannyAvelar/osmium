/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#include "../osmium.h"

typedef enum {
  OSMIUM_SHP_POINT,
  OSMIUM_SHP_LINE,
  OSMIUM_SHP_POLYGON
} OsmiumShpKind;

typedef struct {
  OsmiumShpKind kind;
  double min_x, min_y, max_x, max_y;
  size_t n_parts;
  size_t *part_start;
  double *xy;
} OsmiumShpFeature;

typedef struct {
  OsmiumShpFeature *features;
  size_t n_features;
  double min_x, min_y, max_x, max_y;
} OsmiumShapefile;

osmium_error osmium_shapefile_open(const char *path, OsmiumShapefile **out);

void osmium_shapefile_free(OsmiumShapefile *shp);
