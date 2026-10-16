/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vector_data.h"
#include "../gdal_setup.h"
#include "vector_edit.h"

#include <cpl_conv.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <curl/curl.h>
#include <gdal.h>
#include <math.h>
#include <unistd.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WEB_MERCATOR_HALF_WORLD 20037508.342789244
#define OSMIUM_VECTOR_MAX_DOWNLOAD (256u * 1024u * 1024u)

typedef struct {
  size_t n_points;
  size_t cap_points;
  double *xy;
  size_t n_parts;
  size_t cap_parts;
  size_t *part_start;
  unsigned char *roles;
} Accumulator;

static int acc_begin_part(Accumulator *acc, unsigned char role) {
  if (acc->n_parts + 2 > acc->cap_parts) {
    size_t cap = acc->cap_parts ? acc->cap_parts * 2 : 8;
    size_t *grown = realloc(acc->part_start, cap * sizeof(size_t));
    unsigned char *roles = realloc(acc->roles, cap);

    if (grown) {
      acc->part_start = grown;
    }
    if (roles) {
      acc->roles = roles;
    }
    if (!grown || !roles) {
      return 0;
    }
    acc->cap_parts = cap;
  }
  acc->roles[acc->n_parts] = role;
  acc->part_start[acc->n_parts++] = acc->n_points;
  return 1;
}

static int acc_add_point(Accumulator *acc, double x, double y) {
  if (!isfinite(x) || !isfinite(y)) {
    return 0;
  }

  if (acc->n_points + 1 > acc->cap_points) {
    size_t cap = acc->cap_points ? acc->cap_points * 2 : 32;
    double *grown = realloc(acc->xy, cap * 2 * sizeof(double));
    if (!grown) {
      return 0;
    }
    acc->xy = grown;
    acc->cap_points = cap;
  }

  acc->xy[acc->n_points * 2] = x;
  acc->xy[acc->n_points * 2 + 1] = y;
  acc->n_points++;
  return 1;
}

static int kind_of(OGRGeometryH geom, OsmiumVectorKind *kind) {
  switch (wkbFlatten(OGR_G_GetGeometryType(geom))) {
  case wkbPoint:
  case wkbMultiPoint:
    *kind = OSMIUM_VECTOR_POINT;
    return 1;
  case wkbLineString:
  case wkbMultiLineString:
    *kind = OSMIUM_VECTOR_LINE;
    return 1;
  case wkbPolygon:
  case wkbMultiPolygon:
    *kind = OSMIUM_VECTOR_POLYGON;
    return 1;
  case wkbGeometryCollection:
    return OGR_G_GetGeometryCount(geom) > 0 && kind_of(OGR_G_GetGeometryRef(geom, 0), kind);
  default:
    return 0;
  }
}

static int collect(OGRGeometryH geom, Accumulator *acc, unsigned char role) {
  switch (wkbFlatten(OGR_G_GetGeometryType(geom))) {
  case wkbPoint:
    return acc_begin_part(acc, 0) && acc_add_point(acc, OGR_G_GetX(geom, 0), OGR_G_GetY(geom, 0));
  case wkbLineString:
  case wkbLinearRing:
    if (!acc_begin_part(acc, role)) {
      return 0;
    }
    for (int i = 0; i < OGR_G_GetPointCount(geom); i++) {
      if (!acc_add_point(acc, OGR_G_GetX(geom, i), OGR_G_GetY(geom, i))) {
        return 0;
      }
    }
    return 1;
  case wkbPolygon:
    for (int i = 0; i < OGR_G_GetGeometryCount(geom); i++) {
      if (!collect(OGR_G_GetGeometryRef(geom, i), acc, i == 0 ? 0 : 1)) {
        return 0;
      }
    }
    return 1;
  case wkbMultiPoint:
  case wkbMultiLineString:
  case wkbMultiPolygon:
  case wkbGeometryCollection:
    for (int i = 0; i < OGR_G_GetGeometryCount(geom); i++) {
      if (!collect(OGR_G_GetGeometryRef(geom, i), acc, 0)) {
        return 0;
      }
    }
    return 1;
  default:
    return 1;
  }
}

static void feature_clear(OsmiumVectorFeature *f) {
  free(f->part_start);
  free(f->part_role);
  free(f->ll);
  free(f->xy);
  memset(f, 0, sizeof(*f));
}

static int feature_from(OGRGeometryH geom, OsmiumVectorFeature *out) {
  Accumulator acc = {0};
  OsmiumVectorKind kind;

  if (OGR_G_IsEmpty(geom) || !kind_of(geom, &kind) || !collect(geom, &acc, 0) || acc.n_points == 0) {
    free(acc.xy);
    free(acc.part_start);
    free(acc.roles);
    return 0;
  }

  acc.part_start[acc.n_parts] = acc.n_points;

  out->kind = kind;
  out->n_parts = acc.n_parts;
  out->part_start = acc.part_start;
  out->part_role = acc.roles;
  out->ll = acc.xy;
  out->xy = NULL;
  return 1;
}

typedef struct {
  char *data;
  size_t len;
} Download;

static size_t download_write(char *chunk, size_t size, size_t nmemb, void *userdata) {
  Download *dl = userdata;
  size_t n = size * nmemb;

  if (n > OSMIUM_VECTOR_MAX_DOWNLOAD - dl->len) {
    return 0;
  }

  char *grown = realloc(dl->data, dl->len + n + 1);
  if (!grown) {
    return 0;
  }

  memcpy(grown + dl->len, chunk, n);
  dl->len += n;
  grown[dl->len] = '\0';
  dl->data = grown;
  return n;
}

static int is_remote(const char *path) {
  return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

static osmium_error download(const char *url, const char *user_agent, Download *dl) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return OSMIUM_NULL_POINTER;
  }

  struct curl_slist *headers =
      curl_slist_append(NULL, "Accept: application/geo+json, application/json;q=0.9, */*;q=0.5");

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, dl);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : "Osmium/0.1");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode result = curl_easy_perform(curl);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return result == CURLE_OK && dl->len > 0 ? OSMIUM_OK : OSMIUM_NETWORK_ERROR;
}

static char *describe_srs(OGRSpatialReferenceH srs) {
  const char *authority = OSRGetAuthorityName(srs, NULL);
  const char *code = OSRGetAuthorityCode(srs, NULL);
  char *wkt = NULL;

  if (authority && code) {
    size_t len = strlen(authority) + strlen(code) + 2;
    char *out = malloc(len);

    if (out) {
      snprintf(out, len, "%s:%s", authority, code);
    }
    return out;
  }

  if (OSRExportToWkt(srs, &wkt) != OGRERR_NONE || !wkt) {
    return NULL;
  }

  char *out = strdup(wkt);

  CPLFree(wkt);
  return out;
}

static size_t point_count(const OsmiumVectorFeature *f) {
  return f->n_parts ? f->part_start[f->n_parts] : 0;
}

static void lonlat_extent(OsmiumVectorData *vec, const OsmiumVectorFeature *f) {
  const size_t n = point_count(f);

  for (size_t i = 0; i < n; i++) {
    const double lon = f->ll[i * 2];
    const double lat = f->ll[i * 2 + 1];

    if (!isfinite(lon) || !isfinite(lat)) {
      continue;
    }
    vec->lon_min = fmin(vec->lon_min, lon);
    vec->lon_max = fmax(vec->lon_max, lon);
    vec->lat_min = fmin(vec->lat_min, lat);
    vec->lat_max = fmax(vec->lat_max, lat);
  }
}

static int source_area(OGRLayerH layer, const char *source, double area[4]) {
  OGREnvelope envelope;
  OsmiumTransform *rough;

  if (OGR_L_GetExtent(layer, &envelope, TRUE) != OGRERR_NONE) {
    return 0;
  }

  rough = osmium_transform_new(source, "EPSG:4326", NULL, NULL, 0);
  if (!rough) {
    return 0;
  }

  double corners[10] = {envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MinY, envelope.MaxX,
                        envelope.MaxY, envelope.MinX, envelope.MaxY,
                        (envelope.MinX + envelope.MaxX) / 2.0, (envelope.MinY + envelope.MaxY) / 2.0};

  osmium_transform_apply(rough, corners, 5);
  osmium_transform_free(rough);

  /* a bogus or assumed coordinate system can put the extent outside the globe */
  for (int i = 0; i < 5; i++) {
    if (!isfinite(corners[i * 2]) || !isfinite(corners[i * 2 + 1]) || fabs(corners[i * 2]) > 180.0 ||
        fabs(corners[i * 2 + 1]) > 90.0) {
      corners[i * 2] = corners[i * 2 + 1] = NAN;
    }
  }

  int found = 0;

  for (int i = 0; i < 5; i++) {
    if (!isfinite(corners[i * 2]) || !isfinite(corners[i * 2 + 1])) {
      continue;
    }
    if (!found) {
      area[0] = area[2] = corners[i * 2];
      area[1] = area[3] = corners[i * 2 + 1];
      found = 1;
    } else {
      area[0] = fmin(area[0], corners[i * 2]);
      area[1] = fmin(area[1], corners[i * 2 + 1]);
      area[2] = fmax(area[2], corners[i * 2]);
      area[3] = fmax(area[3], corners[i * 2 + 1]);
    }
  }
  return found;
}

static osmium_error read_dataset(GDALDatasetH dataset,
                                 const char *source_crs,
                                 const OsmiumProjection *projection,
                                 OsmiumVectorData **out) {
  OGRLayerH layer = GDALDatasetGetLayer(dataset, 0);
  if (!layer) {
    return OSMIUM_FILE_ERROR;
  }

  OsmiumVectorData *vec = calloc(1, sizeof(*vec));
  if (!vec) {
    return OSMIUM_OUT_OF_MEMORY;
  }
  atomic_init(&vec->refs, 1);
  pthread_mutex_init(&vec->lock, NULL);
  pthread_rwlock_init(&vec->xy_lock, NULL);
  vec->lon_min = vec->lat_min = INFINITY;
  vec->lon_max = vec->lat_max = -INFINITY;
  vec->crs_accuracy = -1.0;

  if (source_crs && *source_crs) {
    vec->crs_definition = strdup(source_crs);
  } else {
    OGRSpatialReferenceH layer_srs = OGR_L_GetSpatialRef(layer);

    if (layer_srs) {
      vec->crs_definition = describe_srs(layer_srs);
    } else {
      vec->crs_definition = strdup("EPSG:4326");
      vec->crs_assumed = 1;
    }
  }

  OsmiumCrsInfo info;

  if (!vec->crs_definition || !osmium_crs_describe_internal(vec->crs_definition, &info)) {
    osmium_vector_data_free(vec);
    return OSMIUM_CRS_ERROR;
  }
  vec->crs_name = info.name;
  vec->crs_code = info.code;

  double area[4];
  const int has_area = source_area(layer, vec->crs_definition, area);
  OsmiumTransform *to_geographic = osmium_transform_new(vec->crs_definition, "EPSG:4326", has_area ? area : NULL, NULL, 0);

  if (!to_geographic) {
    osmium_vector_data_free(vec);
    return OSMIUM_CRS_ERROR;
  }
  vec->crs_ballpark = osmium_transform_is_ballpark(to_geographic);
  vec->crs_accuracy = osmium_transform_accuracy(to_geographic);

  switch (wkbFlatten(OGR_L_GetGeomType(layer))) {
  case wkbMultiPoint:
    vec->geometry_multi = 1;
    /* fall through */
  case wkbPoint:
    vec->geometry_kind = OSMIUM_VECTOR_POINT;
    break;
  case wkbMultiLineString:
    vec->geometry_multi = 1;
    /* fall through */
  case wkbLineString:
    vec->geometry_kind = OSMIUM_VECTOR_LINE;
    break;
  case wkbMultiPolygon:
    vec->geometry_multi = 1;
    /* fall through */
  case wkbPolygon:
    vec->geometry_kind = OSMIUM_VECTOR_POLYGON;
    break;
  default:
    vec->geometry_kind = (OsmiumVectorKind)-1;
    break;
  }

  size_t cap = 0;
  osmium_error err = OSMIUM_OK;

  OGR_L_ResetReading(layer);
  OGRFeatureH feature;
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geom = OGR_F_GetGeometryRef(feature);
    OsmiumVectorFeature item = {0};
    item.fid = OGR_F_GetFID(feature);

    if (geom != NULL && feature_from(geom, &item)) {
      const size_t n = point_count(&item);

      if (osmium_transform_apply(to_geographic, item.ll, n) == n) {
        feature_clear(&item);
        OGR_F_Destroy(feature);
        continue;
      }

      if (vec->n_features == cap) {
        cap = cap ? cap * 2 : 256;
        OsmiumVectorFeature *grown = realloc(vec->features, cap * sizeof(*grown));
        if (!grown) {
          feature_clear(&item);
          OGR_F_Destroy(feature);
          err = OSMIUM_OUT_OF_MEMORY;
          break;
        }
        vec->features = grown;
      }

      lonlat_extent(vec, &item);
      vec->features[vec->n_features++] = item;
    }

    OGR_F_Destroy(feature);
  }

  osmium_transform_free(to_geographic);
  vec->cap_features = cap;
  vec->next_temp_fid = -1;
  if ((int)vec->geometry_kind < 0) {
    vec->geometry_kind = vec->n_features > 0 ? vec->features[0].kind : OSMIUM_VECTOR_POLYGON;
  }

  if (err == OSMIUM_OK) {
    err = osmium_vector_data_project(vec, projection);
  }
  if (err != OSMIUM_OK) {
    osmium_vector_data_free(vec);
    return err;
  }

  *out = vec;
  return OSMIUM_OK;
}

osmium_error osmium_vector_data_project(OsmiumVectorData *vec, const OsmiumProjection *projection) {
  if (!vec || !projection) {
    return OSMIUM_NULL_POINTER;
  }

  pthread_rwlock_wrlock(&vec->xy_lock);

  if (vec->projected_definition &&
      strcmp(vec->projected_definition, osmium_projection_get_definition(projection)) == 0) {
    pthread_rwlock_unlock(&vec->xy_lock);
    return OSMIUM_OK;
  }

  vec->min_x = vec->min_y = INFINITY;
  vec->max_x = vec->max_y = -INFINITY;

  for (size_t i = 0; i < vec->n_features; i++) {
    OsmiumVectorFeature *f = &vec->features[i];
    const size_t n = point_count(f);

    if (!f->xy) {
      f->xy = malloc(n * 2 * sizeof(double));
    }
    if (!f->xy) {
      pthread_rwlock_unlock(&vec->xy_lock);
      return OSMIUM_OUT_OF_MEMORY;
    }

    memcpy(f->xy, f->ll, n * 2 * sizeof(double));

    f->min_x = f->min_y = INFINITY;
    f->max_x = f->max_y = -INFINITY;

    if (osmium_projection_forward_many(projection, f->xy, n) > 0) {
      /* a vertex outside the projection's domain: keep the feature out of drawing */
      continue;
    }

    for (size_t p = 0; p < n; p++) {
      f->min_x = fmin(f->min_x, f->xy[p * 2]);
      f->max_x = fmax(f->max_x, f->xy[p * 2]);
      f->min_y = fmin(f->min_y, f->xy[p * 2 + 1]);
      f->max_y = fmax(f->max_y, f->xy[p * 2 + 1]);
    }

    vec->min_x = fmin(vec->min_x, f->min_x);
    vec->max_x = fmax(vec->max_x, f->max_x);
    vec->min_y = fmin(vec->min_y, f->min_y);
    vec->max_y = fmax(vec->max_y, f->max_y);
  }

  free(vec->projected_definition);
  vec->projected_definition = strdup(osmium_projection_get_definition(projection));
  if (vec->projection) {
    osmium_projection_unref(vec->projection);
  }
  vec->projection = osmium_projection_ref((OsmiumProjection *)projection);
  pthread_rwlock_unlock(&vec->xy_lock);
  return OSMIUM_OK;
}

int osmium_vector_data_begin_read(OsmiumVectorData *vec, const OsmiumProjection *projection) {
  for (;;) {
    pthread_rwlock_rdlock(&vec->xy_lock);

    if (vec->projected_definition &&
        strcmp(vec->projected_definition, osmium_projection_get_definition(projection)) == 0) {
      return 1;
    }

    pthread_rwlock_unlock(&vec->xy_lock);
    if (osmium_vector_data_project(vec, projection) != OSMIUM_OK) {
      return 0;
    }
  }
}

int osmium_vector_data_try_begin_read(OsmiumVectorData *vec, const OsmiumProjection *projection) {
  if (pthread_rwlock_tryrdlock(&vec->xy_lock) != 0) {
    return 0;
  }

  if (vec->projected_definition &&
      strcmp(vec->projected_definition, osmium_projection_get_definition(projection)) == 0) {
    return 1;
  }

  pthread_rwlock_unlock(&vec->xy_lock);
  return 0;
}

void osmium_vector_data_end_read(OsmiumVectorData *vec) {
  pthread_rwlock_unlock(&vec->xy_lock);
}

osmium_error osmium_vector_data_open(const char *path,
                                     const char *user_agent,
                                     const char *source_crs,
                                     const OsmiumProjection *projection,
                                     OsmiumVectorData **out) {
  static atomic_uint counter;

  if (!path || !out || !projection) {
    return OSMIUM_NULL_POINTER;
  }
  *out = NULL;

  osmium_gdal_init();

  if (!is_remote(path)) {
    GDALDatasetH dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    if (!dataset) {
      return OSMIUM_FILE_ERROR;
    }

    osmium_error err = read_dataset(dataset, source_crs, projection, out);
    GDALClose(dataset);
    if (err == OSMIUM_OK) {
      (*out)->source_path = strdup(path);
    }
    return err;
  }

  Download dl = {0};
  osmium_error err = download(path, user_agent, &dl);
  if (err != OSMIUM_OK) {
    free(dl.data);
    return err;
  }

  char name[64];
  snprintf(name, sizeof(name), "/vsimem/osmium-%u.json", atomic_fetch_add(&counter, 1u));

  VSILFILE *file = VSIFileFromMemBuffer(name, (GByte *)dl.data, dl.len, FALSE);
  if (!file) {
    free(dl.data);
    return OSMIUM_OUT_OF_MEMORY;
  }
  VSIFCloseL(file);

  GDALDatasetH dataset = GDALOpenEx(name, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
  if (!dataset) {
    err = OSMIUM_FILE_ERROR;
  } else {
    err = read_dataset(dataset, source_crs, projection, out);
    GDALClose(dataset);
  }

  if (err == OSMIUM_OK) {
    (*out)->vsi_name = strdup(name);
    (*out)->buffer = dl.data;
    return OSMIUM_OK;
  }

  VSIUnlink(name);
  free(dl.data);
  return err;
}

OsmiumVectorData *osmium_vector_data_ref(OsmiumVectorData *vec) {
  atomic_fetch_add(&vec->refs, 1);
  return vec;
}

void osmium_vector_data_free(OsmiumVectorData *vec) {
  if (!vec || atomic_fetch_sub(&vec->refs, 1) != 1) {
    return;
  }
  for (size_t i = 0; i < vec->n_features; i++) {
    feature_clear(&vec->features[i]);
  }
  free(vec->features);

  if (vec->dataset) {
    GDALClose(vec->dataset);
  }
  free(vec->projected_definition);
  if (vec->projection) {
    osmium_projection_unref(vec->projection);
  }
  free(vec->crs_definition);
  free(vec->crs_name);
  free(vec->crs_code);
  pthread_rwlock_destroy(&vec->xy_lock);
  if (vec->vsi_name) {
    VSIUnlink(vec->vsi_name);
  }
  free(vec->vsi_name);
  free(vec->buffer);
  free(vec->source_path);
  pthread_mutex_destroy(&vec->lock);
  free(vec);
}

static double segment_distance_squared(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double length_squared = dx * dx + dy * dy;
  double t = length_squared > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / length_squared : 0.0;

  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);

  const double cx = ax + t * dx - px;
  const double cy = ay + t * dy - py;
  return cx * cx + cy * cy;
}

static int feature_hit(const OsmiumVectorFeature *f, double x, double y, double tolerance) {
  const double limit = tolerance * tolerance;
  int inside = 0;

  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];

    if (f->kind == OSMIUM_VECTOR_POINT) {
      const double dx = f->xy[start * 2] - x;
      const double dy = f->xy[start * 2 + 1] - y;

      if (dx * dx + dy * dy <= limit) {
        return 1;
      }
      continue;
    }

    for (size_t i = start; i + 1 < end; i++) {
      const double ax = f->xy[i * 2];
      const double ay = f->xy[i * 2 + 1];
      const double bx = f->xy[(i + 1) * 2];
      const double by = f->xy[(i + 1) * 2 + 1];

      if (segment_distance_squared(x, y, ax, ay, bx, by) <= limit) {
        return 1;
      }

      if (f->kind == OSMIUM_VECTOR_POLYGON && ((ay > y) != (by > y)) &&
          x < (bx - ax) * (y - ay) / (by - ay) + ax) {
        inside = !inside;
      }
    }
  }

  return f->kind == OSMIUM_VECTOR_POLYGON && inside;
}

size_t osmium_vector_data_hit_test(const OsmiumVectorData *vec,
                                   double x,
                                   double y,
                                   double tolerance,
                                   long long *fids,
                                   size_t max_fids) {
  size_t count = 0;

  if (!vec || !fids) {
    return 0;
  }

  /* later features draw on top, so they come first */
  for (size_t i = vec->n_features; i > 0 && count < max_fids; i--) {
    const OsmiumVectorFeature *f = &vec->features[i - 1];

    if (f->deleted || x < f->min_x - tolerance || x > f->max_x + tolerance || y < f->min_y - tolerance ||
        y > f->max_y + tolerance) {
      continue;
    }

    if (feature_hit(f, x, y, tolerance)) {
      fids[count++] = f->fid;
    }
  }

  return count;
}

void osmium_vector_fields_free(OsmiumVectorField *fields, size_t n_fields) {
  if (!fields) {
    return;
  }
  for (size_t i = 0; i < n_fields; i++) {
    free(fields[i].name);
    free(fields[i].value);
  }
  free(fields);
}

static OsmiumFieldType field_type_of(OGRFieldDefnH field_defn) {
  switch (OGR_Fld_GetType(field_defn)) {
  case OFTString:
    return OSMIUM_FIELD_STRING;
  case OFTInteger:
  case OFTInteger64:
    return OSMIUM_FIELD_INTEGER;
  case OFTReal:
    return OSMIUM_FIELD_REAL;
  case OFTDate:
    return OSMIUM_FIELD_DATE;
  default:
    return OSMIUM_FIELD_OTHER;
  }
}

osmium_error osmium_vector_data_get_attributes(OsmiumVectorData *vec,
                                               long long fid,
                                               OsmiumVectorField **fields,
                                               size_t *n_fields) {
  if (!vec || !fields || !n_fields) {
    return OSMIUM_NULL_POINTER;
  }
  *fields = NULL;
  *n_fields = 0;

  pthread_mutex_lock(&vec->lock);

  if (!vec->dataset) {
    const char *path = vec->vsi_name ? vec->vsi_name : vec->source_path;

    if (path) {
      vec->dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    }
  }

  osmium_error err = OSMIUM_FILE_ERROR;
  OGRLayerH layer = vec->dataset ? GDALDatasetGetLayer(vec->dataset, 0) : NULL;
  OGRFeatureH feature = layer ? OGR_L_GetFeature(layer, fid) : NULL;

  if (feature) {
    OGRFeatureDefnH defn = OGR_F_GetDefnRef(feature);
    const int count = OGR_FD_GetFieldCount(defn);
    OsmiumVectorField *out = calloc((size_t)count + 1, sizeof(*out));

    err = OSMIUM_OUT_OF_MEMORY;
    if (out) {
      for (int i = 0; i < count; i++) {
        OGRFieldDefnH field_defn = OGR_FD_GetFieldDefn(defn, i);
        const char *value = OGR_F_IsFieldSetAndNotNull(feature, i) ? OGR_F_GetFieldAsString(feature, i) : "";

        out[i].name = strdup(OGR_Fld_GetNameRef(field_defn));
        out[i].value = strdup(value ? value : "");
        out[i].type = field_type_of(field_defn);
        out[i].width = OGR_Fld_GetWidth(field_defn);

        if (out[i].value && OGR_Fld_GetType(field_defn) == OFTReal &&
            OGR_F_IsFieldSetAndNotNull(feature, i)) {
          char text[64];

          CPLsnprintf(text, sizeof(text), "%.15g", OGR_F_GetFieldAsDouble(feature, i));
          free(out[i].value);
          out[i].value = strdup(text);
        }
      }
      *fields = out;
      *n_fields = (size_t)count;
      err = OSMIUM_OK;
    }
    OGR_F_Destroy(feature);
  }

  pthread_mutex_unlock(&vec->lock);
  return err;
}

osmium_error osmium_vector_data_read_column(OsmiumVectorData *vec,
                                            const char *field,
                                            long long **fids,
                                            char ***values,
                                            size_t *n) {
  if (!vec || !field || !fids || !values || !n) {
    return OSMIUM_NULL_POINTER;
  }
  *fids = NULL;
  *values = NULL;
  *n = 0;

  pthread_mutex_lock(&vec->lock);

  if (!vec->dataset) {
    const char *path = vec->vsi_name ? vec->vsi_name : vec->source_path;

    if (path) {
      vec->dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    }
  }

  OGRLayerH layer = vec->dataset ? GDALDatasetGetLayer(vec->dataset, 0) : NULL;
  osmium_error err = OSMIUM_FILE_ERROR;

  if (layer) {
    OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
    const int index = OGR_FD_GetFieldIndex(defn, field);

    if (index >= 0) {
      size_t capacity = 1024;
      long long *out_fids = malloc(capacity * sizeof(*out_fids));
      char **out_values = malloc(capacity * sizeof(*out_values));
      OGRFeatureH feature;
      const int real = OGR_Fld_GetType(OGR_FD_GetFieldDefn(defn, index)) == OFTReal;

      err = OSMIUM_OUT_OF_MEMORY;
      if (out_fids && out_values) {
        size_t count = 0;

        OGR_L_ResetReading(layer);
        while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
          if (count == capacity) {
            long long *grown_fids = realloc(out_fids, capacity * 2 * sizeof(*out_fids));
            char **grown_values = realloc(out_values, capacity * 2 * sizeof(*out_values));

            if (!grown_fids || !grown_values) {
              out_fids = grown_fids ? grown_fids : out_fids;
              out_values = grown_values ? grown_values : out_values;
              OGR_F_Destroy(feature);
              break;
            }
            out_fids = grown_fids;
            out_values = grown_values;
            capacity *= 2;
          }

          out_fids[count] = OGR_F_GetFID(feature);
          if (!OGR_F_IsFieldSetAndNotNull(feature, index)) {
            out_values[count] = strdup("");
          } else if (real) {
            char text[64];

            CPLsnprintf(text, sizeof(text), "%.15g", OGR_F_GetFieldAsDouble(feature, index));
            out_values[count] = strdup(text);
          } else {
            const char *value = OGR_F_GetFieldAsString(feature, index);

            out_values[count] = strdup(value ? value : "");
          }
          count++;
          OGR_F_Destroy(feature);
        }
        OGR_L_ResetReading(layer);

        *fids = out_fids;
        *values = out_values;
        *n = count;
        err = OSMIUM_OK;
      } else {
        free(out_fids);
        free(out_values);
      }
    }
  }

  pthread_mutex_unlock(&vec->lock);
  return err;
}

osmium_error osmium_vector_data_read_table(OsmiumVectorData *vec,
                                           long long **fids,
                                           char ***cells,
                                           size_t *n_rows,
                                           size_t *n_fields) {
  if (!vec || !fids || !cells || !n_rows || !n_fields) {
    return OSMIUM_NULL_POINTER;
  }
  *fids = NULL;
  *cells = NULL;
  *n_rows = 0;
  *n_fields = 0;

  pthread_mutex_lock(&vec->lock);

  if (!vec->dataset) {
    const char *path = vec->vsi_name ? vec->vsi_name : vec->source_path;

    if (path) {
      vec->dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    }
  }

  OGRLayerH layer = vec->dataset ? GDALDatasetGetLayer(vec->dataset, 0) : NULL;
  osmium_error err = OSMIUM_FILE_ERROR;

  if (layer) {
    OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
    const size_t columns = (size_t)OGR_FD_GetFieldCount(defn);
    size_t capacity = 1024;
    size_t count = 0;
    long long *out_fids = malloc(capacity * sizeof(*out_fids));
    char **out_cells = malloc(capacity * (columns ? columns : 1) * sizeof(*out_cells));
    OGRFeatureH feature;

    err = OSMIUM_OUT_OF_MEMORY;
    if (out_fids && out_cells) {
      OGR_L_ResetReading(layer);
      while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
        if (count == capacity) {
          long long *grown_fids = realloc(out_fids, capacity * 2 * sizeof(*out_fids));
          char **grown_cells = realloc(out_cells, capacity * 2 * (columns ? columns : 1) * sizeof(*out_cells));

          if (grown_fids) {
            out_fids = grown_fids;
          }
          if (grown_cells) {
            out_cells = grown_cells;
          }
          if (!grown_fids || !grown_cells) {
            OGR_F_Destroy(feature);
            break;
          }
          capacity *= 2;
        }

        out_fids[count] = OGR_F_GetFID(feature);
        for (size_t i = 0; i < columns; i++) {
          OGRFieldDefnH field_defn = OGR_FD_GetFieldDefn(defn, (int)i);
          char *value;

          if (!OGR_F_IsFieldSetAndNotNull(feature, (int)i)) {
            value = strdup("");
          } else if (OGR_Fld_GetType(field_defn) == OFTReal) {
            char text[64];

            CPLsnprintf(text, sizeof(text), "%.15g", OGR_F_GetFieldAsDouble(feature, (int)i));
            value = strdup(text);
          } else {
            const char *text = OGR_F_GetFieldAsString(feature, (int)i);

            value = strdup(text ? text : "");
          }
          out_cells[count * columns + i] = value;
        }
        count++;
        OGR_F_Destroy(feature);
      }
      OGR_L_ResetReading(layer);

      *fids = out_fids;
      *cells = out_cells;
      *n_rows = count;
      *n_fields = columns;
      err = OSMIUM_OK;
    } else {
      free(out_fids);
      free(out_cells);
    }
  }

  pthread_mutex_unlock(&vec->lock);
  return err;
}

static void set_message(char *message, size_t size, const char *format, const char *a, const char *b) {
  if (message && size > 0) {
    snprintf(message, size, format, a ? a : "", b ? b : "");
  }
}

int osmium_vector_data_is_editable(const OsmiumVectorData *vec) {
  return vec && vec->source_path && access(vec->source_path, W_OK) == 0;
}

void osmium_vector_data_release(OsmiumVectorData *vec) {
  if (!vec) {
    return;
  }

  pthread_mutex_lock(&vec->lock);
  if (vec->dataset) {
    GDALClose(vec->dataset);
    vec->dataset = NULL;
  }
  pthread_mutex_unlock(&vec->lock);
}

osmium_error osmium_vector_data_get_fields(OsmiumVectorData *vec,
                                           OsmiumVectorField **fields,
                                           size_t *n_fields) {
  if (!vec || !fields || !n_fields) {
    return OSMIUM_NULL_POINTER;
  }
  *fields = NULL;
  *n_fields = 0;

  pthread_mutex_lock(&vec->lock);

  if (!vec->dataset) {
    const char *path = vec->vsi_name ? vec->vsi_name : vec->source_path;

    if (path) {
      vec->dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    }
  }

  osmium_error err = OSMIUM_FILE_ERROR;
  OGRLayerH layer = vec->dataset ? GDALDatasetGetLayer(vec->dataset, 0) : NULL;

  if (layer) {
    OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
    const int count = OGR_FD_GetFieldCount(defn);
    OsmiumVectorField *out = calloc((size_t)count + 1, sizeof(*out));

    err = OSMIUM_OUT_OF_MEMORY;
    if (out) {
      for (int i = 0; i < count; i++) {
        OGRFieldDefnH field_defn = OGR_FD_GetFieldDefn(defn, i);

        out[i].name = strdup(OGR_Fld_GetNameRef(field_defn));
        out[i].value = strdup("");
        out[i].type = field_type_of(field_defn);
        out[i].width = OGR_Fld_GetWidth(field_defn);
      }
      *fields = out;
      *n_fields = (size_t)count;
      err = OSMIUM_OK;
    }
  }

  pthread_mutex_unlock(&vec->lock);
  return err;
}

static GDALDatasetH open_for_update(const char *path, OGRLayerH *layer, char *message, size_t size) {
  osmium_gdal_init();

  GDALDatasetH dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_UPDATE, NULL, NULL, NULL);

  if (!dataset) {
    set_message(message, size, "The file can't be opened for writing%s%s", "", "");
    return NULL;
  }

  *layer = GDALDatasetGetLayer(dataset, 0);
  if (!*layer) {
    GDALClose(dataset);
    set_message(message, size, "The file has no layer%s%s", "", "");
    return NULL;
  }
  return dataset;
}

static int is_shapefile(GDALDatasetH dataset) {
  GDALDriverH driver = GDALGetDatasetDriver(dataset);

  return driver && strcmp(GDALGetDriverShortName(driver), "ESRI Shapefile") == 0;
}

static int parse_date(const char *value) {
  int y, m, d;
  char tail;

  return sscanf(value, "%4d-%2d-%2d%c", &y, &m, &d, &tail) == 3 && m >= 1 && m <= 12 && d >= 1 &&
         d <= 31;
}

static int check_value(OGRFieldDefnH field_defn, const char *name, const char *value, char *message, size_t size) {
  if (*value == '\0') {
    return 1;
  }

  switch (OGR_Fld_GetType(field_defn)) {
  case OFTInteger:
  case OFTInteger64: {
    char *end = NULL;

    strtoll(value, &end, 10);
    if (*end != '\0') {
      set_message(message, size, "%s must be a whole number", name, NULL);
      return 0;
    }
    return 1;
  }
  case OFTReal: {
    char *end = NULL;

    CPLStrtod(value, &end);
    if (*end != '\0') {
      set_message(message, size, "%s must be a number", name, NULL);
      return 0;
    }
    return 1;
  }
  case OFTDate:
    if (!parse_date(value)) {
      set_message(message, size, "%s must be a date like 2026-12-31", name, NULL);
      return 0;
    }
    return 1;
  case OFTString: {
    const int width = OGR_Fld_GetWidth(field_defn);

    if (width > 0 && strlen(value) > (size_t)width) {
      char limit[16];

      snprintf(limit, sizeof(limit), "%d", width);
      set_message(message, size, "%s is too long (at most %s bytes)", name, limit);
      return 0;
    }
    return 1;
  }
  default:
    set_message(message, size, "%s can't be edited", name, NULL);
    return 0;
  }
}

static void store_value(OGRFeatureH feature, int index, OGRFieldDefnH field_defn, const char *value) {
  if (*value == '\0' && OGR_Fld_GetType(field_defn) != OFTString) {
    OGR_F_SetFieldNull(feature, index);
    return;
  }

  switch (OGR_Fld_GetType(field_defn)) {
  case OFTInteger:
  case OFTInteger64:
    OGR_F_SetFieldInteger64(feature, index, strtoll(value, NULL, 10));
    break;
  case OFTReal:
    OGR_F_SetFieldDouble(feature, index, CPLAtof(value));
    break;
  default:
    OGR_F_SetFieldString(feature, index, value);
    break;
  }
}

osmium_error osmium_vector_write_attributes(const char *path,
                                            const OsmiumVectorEdit *edits,
                                            size_t n_edits,
                                            char *message,
                                            size_t message_size) {
  OGRLayerH layer = NULL;

  if (!path || (!edits && n_edits > 0)) {
    return OSMIUM_NULL_POINTER;
  }

  GDALDatasetH dataset = open_for_update(path, &layer, message, message_size);
  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);

  for (size_t i = 0; i < n_edits; i++) {
    const int index = OGR_FD_GetFieldIndex(defn, edits[i].field);

    if (index < 0) {
      set_message(message, message_size, "Unknown field %s", edits[i].field, NULL);
      GDALClose(dataset);
      return OSMIUM_FILE_ERROR;
    }
    if (!check_value(OGR_FD_GetFieldDefn(defn, index), edits[i].field, edits[i].value, message, message_size)) {
      GDALClose(dataset);
      return OSMIUM_FILE_ERROR;
    }
  }

  osmium_error err = OSMIUM_OK;
  const int transaction = OGR_L_StartTransaction(layer) == OGRERR_NONE;

  for (size_t i = 0; i < n_edits && err == OSMIUM_OK; i++) {
    OGRFeatureH feature = OGR_L_GetFeature(layer, edits[i].fid);
    const int index = OGR_FD_GetFieldIndex(defn, edits[i].field);

    if (!feature) {
      set_message(message, message_size, "A selected feature no longer exists in the file%s%s", "", "");
      err = OSMIUM_FILE_ERROR;
      break;
    }

    store_value(feature, index, OGR_FD_GetFieldDefn(defn, index), edits[i].value);

    if (OGR_L_SetFeature(layer, feature) != OGRERR_NONE) {
      set_message(message, message_size, "Could not write the change: %s%s", CPLGetLastErrorMsg(), "");
      err = OSMIUM_FILE_ERROR;
    }
    OGR_F_Destroy(feature);
  }

  if (transaction) {
    if (err == OSMIUM_OK) {
      if (OGR_L_CommitTransaction(layer) != OGRERR_NONE) {
        set_message(message, message_size, "Could not commit the changes: %s%s", CPLGetLastErrorMsg(), "");
        err = OSMIUM_FILE_ERROR;
      }
    } else {
      OGR_L_RollbackTransaction(layer);
    }
  }

  OGR_L_SyncToDisk(layer);
  GDALClose(dataset);
  return err;
}

static int valid_field_name(GDALDatasetH dataset, const char *name, char *message, size_t size) {
  if (!name || *name == '\0') {
    set_message(message, size, "The field name can't be empty%s%s", "", "");
    return 0;
  }

  if (is_shapefile(dataset)) {
    if (strlen(name) > 10) {
      set_message(message, size, "Shapefile field names have at most 10 characters%s%s", "", "");
      return 0;
    }
    for (const char *p = name; *p; p++) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
        set_message(message, size, "Use only letters, digits and underscores in shapefile field names%s%s", "", "");
        return 0;
      }
    }
  }
  return 1;
}

osmium_error osmium_vector_add_field(const char *path,
                                     const char *name,
                                     OsmiumFieldType type,
                                     char *message,
                                     size_t message_size) {
  OGRLayerH layer = NULL;
  GDALDatasetH dataset = open_for_update(path, &layer, message, message_size);

  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  if (!valid_field_name(dataset, name, message, message_size)) {
    GDALClose(dataset);
    return OSMIUM_FILE_ERROR;
  }

  if (OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), name) >= 0) {
    set_message(message, message_size, "A field named %s already exists", name, NULL);
    GDALClose(dataset);
    return OSMIUM_FILE_ERROR;
  }

  OGRFieldType ogr_type = type == OSMIUM_FIELD_INTEGER ? OFTInteger64
                          : type == OSMIUM_FIELD_REAL  ? OFTReal
                          : type == OSMIUM_FIELD_DATE  ? OFTDate
                                                       : OFTString;
  OGRFieldDefnH field_defn = OGR_Fld_Create(name, ogr_type);

  if (ogr_type == OFTString) {
    OGR_Fld_SetWidth(field_defn, 254);
  } else if (ogr_type == OFTInteger64) {
    OGR_Fld_SetWidth(field_defn, 18);
  } else if (ogr_type == OFTReal) {
    OGR_Fld_SetWidth(field_defn, 24);
    OGR_Fld_SetPrecision(field_defn, 15);
  }

  osmium_error err = OSMIUM_OK;

  if (OGR_L_CreateField(layer, field_defn, TRUE) != OGRERR_NONE) {
    set_message(message, message_size, "Could not add the field: %s%s", CPLGetLastErrorMsg(), "");
    err = OSMIUM_FILE_ERROR;
  }

  OGR_Fld_Destroy(field_defn);
  OGR_L_SyncToDisk(layer);
  GDALClose(dataset);
  return err;
}

osmium_error osmium_vector_rename_field(const char *path,
                                        const char *old_name,
                                        const char *new_name,
                                        char *message,
                                        size_t message_size) {
  OGRLayerH layer = NULL;
  GDALDatasetH dataset = open_for_update(path, &layer, message, message_size);

  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
  const int index = OGR_FD_GetFieldIndex(defn, old_name);

  if (index < 0 || !valid_field_name(dataset, new_name, message, message_size)) {
    if (index < 0) {
      set_message(message, message_size, "Unknown field %s", old_name, NULL);
    }
    GDALClose(dataset);
    return OSMIUM_FILE_ERROR;
  }

  if (strcmp(old_name, new_name) != 0 && OGR_FD_GetFieldIndex(defn, new_name) >= 0) {
    set_message(message, message_size, "A field named %s already exists", new_name, NULL);
    GDALClose(dataset);
    return OSMIUM_FILE_ERROR;
  }

  OGRFieldDefnH field_defn = OGR_Fld_Create(new_name, OGR_Fld_GetType(OGR_FD_GetFieldDefn(defn, index)));
  osmium_error err = OSMIUM_OK;

  if (OGR_L_AlterFieldDefn(layer, index, field_defn, ALTER_NAME_FLAG) != OGRERR_NONE) {
    set_message(message, message_size, "Could not rename the field: %s%s", CPLGetLastErrorMsg(), "");
    err = OSMIUM_FILE_ERROR;
  }

  OGR_Fld_Destroy(field_defn);
  OGR_L_SyncToDisk(layer);
  GDALClose(dataset);
  return err;
}

osmium_error osmium_vector_delete_field(const char *path,
                                        const char *name,
                                        char *message,
                                        size_t message_size) {
  OGRLayerH layer = NULL;
  GDALDatasetH dataset = open_for_update(path, &layer, message, message_size);

  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  const int index = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), name);
  osmium_error err = OSMIUM_OK;

  if (index < 0) {
    set_message(message, message_size, "Unknown field %s", name, NULL);
    err = OSMIUM_FILE_ERROR;
  } else if (OGR_L_DeleteField(layer, index) != OGRERR_NONE) {
    set_message(message, message_size, "Could not delete the field: %s%s", CPLGetLastErrorMsg(), "");
    err = OSMIUM_FILE_ERROR;
  }

  OGR_L_SyncToDisk(layer);
  GDALClose(dataset);
  return err;
}

void osmium_geometry_clear(OsmiumGeometry *geometry) {
  if (!geometry) {
    return;
  }
  free(geometry->ll);
  free(geometry->part_start);
  free(geometry->part_role);
  memset(geometry, 0, sizeof(*geometry));
}

int osmium_geometry_copy(const OsmiumGeometry *from, OsmiumGeometry *to) {
  memset(to, 0, sizeof(*to));
  to->kind = from->kind;
  to->n_points = from->n_points;
  to->n_parts = from->n_parts;
  to->ll = malloc((from->n_points ? from->n_points : 1) * 2 * sizeof(double));
  to->part_start = malloc((from->n_parts + 1) * sizeof(size_t));
  to->part_role = malloc(from->n_parts ? from->n_parts : 1);

  if (!to->ll || !to->part_start || !to->part_role) {
    osmium_geometry_clear(to);
    return 0;
  }

  memcpy(to->ll, from->ll, from->n_points * 2 * sizeof(double));
  memcpy(to->part_start, from->part_start, (from->n_parts + 1) * sizeof(size_t));
  memcpy(to->part_role, from->part_role, from->n_parts);
  return 1;
}

ptrdiff_t osmium_vector_data_find_fid(OsmiumVectorData *vec, long long fid) {
  for (size_t i = 0; i < vec->n_features; i++) {
    if (vec->features[i].fid == fid) {
      return (ptrdiff_t)i;
    }
  }
  return -1;
}

long long osmium_vector_data_new_fid(OsmiumVectorData *vec) {
  return vec->next_temp_fid--;
}

int osmium_vector_data_copy_geometry(OsmiumVectorData *vec, size_t index, OsmiumGeometry *out) {
  if (index >= vec->n_features) {
    return 0;
  }

  const OsmiumVectorFeature *f = &vec->features[index];
  OsmiumGeometry view = {.kind = f->kind,
                         .ll = f->ll,
                         .n_points = point_count(f),
                         .part_start = f->part_start,
                         .part_role = f->part_role,
                         .n_parts = f->n_parts};

  return osmium_geometry_copy(&view, out);
}

/* caller holds the write lock */
static int store_geometry(OsmiumVectorData *vec, OsmiumVectorFeature *f, const OsmiumGeometry *g) {
  double *ll = malloc((g->n_points ? g->n_points : 1) * 2 * sizeof(double));
  double *xy = malloc((g->n_points ? g->n_points : 1) * 2 * sizeof(double));
  size_t *part_start = malloc((g->n_parts + 1) * sizeof(size_t));
  unsigned char *part_role = malloc(g->n_parts ? g->n_parts : 1);

  if (!ll || !xy || !part_start || !part_role) {
    free(ll);
    free(xy);
    free(part_start);
    free(part_role);
    return 0;
  }

  memcpy(ll, g->ll, g->n_points * 2 * sizeof(double));
  memcpy(xy, g->ll, g->n_points * 2 * sizeof(double));
  memcpy(part_start, g->part_start, (g->n_parts + 1) * sizeof(size_t));
  memcpy(part_role, g->part_role, g->n_parts);

  free(f->ll);
  free(f->xy);
  free(f->part_start);
  free(f->part_role);
  f->ll = ll;
  f->xy = xy;
  f->part_start = part_start;
  f->part_role = part_role;
  f->n_parts = g->n_parts;
  f->kind = g->kind;

  f->min_x = f->min_y = INFINITY;
  f->max_x = f->max_y = -INFINITY;

  if (vec->projection && osmium_projection_forward_many(vec->projection, f->xy, g->n_points) == 0) {
    for (size_t i = 0; i < g->n_points; i++) {
      f->min_x = fmin(f->min_x, f->xy[i * 2]);
      f->max_x = fmax(f->max_x, f->xy[i * 2]);
      f->min_y = fmin(f->min_y, f->xy[i * 2 + 1]);
      f->max_y = fmax(f->max_y, f->xy[i * 2 + 1]);
    }
    vec->min_x = fmin(vec->min_x, f->min_x);
    vec->max_x = fmax(vec->max_x, f->max_x);
    vec->min_y = fmin(vec->min_y, f->min_y);
    vec->max_y = fmax(vec->max_y, f->max_y);
  }

  for (size_t i = 0; i < g->n_points; i++) {
    vec->lon_min = fmin(vec->lon_min, g->ll[i * 2]);
    vec->lon_max = fmax(vec->lon_max, g->ll[i * 2]);
    vec->lat_min = fmin(vec->lat_min, g->ll[i * 2 + 1]);
    vec->lat_max = fmax(vec->lat_max, g->ll[i * 2 + 1]);
  }
  return 1;
}

int osmium_vector_data_set_geometry(OsmiumVectorData *vec, size_t index, const OsmiumGeometry *geometry) {
  int ok = 0;

  pthread_rwlock_wrlock(&vec->xy_lock);
  if (index < vec->n_features) {
    ok = store_geometry(vec, &vec->features[index], geometry);
  }
  pthread_rwlock_unlock(&vec->xy_lock);
  return ok;
}

ptrdiff_t osmium_vector_data_append_feature(OsmiumVectorData *vec, long long fid, const OsmiumGeometry *geometry) {
  ptrdiff_t index = -1;

  pthread_rwlock_wrlock(&vec->xy_lock);

  if (vec->n_features == vec->cap_features) {
    size_t cap = vec->cap_features ? vec->cap_features * 2 : 16;
    OsmiumVectorFeature *grown = realloc(vec->features, cap * sizeof(*grown));

    if (grown) {
      vec->features = grown;
      vec->cap_features = cap;
    }
  }

  if (vec->n_features < vec->cap_features) {
    OsmiumVectorFeature *f = &vec->features[vec->n_features];

    memset(f, 0, sizeof(*f));
    f->fid = fid;
    f->state = OSMIUM_FEATURE_NEW;
    if (store_geometry(vec, f, geometry)) {
      index = (ptrdiff_t)vec->n_features++;
    } else {
      feature_clear(f);
    }
  }

  pthread_rwlock_unlock(&vec->xy_lock);
  return index;
}

void osmium_vector_data_set_deleted(OsmiumVectorData *vec, size_t index, int deleted) {
  pthread_rwlock_wrlock(&vec->xy_lock);
  if (index < vec->n_features) {
    vec->features[index].deleted = deleted;
  }
  pthread_rwlock_unlock(&vec->xy_lock);
}

void osmium_vector_data_set_state(OsmiumVectorData *vec, size_t index, int state) {
  pthread_rwlock_wrlock(&vec->xy_lock);
  if (index < vec->n_features) {
    vec->features[index].state = state;
  }
  pthread_rwlock_unlock(&vec->xy_lock);
}

static OGRGeometryH make_point(double x, double y) {
  OGRGeometryH point = OGR_G_CreateGeometry(wkbPoint);

  OGR_G_SetPoint_2D(point, 0, x, y);
  return point;
}

OGRGeometryH osmium_vector_build_geometry(const OsmiumVectorData *vec,
                                          const OsmiumVectorFeature *f,
                                          OsmiumTransform *transform) {
  const size_t n = point_count(f);
  double *xy = malloc((n ? n : 1) * 2 * sizeof(double));
  OGRGeometryH result = NULL;

  if (!xy) {
    return NULL;
  }
  memcpy(xy, f->ll, n * 2 * sizeof(double));

  if (transform && osmium_transform_apply(transform, xy, n) > 0) {
    free(xy);
    return NULL;
  }

  if (f->kind == OSMIUM_VECTOR_POINT) {
    if (f->n_parts == 1 && !vec->geometry_multi) {
      result = make_point(xy[0], xy[1]);
    } else {
      result = OGR_G_CreateGeometry(wkbMultiPoint);
      for (size_t p = 0; p < f->n_parts; p++) {
        OGR_G_AddGeometryDirectly(result, make_point(xy[f->part_start[p] * 2], xy[f->part_start[p] * 2 + 1]));
      }
    }
  } else if (f->kind == OSMIUM_VECTOR_LINE) {
    OGRGeometryH collection = OGR_G_CreateGeometry(wkbMultiLineString);

    for (size_t p = 0; p < f->n_parts; p++) {
      OGRGeometryH line = OGR_G_CreateGeometry(wkbLineString);

      for (size_t i = f->part_start[p]; i < f->part_start[p + 1]; i++) {
        OGR_G_AddPoint_2D(line, xy[i * 2], xy[i * 2 + 1]);
      }
      OGR_G_AddGeometryDirectly(collection, line);
    }

    if (f->n_parts == 1 && !vec->geometry_multi) {
      result = OGR_G_Clone(OGR_G_GetGeometryRef(collection, 0));
      OGR_G_DestroyGeometry(collection);
    } else {
      result = collection;
    }
  } else {
    OGRGeometryH multi = OGR_G_CreateGeometry(wkbMultiPolygon);
    OGRGeometryH polygon = NULL;
    int polygons = 0;

    for (size_t p = 0; p < f->n_parts; p++) {
      OGRGeometryH ring = OGR_G_CreateGeometry(wkbLinearRing);

      if (f->part_role[p] == 0 || polygon == NULL) {
        if (polygon) {
          OGR_G_AddGeometryDirectly(multi, polygon);
        }
        polygon = OGR_G_CreateGeometry(wkbPolygon);
        polygons++;
      }

      for (size_t i = f->part_start[p]; i < f->part_start[p + 1]; i++) {
        OGR_G_AddPoint_2D(ring, xy[i * 2], xy[i * 2 + 1]);
      }
      OGR_G_CloseRings(ring);
      OGR_G_AddGeometryDirectly(polygon, ring);
    }
    if (polygon) {
      OGR_G_AddGeometryDirectly(multi, polygon);
    }

    if (polygons == 1 && !vec->geometry_multi) {
      result = OGR_G_Clone(OGR_G_GetGeometryRef(multi, 0));
      OGR_G_DestroyGeometry(multi);
    } else {
      result = multi;
    }
  }

  free(xy);
  return result;
}

static int apply_overrides(const OsmiumEditSession *session,
                           OGRFeatureH feature,
                           OGRFeatureDefnH defn,
                           long long fid,
                           char *message,
                           size_t message_size) {
  for (size_t i = 0; i < session->n_overrides; i++) {
    const OsmiumAttributeOverride *entry = &session->overrides[i];
    const int index = entry->fid == fid ? OGR_FD_GetFieldIndex(defn, entry->field) : -1;

    if (index < 0) {
      continue;
    }
    if (!check_value(OGR_FD_GetFieldDefn(defn, index), entry->field, entry->value, message, message_size)) {
      return 0;
    }
    store_value(feature, index, OGR_FD_GetFieldDefn(defn, index), entry->value);
  }
  return 1;
}

osmium_error osmium_edit_save(OsmiumEditSession *session, char *message, size_t message_size) {
  OsmiumVectorData *vec = session->data;
  OGRLayerH layer = NULL;
  osmium_error err = OSMIUM_OK;
  int deleted_any = 0;

  if (!vec->source_path) {
    set_message(message, message_size, "This layer has no file to write to%s%s", "", "");
    return OSMIUM_FILE_ERROR;
  }

  osmium_vector_data_release(vec);
  GDALDatasetH dataset = open_for_update(vec->source_path, &layer, message, message_size);
  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
  const double area[4] = {vec->lon_min, vec->lat_min, vec->lon_max, vec->lat_max};
  OsmiumTransform *transform = osmium_transform_new("EPSG:4326", vec->crs_definition, isfinite(area[0]) ? area : NULL, NULL, 0);

  if (!transform) {
    GDALClose(dataset);
    set_message(message, message_size, "No transformation back to the file's coordinate system%s%s", "", "");
    return OSMIUM_CRS_ERROR;
  }

  const int transaction = OGR_L_StartTransaction(layer) == OGRERR_NONE;

  for (size_t i = 0; i < vec->n_features && err == OSMIUM_OK; i++) {
    OsmiumVectorFeature *f = &vec->features[i];
    int has_override = 0;

    for (size_t o = 0; o < session->n_overrides; o++) {
      has_override |= session->overrides[o].fid == f->fid;
    }

    if (f->deleted) {
      if (f->fid >= 0 && OGR_L_DeleteFeature(layer, f->fid) != OGRERR_NONE) {
        set_message(message, message_size, "Could not delete a feature: %s%s", CPLGetLastErrorMsg(), "");
        err = OSMIUM_FILE_ERROR;
      } else if (f->fid >= 0) {
        deleted_any = 1;
      }
      continue;
    }

    if (f->state == OSMIUM_FEATURE_CLEAN && !has_override) {
      continue;
    }

    OGRFeatureH feature = f->state == OSMIUM_FEATURE_NEW ? OGR_F_Create(defn) : OGR_L_GetFeature(layer, f->fid);

    if (!feature) {
      set_message(message, message_size, "A feature no longer exists in the file%s%s", "", "");
      err = OSMIUM_FILE_ERROR;
      break;
    }

    if (f->state != OSMIUM_FEATURE_CLEAN) {
      OGRGeometryH geometry = osmium_vector_build_geometry(vec, f, transform);

      if (!geometry) {
        set_message(message, message_size, "A geometry can't be converted back to the file's coordinate system%s%s", "", "");
        OGR_F_Destroy(feature);
        err = OSMIUM_CRS_ERROR;
        break;
      }
      OGR_F_SetGeometryDirectly(feature, geometry);
    }

    if (!apply_overrides(session, feature, defn, f->fid, message, message_size)) {
      OGR_F_Destroy(feature);
      err = OSMIUM_FILE_ERROR;
      break;
    }

    const OGRErr write = f->state == OSMIUM_FEATURE_NEW ? OGR_L_CreateFeature(layer, feature)
                                                          : OGR_L_SetFeature(layer, feature);

    if (write != OGRERR_NONE) {
      set_message(message, message_size, "Could not write a feature: %s%s", CPLGetLastErrorMsg(), "");
      err = OSMIUM_FILE_ERROR;
    }
    OGR_F_Destroy(feature);
  }

  osmium_transform_free(transform);

  if (transaction) {
    if (err == OSMIUM_OK) {
      if (OGR_L_CommitTransaction(layer) != OGRERR_NONE) {
        set_message(message, message_size, "Could not commit the changes: %s%s", CPLGetLastErrorMsg(), "");
        err = OSMIUM_FILE_ERROR;
      }
    } else {
      OGR_L_RollbackTransaction(layer);
    }
  }

  if (err == OSMIUM_OK && deleted_any && is_shapefile(dataset)) {
    char sql[256];

    snprintf(sql, sizeof(sql), "REPACK %s", OGR_L_GetName(layer));
    GDALDatasetExecuteSQL(dataset, sql, NULL, NULL);
  }

  OGR_L_SyncToDisk(layer);
  GDALClose(dataset);

  if (err == OSMIUM_OK) {
    osmium_edit_session_clear(session);
  }
  return err;
}
