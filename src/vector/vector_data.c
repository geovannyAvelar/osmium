/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vector_data.h"

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
} Accumulator;

static pthread_once_t gdal_once = PTHREAD_ONCE_INIT;

static void gdal_init(void) {
  GDALAllRegister();
}

static int acc_begin_part(Accumulator *acc) {
  if (acc->n_parts + 2 > acc->cap_parts) {
    size_t cap = acc->cap_parts ? acc->cap_parts * 2 : 8;
    size_t *grown = realloc(acc->part_start, cap * sizeof(size_t));
    if (!grown) {
      return 0;
    }
    acc->part_start = grown;
    acc->cap_parts = cap;
  }
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

  acc->xy[acc->n_points * 2] = (x + WEB_MERCATOR_HALF_WORLD) / (2.0 * WEB_MERCATOR_HALF_WORLD);
  acc->xy[acc->n_points * 2 + 1] = (WEB_MERCATOR_HALF_WORLD - y) / (2.0 * WEB_MERCATOR_HALF_WORLD);
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

static int collect(OGRGeometryH geom, Accumulator *acc) {
  switch (wkbFlatten(OGR_G_GetGeometryType(geom))) {
  case wkbPoint:
    return acc_begin_part(acc) && acc_add_point(acc, OGR_G_GetX(geom, 0), OGR_G_GetY(geom, 0));
  case wkbLineString:
  case wkbLinearRing:
    if (!acc_begin_part(acc)) {
      return 0;
    }
    for (int i = 0; i < OGR_G_GetPointCount(geom); i++) {
      if (!acc_add_point(acc, OGR_G_GetX(geom, i), OGR_G_GetY(geom, i))) {
        return 0;
      }
    }
    return 1;
  case wkbPolygon:
  case wkbMultiPoint:
  case wkbMultiLineString:
  case wkbMultiPolygon:
  case wkbGeometryCollection:
    for (int i = 0; i < OGR_G_GetGeometryCount(geom); i++) {
      if (!collect(OGR_G_GetGeometryRef(geom, i), acc)) {
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
  free(f->xy);
  memset(f, 0, sizeof(*f));
}

static int feature_from(OGRGeometryH geom, OsmiumVectorFeature *out) {
  Accumulator acc = {0};
  OsmiumVectorKind kind;

  if (OGR_G_IsEmpty(geom) || !kind_of(geom, &kind) || !collect(geom, &acc) || acc.n_points == 0) {
    free(acc.xy);
    free(acc.part_start);
    return 0;
  }

  acc.part_start[acc.n_parts] = acc.n_points;

  out->kind = kind;
  out->n_parts = acc.n_parts;
  out->part_start = acc.part_start;
  out->xy = acc.xy;
  out->min_x = out->max_x = acc.xy[0];
  out->min_y = out->max_y = acc.xy[1];
  for (size_t i = 1; i < acc.n_points; i++) {
    const double x = acc.xy[i * 2];
    const double y = acc.xy[i * 2 + 1];
    out->min_x = fmin(out->min_x, x);
    out->max_x = fmax(out->max_x, x);
    out->min_y = fmin(out->min_y, y);
    out->max_y = fmax(out->max_y, y);
  }
  return 1;
}

static OGRCoordinateTransformationH make_transform(OGRLayerH layer) {
  OGRSpatialReferenceH source = NULL;
  OGRSpatialReferenceH target = OSRNewSpatialReference(NULL);
  OGRSpatialReferenceH layer_srs = OGR_L_GetSpatialRef(layer);

  if (layer_srs != NULL) {
    source = OSRClone(layer_srs);
  } else {
    source = OSRNewSpatialReference(NULL);
    OSRImportFromEPSG(source, 4326);
  }
  OSRImportFromEPSG(target, 3857);

  OSRSetAxisMappingStrategy(source, OAMS_TRADITIONAL_GIS_ORDER);
  OSRSetAxisMappingStrategy(target, OAMS_TRADITIONAL_GIS_ORDER);

  OGRCoordinateTransformationH ct = OCTNewCoordinateTransformation(source, target);

  OSRDestroySpatialReference(source);
  OSRDestroySpatialReference(target);
  return ct;
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

static osmium_error read_dataset(GDALDatasetH dataset, OsmiumVectorData **out) {
  OGRLayerH layer = GDALDatasetGetLayer(dataset, 0);
  if (!layer) {
    return OSMIUM_FILE_ERROR;
  }

  OGRCoordinateTransformationH ct = make_transform(layer);
  if (!ct) {
    return OSMIUM_CRS_ERROR;
  }

  OsmiumVectorData *vec = calloc(1, sizeof(*vec));
  if (!vec) {
    OCTDestroyCoordinateTransformation(ct);
    return OSMIUM_OUT_OF_MEMORY;
  }
  atomic_init(&vec->refs, 1);
  pthread_mutex_init(&vec->lock, NULL);

  size_t cap = 0;
  osmium_error err = OSMIUM_OK;

  OGR_L_ResetReading(layer);
  OGRFeatureH feature;
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geom = OGR_F_GetGeometryRef(feature);
    OsmiumVectorFeature item = {0};
    item.fid = OGR_F_GetFID(feature);

    if (geom != NULL && OGR_G_Transform(geom, ct) == OGRERR_NONE && feature_from(geom, &item)) {
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

      if (vec->n_features == 0) {
        vec->min_x = item.min_x;
        vec->min_y = item.min_y;
        vec->max_x = item.max_x;
        vec->max_y = item.max_y;
      } else {
        vec->min_x = fmin(vec->min_x, item.min_x);
        vec->min_y = fmin(vec->min_y, item.min_y);
        vec->max_x = fmax(vec->max_x, item.max_x);
        vec->max_y = fmax(vec->max_y, item.max_y);
      }
      vec->features[vec->n_features++] = item;
    }

    OGR_F_Destroy(feature);
  }

  OCTDestroyCoordinateTransformation(ct);

  if (err != OSMIUM_OK) {
    osmium_vector_data_free(vec);
    return err;
  }

  *out = vec;
  return OSMIUM_OK;
}


osmium_error osmium_vector_data_open(const char *path,
                                     const char *user_agent,
                                     OsmiumVectorData **out) {
  static atomic_uint counter;

  if (!path || !out) {
    return OSMIUM_NULL_POINTER;
  }
  *out = NULL;

  pthread_once(&gdal_once, gdal_init);

  if (!is_remote(path)) {
    GDALDatasetH dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
    if (!dataset) {
      return OSMIUM_FILE_ERROR;
    }

    osmium_error err = read_dataset(dataset, out);
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
    err = read_dataset(dataset, out);
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

    if (x < f->min_x - tolerance || x > f->max_x + tolerance || y < f->min_y - tolerance ||
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
  pthread_once(&gdal_once, gdal_init);

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
