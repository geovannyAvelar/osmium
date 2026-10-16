/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "shapefile.h"

#include <cpl_conv.h>
#include <gdal.h>
#include <math.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define WEB_MERCATOR_HALF_WORLD 20037508.342789244

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

static int kind_of(OGRGeometryH geom, OsmiumShpKind *kind) {
  switch (wkbFlatten(OGR_G_GetGeometryType(geom))) {
  case wkbPoint:
  case wkbMultiPoint:
    *kind = OSMIUM_SHP_POINT;
    return 1;
  case wkbLineString:
  case wkbMultiLineString:
    *kind = OSMIUM_SHP_LINE;
    return 1;
  case wkbPolygon:
  case wkbMultiPolygon:
    *kind = OSMIUM_SHP_POLYGON;
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

static void feature_clear(OsmiumShpFeature *f) {
  free(f->part_start);
  free(f->xy);
  memset(f, 0, sizeof(*f));
}

static int feature_from(OGRGeometryH geom, OsmiumShpFeature *out) {
  Accumulator acc = {0};
  OsmiumShpKind kind;

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

osmium_error osmium_shapefile_open(const char *path, OsmiumShapefile **out) {
  if (!path || !out) {
    return OSMIUM_NULL_POINTER;
  }
  *out = NULL;

  pthread_once(&gdal_once, gdal_init);

  GDALDatasetH dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
  if (!dataset) {
    return OSMIUM_FILE_ERROR;
  }

  OGRLayerH layer = GDALDatasetGetLayer(dataset, 0);
  if (!layer) {
    GDALClose(dataset);
    return OSMIUM_FILE_ERROR;
  }

  OGRCoordinateTransformationH ct = make_transform(layer);
  if (!ct) {
    GDALClose(dataset);
    return OSMIUM_CRS_ERROR;
  }

  OsmiumShapefile *shp = calloc(1, sizeof(*shp));
  if (!shp) {
    OCTDestroyCoordinateTransformation(ct);
    GDALClose(dataset);
    return OSMIUM_OUT_OF_MEMORY;
  }

  size_t cap = 0;
  osmium_error err = OSMIUM_OK;

  OGR_L_ResetReading(layer);
  OGRFeatureH feature;
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geom = OGR_F_GetGeometryRef(feature);
    OsmiumShpFeature item = {0};

    if (geom != NULL && OGR_G_Transform(geom, ct) == OGRERR_NONE && feature_from(geom, &item)) {
      if (shp->n_features == cap) {
        cap = cap ? cap * 2 : 256;
        OsmiumShpFeature *grown = realloc(shp->features, cap * sizeof(*grown));
        if (!grown) {
          feature_clear(&item);
          OGR_F_Destroy(feature);
          err = OSMIUM_OUT_OF_MEMORY;
          break;
        }
        shp->features = grown;
      }

      if (shp->n_features == 0) {
        shp->min_x = item.min_x;
        shp->min_y = item.min_y;
        shp->max_x = item.max_x;
        shp->max_y = item.max_y;
      } else {
        shp->min_x = fmin(shp->min_x, item.min_x);
        shp->min_y = fmin(shp->min_y, item.min_y);
        shp->max_x = fmax(shp->max_x, item.max_x);
        shp->max_y = fmax(shp->max_y, item.max_y);
      }
      shp->features[shp->n_features++] = item;
    }

    OGR_F_Destroy(feature);
  }

  OCTDestroyCoordinateTransformation(ct);
  GDALClose(dataset);

  if (err != OSMIUM_OK) {
    osmium_shapefile_free(shp);
    return err;
  }

  *out = shp;
  return OSMIUM_OK;
}

void osmium_shapefile_free(OsmiumShapefile *shp) {
  if (!shp) {
    return;
  }
  for (size_t i = 0; i < shp->n_features; i++) {
    feature_clear(&shp->features[i]);
  }
  free(shp->features);
  free(shp);
}
