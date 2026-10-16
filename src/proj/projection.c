/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "projection.h"

#include <math.h>
#include <proj.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEB_MERCATOR_HALF_WORLD 20037508.342789244
#define MAX_MERCATOR_LATITUDE 85.0511287798066
#define TRANSFORM_CACHE_SIZE 8

struct OsmiumProjection {
  atomic_int refs;
  char *definition;
  char *name;
  char *code;
  int geographic;
  int web_mercator;
  double origin_x;
  double origin_y;
  double span;
  double meters_per_unit;
};

struct OsmiumTransform {
  char *source;
  char *target;
  double area[4];
  int has_area;
  int ballpark;
  double accuracy;
  PJ *transformer;
  PJ_CONTEXT *context;
};

static atomic_int network_enabled;

static __thread PJ_CONTEXT *thread_context;

typedef struct {
  char *definition;
  PJ *forward;
} CacheEntry;

static __thread CacheEntry thread_cache[TRANSFORM_CACHE_SIZE];
static __thread unsigned thread_cache_next;

static PJ_CONTEXT *context_for_thread(void) {
  if (!thread_context) {
    thread_context = proj_context_create();
    proj_context_set_enable_network(thread_context, atomic_load(&network_enabled));
  }
  return thread_context;
}

void osmium_proj_set_network_enabled(int enabled) {
  atomic_store(&network_enabled, enabled ? 1 : 0);
  if (thread_context) {
    proj_context_set_enable_network(thread_context, enabled ? 1 : 0);
  }
}

int osmium_proj_get_network_enabled(void) {
  return atomic_load(&network_enabled);
}

static void set_message(char *message, size_t size, const char *text) {
  if (message && size > 0) {
    snprintf(message, size, "%s", text);
  }
}

static int is_valid_crs(PJ *crs) {
  return crs != NULL && proj_is_crs(crs);
}

OsmiumProjection *osmium_projection_new(const char *definition, char *message, size_t message_size) {
  if (!definition || !*definition) {
    set_message(message, message_size, "Empty coordinate system");
    return NULL;
  }

  PJ_CONTEXT *context = context_for_thread();
  PJ *crs = proj_create(context, definition);

  if (!is_valid_crs(crs)) {
    set_message(message, message_size, "Unknown coordinate system");
    if (crs) {
      proj_destroy(crs);
    }
    return NULL;
  }

  OsmiumProjection *projection = calloc(1, sizeof(*projection));
  if (!projection) {
    proj_destroy(crs);
    return NULL;
  }
  atomic_init(&projection->refs, 1);
  projection->definition = strdup(definition);

  const char *name = proj_get_name(crs);
  const char *authority = proj_get_id_auth_name(crs, 0);
  const char *code = proj_get_id_code(crs, 0);

  projection->name = strdup(name ? name : definition);
  if (authority && code) {
    size_t len = strlen(authority) + strlen(code) + 2;

    projection->code = malloc(len);
    if (projection->code) {
      snprintf(projection->code, len, "%s:%s", authority, code);
    }
  }

  const PJ_TYPE type = proj_get_type(crs);
  projection->geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS || type == PJ_TYPE_GEOGRAPHIC_3D_CRS ||
                           type == PJ_TYPE_GEODETIC_CRS;
  projection->web_mercator =
      projection->code && (strcmp(projection->code, "EPSG:3857") == 0 || strcmp(projection->code, "EPSG:900913") == 0);
  projection->meters_per_unit = 1.0;

  if (projection->geographic) {
    projection->origin_x = -180.0;
    projection->origin_y = 90.0;
    projection->span = 360.0;
  } else {
    PJ *cs = proj_crs_get_coordinate_system(context, crs);

    if (cs) {
      double factor = 1.0;

      if (proj_cs_get_axis_info(context, cs, 0, NULL, NULL, NULL, &factor, NULL, NULL, NULL) && factor > 0.0) {
        projection->meters_per_unit = factor;
      }
      proj_destroy(cs);
    }
    projection->origin_x = -WEB_MERCATOR_HALF_WORLD / projection->meters_per_unit;
    projection->origin_y = WEB_MERCATOR_HALF_WORLD / projection->meters_per_unit;
    projection->span = 2.0 * WEB_MERCATOR_HALF_WORLD / projection->meters_per_unit;
  }

  proj_destroy(crs);
  return projection;
}

OsmiumProjection *osmium_projection_ref(OsmiumProjection *projection) {
  atomic_fetch_add(&projection->refs, 1);
  return projection;
}

void osmium_projection_unref(OsmiumProjection *projection) {
  if (!projection || atomic_fetch_sub(&projection->refs, 1) != 1) {
    return;
  }

  free(projection->definition);
  free(projection->name);
  free(projection->code);
  free(projection);
}

const char *osmium_projection_get_definition(const OsmiumProjection *projection) {
  return projection->definition;
}

const char *osmium_projection_get_name(const OsmiumProjection *projection) {
  return projection->name;
}

const char *osmium_projection_get_code(const OsmiumProjection *projection) {
  return projection->code;
}

int osmium_projection_is_geographic(const OsmiumProjection *projection) {
  return projection->geographic;
}

int osmium_projection_is_web_mercator(const OsmiumProjection *projection) {
  return projection->web_mercator;
}

void osmium_projection_normalized_to_units(const OsmiumProjection *projection, double nx, double ny, double *x, double *y) {
  *x = projection->origin_x + nx * projection->span;
  *y = projection->origin_y - ny * projection->span;
}

void osmium_projection_units_to_normalized(const OsmiumProjection *projection, double x, double y, double *nx, double *ny) {
  *nx = (x - projection->origin_x) / projection->span;
  *ny = (projection->origin_y - y) / projection->span;
}

static PJ *thread_transformer(const OsmiumProjection *projection, int forward) {
  PJ_CONTEXT *context = context_for_thread();
  const char *source = forward ? "EPSG:4326" : projection->definition;
  const char *target = forward ? projection->definition : "EPSG:4326";

  if (forward) {
    for (int i = 0; i < TRANSFORM_CACHE_SIZE; i++) {
      if (thread_cache[i].definition && strcmp(thread_cache[i].definition, projection->definition) == 0) {
        return thread_cache[i].forward;
      }
    }
  }

  PJ *raw = proj_create_crs_to_crs(context, source, target, NULL);
  PJ *normalized = raw ? proj_normalize_for_visualization(context, raw) : NULL;

  if (raw) {
    proj_destroy(raw);
  }

  if (forward && normalized) {
    CacheEntry *slot = &thread_cache[thread_cache_next++ % TRANSFORM_CACHE_SIZE];

    if (slot->forward) {
      proj_destroy(slot->forward);
    }
    free(slot->definition);
    slot->definition = strdup(projection->definition);
    slot->forward = normalized;
  }
  return normalized;
}

int osmium_projection_forward(const OsmiumProjection *projection, double lon, double lat, double *nx, double *ny) {
  if (projection->web_mercator) {
    const double clamped = fmax(-MAX_MERCATOR_LATITUDE, fmin(MAX_MERCATOR_LATITUDE, lat));
    const double rad = clamped * M_PI / 180.0;

    *nx = (lon + 180.0) / 360.0;
    *ny = (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0;
    return 1;
  }

  PJ *transformer = thread_transformer(projection, 1);
  if (!transformer) {
    return 0;
  }

  PJ_COORD in = proj_coord(lon, lat, 0.0, 0.0);
  PJ_COORD out = proj_trans(transformer, PJ_FWD, in);

  if (!isfinite(out.xy.x) || !isfinite(out.xy.y)) {
    return 0;
  }

  osmium_projection_units_to_normalized(projection, out.xy.x, out.xy.y, nx, ny);
  return 1;
}

int osmium_projection_inverse(const OsmiumProjection *projection, double nx, double ny, double *lon, double *lat) {
  if (projection->web_mercator) {
    *lon = nx * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * ny))) * 180.0 / M_PI;
    return 1;
  }

  PJ *transformer = thread_transformer(projection, 0);
  double x, y;

  if (!transformer) {
    return 0;
  }

  osmium_projection_normalized_to_units(projection, nx, ny, &x, &y);

  PJ_COORD out = proj_trans(transformer, PJ_FWD, proj_coord(x, y, 0.0, 0.0));

  proj_destroy(transformer);
  if (!isfinite(out.xy.x) || !isfinite(out.xy.y)) {
    return 0;
  }

  *lon = out.xy.x;
  *lat = out.xy.y;
  return 1;
}

size_t osmium_projection_forward_many(const OsmiumProjection *projection, double *xy, size_t n) {
  size_t failed = 0;

  if (projection->web_mercator) {
    for (size_t i = 0; i < n; i++) {
      double nx, ny;

      osmium_projection_forward(projection, xy[i * 2], xy[i * 2 + 1], &nx, &ny);
      xy[i * 2] = nx;
      xy[i * 2 + 1] = ny;
    }
    return 0;
  }

  PJ *transformer = thread_transformer(projection, 1);
  if (!transformer) {
    for (size_t i = 0; i < n * 2; i++) {
      xy[i] = NAN;
    }
    return n;
  }

  proj_trans_generic(transformer, PJ_FWD, xy, 2 * sizeof(double), n, xy + 1, 2 * sizeof(double), n, NULL, 0, 0,
                     NULL, 0, 0);

  for (size_t i = 0; i < n; i++) {
    if (!isfinite(xy[i * 2]) || !isfinite(xy[i * 2 + 1])) {
      xy[i * 2] = xy[i * 2 + 1] = NAN;
      failed++;
      continue;
    }
    osmium_projection_units_to_normalized(projection, xy[i * 2], xy[i * 2 + 1], &xy[i * 2], &xy[i * 2 + 1]);
  }
  return failed;
}

OsmiumTransform *osmium_transform_new(const char *source,
                                      const char *target,
                                      const double *area,
                                      char *message,
                                      size_t message_size) {
  PJ_CONTEXT *context = context_for_thread();
  PJ_AREA *proj_area = NULL;

  if (!source || !target) {
    return NULL;
  }

  if (area) {
    proj_area = proj_area_create();
    proj_area_set_bbox(proj_area, area[0], area[1], area[2], area[3]);
  }

  PJ *raw = proj_create_crs_to_crs(context, source, target, proj_area);
  if (proj_area) {
    proj_area_destroy(proj_area);
  }
  if (!raw) {
    set_message(message, message_size, "No transformation between these coordinate systems");
    return NULL;
  }

  OsmiumTransform *transform = calloc(1, sizeof(*transform));
  if (!transform) {
    proj_destroy(raw);
    return NULL;
  }

  transform->context = context;
  transform->source = strdup(source);
  transform->target = strdup(target);
  transform->transformer = proj_normalize_for_visualization(context, raw);
  transform->accuracy = -1.0;

  proj_destroy(raw);

  /* the operation PROJ really used is only known once a position went through it */
  if (transform->transformer) {
    const double cx = area ? (area[0] + area[2]) / 2.0 : 0.0;
    const double cy = area ? (area[1] + area[3]) / 2.0 : 0.0;
    PJ *probe_source = proj_create(context, source);
    PJ *to_geographic = probe_source ? proj_create_crs_to_crs(context, "EPSG:4326", source, NULL) : NULL;
    PJ *to_source = to_geographic ? proj_normalize_for_visualization(context, to_geographic) : NULL;
    PJ_COORD start = proj_coord(cx, cy, 0.0, 0.0);

    if (to_source) {
      start = proj_trans(to_source, PJ_FWD, start);
    }

    PJ_COORD moved = proj_trans(transform->transformer, PJ_FWD, start);
    PJ *used = isfinite(moved.xy.x) ? proj_trans_get_last_used_operation(transform->transformer) : NULL;

    if (used) {
      transform->ballpark = proj_coordoperation_has_ballpark_transformation(context, used);
      transform->accuracy = proj_coordoperation_get_accuracy(context, used);
      proj_destroy(used);
    }
    if (to_source) {
      proj_destroy(to_source);
    }
    if (to_geographic) {
      proj_destroy(to_geographic);
    }
    if (probe_source) {
      proj_destroy(probe_source);
    }
  }

  if (!transform->transformer) {
    osmium_transform_free(transform);
    return NULL;
  }
  return transform;
}

void osmium_transform_free(OsmiumTransform *transform) {
  if (!transform) {
    return;
  }
  if (transform->transformer) {
    proj_destroy(transform->transformer);
  }
  free(transform->source);
  free(transform->target);
  free(transform);
}

int osmium_transform_is_ballpark(const OsmiumTransform *transform) {
  return transform->ballpark;
}

double osmium_transform_accuracy(const OsmiumTransform *transform) {
  return transform->accuracy;
}

size_t osmium_transform_apply(const OsmiumTransform *transform, double *xy, size_t n) {
  size_t failed = 0;

  proj_trans_generic(transform->transformer, PJ_FWD, xy, 2 * sizeof(double), n, xy + 1, 2 * sizeof(double), n, NULL,
                     0, 0, NULL, 0, 0);

  for (size_t i = 0; i < n; i++) {
    if (!isfinite(xy[i * 2]) || !isfinite(xy[i * 2 + 1])) {
      xy[i * 2] = xy[i * 2 + 1] = NAN;
      failed++;
    }
  }
  return failed;
}

int osmium_crs_describe_internal(const char *definition, OsmiumCrsInfo *info) {
  char message[64];
  OsmiumProjection *projection = osmium_projection_new(definition, message, sizeof(message));

  memset(info, 0, sizeof(*info));
  if (!projection) {
    return 0;
  }

  info->name = strdup(projection->name);
  info->code = projection->code ? strdup(projection->code) : NULL;
  info->geographic = projection->geographic;
  osmium_projection_unref(projection);
  return 1;
}

void osmium_crs_info_clear(OsmiumCrsInfo *info) {
  free(info->name);
  free(info->code);
  memset(info, 0, sizeof(*info));
}

int osmium_crs_axis_swapped(const char *definition) {
  PJ_CONTEXT *context = context_for_thread();
  PJ *crs = definition ? proj_create(context, definition) : NULL;
  int swapped = 0;

  if (is_valid_crs(crs)) {
    PJ *cs = proj_crs_get_coordinate_system(context, crs);

    if (cs) {
      const char *direction = NULL;

      if (proj_cs_get_axis_info(context, cs, 0, NULL, NULL, &direction, NULL, NULL, NULL, NULL) && direction) {
        swapped = strcmp(direction, "north") == 0;
      }
      proj_destroy(cs);
    }
  }

  if (crs) {
    proj_destroy(crs);
  }
  return swapped;
}
