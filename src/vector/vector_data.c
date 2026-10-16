/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vector_data.h"

#include <cpl_conv.h>
#include <cpl_vsi.h>
#include <curl/curl.h>
#include <gdal.h>
#include <math.h>
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

  size_t cap = 0;
  osmium_error err = OSMIUM_OK;

  OGR_L_ResetReading(layer);
  OGRFeatureH feature;
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geom = OGR_F_GetGeometryRef(feature);
    OsmiumVectorFeature item = {0};

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
  free(vec);
}
