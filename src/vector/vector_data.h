/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "../osmium.h"
#include "../proj/projection.h"

typedef enum {
  OSMIUM_VECTOR_POINT,
  OSMIUM_VECTOR_LINE,
  OSMIUM_VECTOR_POLYGON
} OsmiumVectorKind;

typedef struct {
  long long fid;
  OsmiumVectorKind kind;
  double min_x, min_y, max_x, max_y;
  size_t n_parts;
  size_t *part_start;
  double *ll;
  double *xy;
} OsmiumVectorFeature;

typedef struct {
  atomic_int refs;
  OsmiumVectorFeature *features;
  size_t n_features;
  double min_x, min_y, max_x, max_y;
  double lon_min, lat_min, lon_max, lat_max;

  char *projected_definition;
  pthread_rwlock_t xy_lock;

  char *crs_definition;
  char *crs_name;
  char *crs_code;
  int crs_assumed;
  int crs_ballpark;
  double crs_accuracy;

  char *source_path;
  char *vsi_name;
  char *buffer;
  pthread_mutex_t lock;
  void *dataset;
} OsmiumVectorData;

typedef enum {
  OSMIUM_FIELD_STRING,
  OSMIUM_FIELD_INTEGER,
  OSMIUM_FIELD_REAL,
  OSMIUM_FIELD_DATE,
  OSMIUM_FIELD_OTHER
} OsmiumFieldType;

typedef struct {
  char *name;
  char *value;
  OsmiumFieldType type;
  int width;
} OsmiumVectorField;

typedef struct {
  long long fid;
  const char *field;
  const char *value;
} OsmiumVectorEdit;

/* source_crs overrides the coordinate system stored in the file (NULL or empty = use the file's).
 * The coordinates are projected into the given map coordinate system. */
osmium_error osmium_vector_data_open(const char *path,
                                     const char *user_agent,
                                     const char *source_crs,
                                     const OsmiumProjection *projection,
                                     OsmiumVectorData **out);

/* Re-projects the geometries for another map CRS. Cheap when already projected. */
osmium_error osmium_vector_data_project(OsmiumVectorData *vec, const OsmiumProjection *projection);

/* Geometries are only valid between begin_read and end_read; begin_read projects when needed
 * (try_begin_read never projects and fails instead). */
int osmium_vector_data_begin_read(OsmiumVectorData *vec, const OsmiumProjection *projection);
int osmium_vector_data_try_begin_read(OsmiumVectorData *vec, const OsmiumProjection *projection);
void osmium_vector_data_end_read(OsmiumVectorData *vec);

OsmiumVectorData *osmium_vector_data_ref(OsmiumVectorData *vec);

void osmium_vector_data_free(OsmiumVectorData *vec);

size_t osmium_vector_data_hit_test(const OsmiumVectorData *vec,
                                   double x,
                                   double y,
                                   double tolerance,
                                   long long *fids,
                                   size_t max_fids);

osmium_error osmium_vector_data_get_attributes(OsmiumVectorData *vec,
                                               long long fid,
                                               OsmiumVectorField **fields,
                                               size_t *n_fields);

void osmium_vector_fields_free(OsmiumVectorField *fields, size_t n_fields);

int osmium_vector_data_is_editable(const OsmiumVectorData *vec);

osmium_error osmium_vector_data_get_fields(OsmiumVectorData *vec,
                                           OsmiumVectorField **fields,
                                           size_t *n_fields);

void osmium_vector_data_release(OsmiumVectorData *vec);

osmium_error osmium_vector_write_attributes(const char *path,
                                            const OsmiumVectorEdit *edits,
                                            size_t n_edits,
                                            char *message,
                                            size_t message_size);

osmium_error osmium_vector_add_field(const char *path,
                                     const char *name,
                                     OsmiumFieldType type,
                                     char *message,
                                     size_t message_size);

osmium_error osmium_vector_rename_field(const char *path,
                                        const char *old_name,
                                        const char *new_name,
                                        char *message,
                                        size_t message_size);

osmium_error osmium_vector_delete_field(const char *path,
                                        const char *name,
                                        char *message,
                                        size_t message_size);
