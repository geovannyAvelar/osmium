/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#include "../osmium.h"

typedef enum {
  OSMIUM_VECTOR_POINT,
  OSMIUM_VECTOR_LINE,
  OSMIUM_VECTOR_POLYGON
} OsmiumVectorKind;

typedef struct {
  OsmiumVectorKind kind;
  double min_x, min_y, max_x, max_y;
  size_t n_parts;
  size_t *part_start;
  double *xy;
} OsmiumVectorFeature;

typedef struct {
  OsmiumVectorFeature *features;
  size_t n_features;
  double min_x, min_y, max_x, max_y;
} OsmiumVectorData;

osmium_error osmium_vector_data_open(const char *path,
                                     const char *user_agent,
                                     OsmiumVectorData **out);

void osmium_vector_data_free(OsmiumVectorData *vec);
