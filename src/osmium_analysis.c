/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_analysis.h"
#include "gdal_setup.h"

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <math.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <stdio.h>
#include <string.h>

#define SET_ERROR(error, ...) g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, __VA_ARGS__)

/* --- helpers ---------------------------------------------------------------------------------- */

typedef struct {
  GDALDatasetH dataset;
  OGRLayerH layer;
} VectorSource;

static gboolean source_open(VectorSource *source, const char *path, GError **error) {
  osmium_gdal_init();
  source->dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
  source->layer = source->dataset != NULL ? GDALDatasetGetLayer(source->dataset, 0) : NULL;
  if (source->layer == NULL) {
    SET_ERROR(error, "Could not read the vector file %s", path);
    if (source->dataset != NULL) {
      GDALClose(source->dataset);
    }
    source->dataset = NULL;
    return FALSE;
  }
  return TRUE;
}

static void source_close(VectorSource *source) {
  if (source->dataset != NULL) {
    GDALClose(source->dataset);
  }
  source->dataset = NULL;
}

static const char *driver_for(const char *path) {
  if (g_str_has_suffix(path, ".gpkg")) {
    return "GPKG";
  }
  if (g_str_has_suffix(path, ".geojson") || g_str_has_suffix(path, ".json")) {
    return "GeoJSON";
  }
  return "ESRI Shapefile";
}

static void remove_existing(const char *path) {
  osmium_gdal_init();
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    GDALDriverH driver = GDALIdentifyDriver(path, NULL);

    if (driver != NULL) {
      GDALDeleteDataset(driver, path);
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
      remove(path);
    }
  }
}

static gboolean output_create(VectorSource *out, const char *path, OGRSpatialReferenceH srs, OGRwkbGeometryType type,
                              GError **error) {
  GDALDriverH driver;
  g_autofree char *name = g_path_get_basename(path);
  char *dot = strrchr(name, '.');

  osmium_gdal_init();
  remove_existing(path);
  driver = GDALGetDriverByName(driver_for(path));
  if (driver == NULL) {
    SET_ERROR(error, "The format of %s is not available", path);
    return FALSE;
  }
  if (dot != NULL) {
    *dot = '\0';
  }
  out->dataset = GDALCreate(driver, path, 0, 0, 0, GDT_Unknown, NULL);
  out->layer = out->dataset != NULL ? GDALDatasetCreateLayer(out->dataset, name, srs, type, NULL) : NULL;
  if (out->layer == NULL) {
    SET_ERROR(error, "Could not create %s", path);
    source_close(out);
    return FALSE;
  }
  return TRUE;
}

static void copy_fields(OGRLayerH from, OGRLayerH to, const char *prefix) {
  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(from);

  for (int i = 0; i < OGR_FD_GetFieldCount(defn); i++) {
    OGRFieldDefnH field = OGR_FD_GetFieldDefn(defn, i);
    g_autofree char *name = g_strconcat(prefix != NULL ? prefix : "", OGR_Fld_GetNameRef(field), NULL);
    OGRFieldDefnH copy = OGR_Fld_Create(name, OGR_Fld_GetType(field));

    OGR_Fld_SetWidth(copy, OGR_Fld_GetWidth(field));
    OGR_Fld_SetPrecision(copy, OGR_Fld_GetPrecision(field));
    OGR_L_CreateField(to, copy, TRUE);
    OGR_Fld_Destroy(copy);
  }
}

static void copy_values(OGRFeatureH from, OGRFeatureH to, int offset) {
  OGRFeatureDefnH defn = OGR_F_GetDefnRef(from);

  for (int i = 0; i < OGR_FD_GetFieldCount(defn); i++) {
    if (OGR_F_IsFieldSetAndNotNull(from, i) && offset + i < OGR_F_GetFieldCount(to)) {
      OGR_F_SetFieldString(to, offset + i, OGR_F_GetFieldAsString(from, i));
    }
  }
}

static OGRSpatialReferenceH layer_srs(OGRLayerH layer) {
  OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(layer);

  if (srs != NULL) {
    OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  }
  return srs;
}

static OGRCoordinateTransformationH transformation(OGRSpatialReferenceH from, OGRSpatialReferenceH to) {
  if (from == NULL || to == NULL || OSRIsSame(from, to)) {
    return NULL;
  }
  return OCTNewCoordinateTransformation(from, to);
}

static OGRGeometryH extract_dimension(OGRGeometryH geometry, int dimension) {
  OGRwkbGeometryType flat;

  if (geometry == NULL) {
    return NULL;
  }
  if (OGR_G_IsEmpty(geometry)) {
    OGR_G_DestroyGeometry(geometry);
    return NULL;
  }
  flat = wkbFlatten(OGR_G_GetGeometryType(geometry));
  if (flat == wkbGeometryCollection) {
    OGRGeometryH kept = NULL;
    const char *kind = dimension == 2 ? "MULTIPOLYGON" : (dimension == 1 ? "MULTILINESTRING" : "MULTIPOINT");

    for (int i = 0; i < OGR_G_GetGeometryCount(geometry); i++) {
      OGRGeometryH part = OGR_G_GetGeometryRef(geometry, i);

      if (OGR_G_GetDimension(part) != dimension) {
        continue;
      }
      if (kept == NULL) {
        kept = OGR_G_CreateGeometry(dimension == 2 ? wkbMultiPolygon : (dimension == 1 ? wkbMultiLineString : wkbMultiPoint));
      }
      if (wkbFlatten(OGR_G_GetGeometryType(part)) == wkbMultiPolygon || wkbFlatten(OGR_G_GetGeometryType(part)) == wkbMultiLineString ||
          wkbFlatten(OGR_G_GetGeometryType(part)) == wkbMultiPoint) {
        for (int k = 0; k < OGR_G_GetGeometryCount(part); k++) {
          OGR_G_AddGeometry(kept, OGR_G_GetGeometryRef(part, k));
        }
      } else {
        OGR_G_AddGeometry(kept, part);
      }
    }
    (void)kind;
    OGR_G_DestroyGeometry(geometry);
    return kept;
  }
  if (OGR_G_GetDimension(geometry) != dimension) {
    OGR_G_DestroyGeometry(geometry);
    return NULL;
  }
  return geometry;
}

static OGRwkbGeometryType multi_type(OGRwkbGeometryType type) {
  switch (wkbFlatten(type)) {
  case wkbPoint:
  case wkbMultiPoint:
    return wkbMultiPoint;
  case wkbLineString:
  case wkbMultiLineString:
    return wkbMultiLineString;
  case wkbPolygon:
  case wkbMultiPolygon:
    return wkbMultiPolygon;
  default:
    return wkbUnknown;
  }
}

static int dimension_of_layer(OGRLayerH layer) {
  switch (wkbFlatten(OGR_L_GetGeomType(layer))) {
  case wkbPoint:
  case wkbMultiPoint:
    return 0;
  case wkbLineString:
  case wkbMultiLineString:
    return 1;
  default:
    return 2;
  }
}

/* union of every geometry of a layer, in the target coordinate system */
static OGRGeometryH layer_union(OGRLayerH layer, OGRSpatialReferenceH target) {
  OGRCoordinateTransformationH transform = transformation(layer_srs(layer), target);
  OGRGeometryH collection = OGR_G_CreateGeometry(wkbGeometryCollection);
  OGRGeometryH merged;
  OGRFeatureH feature;

  OGR_L_ResetReading(layer);
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);

    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      OGRGeometryH copy = OGR_G_Clone(geometry);

      if (transform != NULL) {
        OGR_G_Transform(copy, transform);
      }
      OGR_G_AddGeometryDirectly(collection, copy);
    }
    OGR_F_Destroy(feature);
  }
  if (transform != NULL) {
    OCTDestroyCoordinateTransformation(transform);
  }
  merged = OGR_G_UnaryUnion(collection);
  OGR_G_DestroyGeometry(collection);
  return merged;
}

/* --- buffer ----------------------------------------------------------------------------------- */

static OGRGeometryH buffer_geographic(OGRGeometryH geometry, OGRSpatialReferenceH geographic, double distance,
                                      int segments) {
  OGREnvelope envelope;
  double lon, lat;
  g_autofree char *definition = NULL;
  OGRSpatialReferenceH local = OSRNewSpatialReference(NULL);
  OGRSpatialReferenceH wgs = OSRNewSpatialReference(NULL);
  OGRCoordinateTransformationH to_wgs = NULL, to_local = NULL, back = NULL;
  OGRGeometryH work = OGR_G_Clone(geometry);
  OGRGeometryH result = NULL;

  OGR_G_GetEnvelope(geometry, &envelope);
  lon = (envelope.MinX + envelope.MaxX) / 2.0;
  lat = (envelope.MinY + envelope.MaxY) / 2.0;
  definition = g_strdup_printf("+proj=aeqd +lat_0=%.8f +lon_0=%.8f +x_0=0 +y_0=0 +datum=WGS84 +units=m +no_defs", lat, lon);
  OSRImportFromProj4(local, definition);
  OSRImportFromEPSG(wgs, 4326);
  OSRSetAxisMappingStrategy(local, OAMS_TRADITIONAL_GIS_ORDER);
  OSRSetAxisMappingStrategy(wgs, OAMS_TRADITIONAL_GIS_ORDER);

  to_wgs = OCTNewCoordinateTransformation(geographic, wgs);
  to_local = OCTNewCoordinateTransformation(wgs, local);
  back = OCTNewCoordinateTransformation(local, geographic);
  if (to_wgs != NULL && to_local != NULL && back != NULL) {
    OGR_G_Transform(work, to_wgs);
    OGR_G_Transform(work, to_local);
    result = OGR_G_Buffer(work, distance, segments);
    if (result != NULL) {
      OGR_G_Transform(result, back);
    }
  }
  OGR_G_DestroyGeometry(work);
  if (to_wgs != NULL) {
    OCTDestroyCoordinateTransformation(to_wgs);
  }
  if (to_local != NULL) {
    OCTDestroyCoordinateTransformation(to_local);
  }
  if (back != NULL) {
    OCTDestroyCoordinateTransformation(back);
  }
  OSRDestroySpatialReference(local);
  OSRDestroySpatialReference(wgs);
  return result;
}

gboolean osmium_analysis_buffer(const char *input, const char *output, double distance, gboolean in_metres, guint segments,
                                gboolean dissolve, GError **error) {
  VectorSource in = {0}, out = {0};
  OGRSpatialReferenceH srs;
  gboolean geographic;
  double layer_distance = distance;
  OGRGeometryH merged = NULL;
  OGRFeatureH feature;
  gboolean ok = FALSE;

  if (!source_open(&in, input, error)) {
    return FALSE;
  }
  srs = layer_srs(in.layer);
  geographic = srs != NULL && OSRIsGeographic(srs);
  if (in_metres && srs != NULL && !geographic) {
    layer_distance = distance / OSRGetLinearUnits(srs, NULL);
  }

  if (!output_create(&out, output, srs, wkbMultiPolygon, error)) {
    source_close(&in);
    return FALSE;
  }
  if (!dissolve) {
    copy_fields(in.layer, out.layer, NULL);
  }

  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    OGRGeometryH buffered = NULL;

    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      if (in_metres && geographic) {
        buffered = buffer_geographic(geometry, srs, distance, (int)segments);
      } else {
        buffered = OGR_G_Buffer(geometry, layer_distance, (int)segments);
      }
    }
    if (buffered != NULL && !OGR_G_IsEmpty(buffered)) {
      if (dissolve) {
        OGRGeometryH union_geometry = merged == NULL ? OGR_G_Clone(buffered) : OGR_G_Union(merged, buffered);

        if (merged != NULL) {
          OGR_G_DestroyGeometry(merged);
        }
        merged = union_geometry;
        OGR_G_DestroyGeometry(buffered);
      } else {
        OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));

        copy_values(feature, created, 0);
        OGR_F_SetGeometryDirectly(created, OGR_G_ForceToMultiPolygon(buffered));
        OGR_L_CreateFeature(out.layer, created);
        OGR_F_Destroy(created);
      }
    } else if (buffered != NULL) {
      OGR_G_DestroyGeometry(buffered);
    }
    OGR_F_Destroy(feature);
  }

  if (dissolve && merged != NULL) {
    OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));

    OGR_F_SetGeometryDirectly(created, OGR_G_ForceToMultiPolygon(merged));
    OGR_L_CreateFeature(out.layer, created);
    OGR_F_Destroy(created);
  }
  ok = TRUE;
  source_close(&out);
  source_close(&in);
  return ok;
}

/* --- overlay ---------------------------------------------------------------------------------- */

typedef struct {
  OGRFeatureH feature;
  OGRGeometryH geometry;
} Overlaid;

gboolean osmium_analysis_overlay(const char *input, const char *overlay, const char *output, OsmiumOverlayOp op,
                                 GError **error) {
  VectorSource in = {0}, over = {0}, out = {0};
  OGRSpatialReferenceH srs;
  OGRGeometryH mask = NULL;
  GPtrArray *pieces = NULL;
  OGRFeatureH feature;
  const int dimension_hint = 0;
  int dimension;

  (void)dimension_hint;
  if (!source_open(&in, input, error)) {
    return FALSE;
  }
  if (!source_open(&over, overlay, error)) {
    source_close(&in);
    return FALSE;
  }
  srs = layer_srs(in.layer);
  dimension = dimension_of_layer(in.layer);

  if (!output_create(&out, output, srs, multi_type(OGR_L_GetGeomType(in.layer)), error)) {
    source_close(&in);
    source_close(&over);
    return FALSE;
  }
  copy_fields(in.layer, out.layer, NULL);

  if (op == OSMIUM_OVERLAY_INTERSECT) {
    const int n_in = OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(in.layer));
    OGRCoordinateTransformationH transform = transformation(layer_srs(over.layer), srs);

    copy_fields(over.layer, out.layer, "b_");
    pieces = g_ptr_array_new();
    OGR_L_ResetReading(over.layer);
    while ((feature = OGR_L_GetNextFeature(over.layer)) != NULL) {
      OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);

      if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
        Overlaid *item = g_new0(Overlaid, 1);

        item->feature = feature;
        item->geometry = OGR_G_Clone(geometry);
        if (transform != NULL) {
          OGR_G_Transform(item->geometry, transform);
        }
        g_ptr_array_add(pieces, item);
      } else {
        OGR_F_Destroy(feature);
      }
    }
    if (transform != NULL) {
      OCTDestroyCoordinateTransformation(transform);
    }

    OGR_L_ResetReading(in.layer);
    while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
      OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);

      for (guint i = 0; geometry != NULL && i < pieces->len; i++) {
        const Overlaid *item = g_ptr_array_index(pieces, i);
        OGRGeometryH common;
        OGREnvelope a, b;

        OGR_G_GetEnvelope(geometry, &a);
        OGR_G_GetEnvelope(item->geometry, &b);
        if (a.MaxX < b.MinX || b.MaxX < a.MinX || a.MaxY < b.MinY || b.MaxY < a.MinY) {
          continue;
        }
        common = extract_dimension(OGR_G_Intersection(geometry, item->geometry), dimension);
        if (common != NULL) {
          OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));

          copy_values(feature, created, 0);
          copy_values(item->feature, created, n_in);
          OGR_F_SetGeometryDirectly(created, common);
          OGR_L_CreateFeature(out.layer, created);
          OGR_F_Destroy(created);
        }
      }
      OGR_F_Destroy(feature);
    }
    for (guint i = 0; i < pieces->len; i++) {
      Overlaid *item = g_ptr_array_index(pieces, i);

      OGR_F_Destroy(item->feature);
      OGR_G_DestroyGeometry(item->geometry);
      g_free(item);
    }
    g_ptr_array_unref(pieces);
  } else {
    mask = layer_union(over.layer, srs);
    OGR_L_ResetReading(in.layer);
    while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
      OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
      OGRGeometryH result = NULL;

      if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
        if (mask == NULL) {
          result = op == OSMIUM_OVERLAY_DIFFERENCE ? OGR_G_Clone(geometry) : NULL;
        } else if (op == OSMIUM_OVERLAY_CLIP) {
          result = OGR_G_Intersection(geometry, mask);
        } else {
          result = OGR_G_Difference(geometry, mask);
        }
        result = extract_dimension(result, dimension);
      }
      if (result != NULL) {
        OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));

        copy_values(feature, created, 0);
        OGR_F_SetGeometryDirectly(created, dimension == 2 ? OGR_G_ForceToMultiPolygon(result)
                                                          : (dimension == 1 ? OGR_G_ForceToMultiLineString(result) : OGR_G_ForceToMultiPoint(result)));
        OGR_L_CreateFeature(out.layer, created);
        OGR_F_Destroy(created);
      }
      OGR_F_Destroy(feature);
    }
    if (mask != NULL) {
      OGR_G_DestroyGeometry(mask);
    }
  }

  source_close(&out);
  source_close(&over);
  source_close(&in);
  return TRUE;
}

/* --- dissolve --------------------------------------------------------------------------------- */

typedef struct {
  OGRGeometryH collection;
  OGRFeatureH sample;
  guint count;
} Group;

static void group_free(gpointer data) {
  Group *group = data;

  OGR_G_DestroyGeometry(group->collection);
  if (group->sample != NULL) {
    OGR_F_Destroy(group->sample);
  }
  g_free(group);
}

gboolean osmium_analysis_dissolve(const char *input, const char *output, const char *group_field, GError **error) {
  VectorSource in = {0}, out = {0};
  OGRFeatureDefnH defn;
  int field_index = -1;
  GHashTable *groups;
  GPtrArray *order;
  OGRFeatureH feature;
  int dimension;

  if (!source_open(&in, input, error)) {
    return FALSE;
  }
  defn = OGR_L_GetLayerDefn(in.layer);
  if (group_field != NULL && group_field[0] != '\0') {
    field_index = OGR_FD_GetFieldIndex(defn, group_field);
    if (field_index < 0) {
      SET_ERROR(error, "The layer has no field “%s”", group_field);
      source_close(&in);
      return FALSE;
    }
  }
  dimension = dimension_of_layer(in.layer);

  if (!output_create(&out, output, layer_srs(in.layer), multi_type(OGR_L_GetGeomType(in.layer)), error)) {
    source_close(&in);
    return FALSE;
  }
  if (field_index >= 0) {
    OGRFieldDefnH field = OGR_FD_GetFieldDefn(defn, field_index);
    OGRFieldDefnH copy = OGR_Fld_Create(OGR_Fld_GetNameRef(field), OGR_Fld_GetType(field));

    OGR_Fld_SetWidth(copy, OGR_Fld_GetWidth(field));
    OGR_L_CreateField(out.layer, copy, TRUE);
    OGR_Fld_Destroy(copy);
  }
  {
    OGRFieldDefnH count = OGR_Fld_Create("count", OFTInteger);

    OGR_L_CreateField(out.layer, count, TRUE);
    OGR_Fld_Destroy(count);
  }

  groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, group_free);
  order = g_ptr_array_new();
  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    const char *raw = field_index >= 0 && OGR_F_IsFieldSetAndNotNull(feature, field_index) ? OGR_F_GetFieldAsString(feature, field_index) : "";
    Group *group;

    if (geometry == NULL || OGR_G_IsEmpty(geometry)) {
      OGR_F_Destroy(feature);
      continue;
    }
    group = g_hash_table_lookup(groups, raw);
    if (group == NULL) {
      char *key = g_strdup(raw);

      group = g_new0(Group, 1);
      group->collection = OGR_G_CreateGeometry(wkbGeometryCollection);
      group->sample = OGR_F_Clone(feature);
      g_hash_table_insert(groups, key, group);
      g_ptr_array_add(order, group);
    }
    OGR_G_AddGeometry(group->collection, geometry);
    group->count++;
    OGR_F_Destroy(feature);
  }

  for (guint i = 0; i < order->len; i++) {
    Group *group = g_ptr_array_index(order, i);
    OGRGeometryH merged = extract_dimension(OGR_G_UnaryUnion(group->collection), dimension);
    OGRFeatureH created;

    if (merged == NULL) {
      continue;
    }
    created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));
    if (field_index >= 0) {
      OGR_F_SetFieldString(created, 0, OGR_F_GetFieldAsString(group->sample, field_index));
    }
    OGR_F_SetFieldInteger(created, field_index >= 0 ? 1 : 0, (int)group->count);
    OGR_F_SetGeometryDirectly(created, dimension == 2 ? OGR_G_ForceToMultiPolygon(merged)
                                                      : (dimension == 1 ? OGR_G_ForceToMultiLineString(merged) : OGR_G_ForceToMultiPoint(merged)));
    OGR_L_CreateFeature(out.layer, created);
    OGR_F_Destroy(created);
  }

  g_ptr_array_unref(order);
  g_hash_table_destroy(groups);
  source_close(&out);
  source_close(&in);
  return TRUE;
}

/* --- location based --------------------------------------------------------------------------- */

static gboolean test_predicate(OGRGeometryH a, OGRGeometryH b, OsmiumPredicate predicate) {
  switch (predicate) {
  case OSMIUM_PREDICATE_DISJOINT:
    return OGR_G_Disjoint(a, b);
  case OSMIUM_PREDICATE_CONTAINS:
    return OGR_G_Contains(a, b);
  case OSMIUM_PREDICATE_WITHIN:
    return OGR_G_Within(a, b);
  case OSMIUM_PREDICATE_TOUCHES:
    return OGR_G_Touches(a, b);
  case OSMIUM_PREDICATE_CROSSES:
    return OGR_G_Crosses(a, b);
  case OSMIUM_PREDICATE_OVERLAPS:
    return OGR_G_Overlaps(a, b);
  case OSMIUM_PREDICATE_INTERSECTS:
  default:
    return OGR_G_Intersects(a, b);
  }
}

typedef struct {
  OGRFeatureH feature;
  OGRGeometryH geometry;
  OGREnvelope box;
} Candidate;

static GPtrArray *load_candidates(OGRLayerH layer, OGRSpatialReferenceH target) {
  GPtrArray *list = g_ptr_array_new();
  OGRCoordinateTransformationH transform = transformation(layer_srs(layer), target);
  OGRFeatureH feature;

  OGR_L_ResetReading(layer);
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);

    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      Candidate *item = g_new0(Candidate, 1);

      item->feature = feature;
      item->geometry = OGR_G_Clone(geometry);
      if (transform != NULL) {
        OGR_G_Transform(item->geometry, transform);
      }
      OGR_G_GetEnvelope(item->geometry, &item->box);
      g_ptr_array_add(list, item);
    } else {
      OGR_F_Destroy(feature);
    }
  }
  if (transform != NULL) {
    OCTDestroyCoordinateTransformation(transform);
  }
  return list;
}

static void candidates_free(GPtrArray *list) {
  for (guint i = 0; i < list->len; i++) {
    Candidate *item = g_ptr_array_index(list, i);

    OGR_F_Destroy(item->feature);
    OGR_G_DestroyGeometry(item->geometry);
    g_free(item);
  }
  g_ptr_array_unref(list);
}

static gboolean boxes_meet(const OGREnvelope *a, const OGREnvelope *b) {
  return !(a->MaxX < b->MinX || b->MaxX < a->MinX || a->MaxY < b->MinY || b->MaxY < a->MinY);
}

GArray *osmium_analysis_select_by_location(const char *input, const char *selector, OsmiumPredicate predicate, GError **error) {
  VectorSource in = {0}, other = {0};
  GPtrArray *candidates;
  GArray *fids = g_array_new(FALSE, FALSE, sizeof(gint64));
  OGRFeatureH feature;

  if (!source_open(&in, input, error)) {
    g_array_unref(fids);
    return NULL;
  }
  if (!source_open(&other, selector, error)) {
    source_close(&in);
    g_array_unref(fids);
    return NULL;
  }
  candidates = load_candidates(other.layer, layer_srs(in.layer));

  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    gboolean matched = FALSE;

    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      OGREnvelope box;

      OGR_G_GetEnvelope(geometry, &box);
      for (guint i = 0; i < candidates->len && !matched; i++) {
        const Candidate *item = g_ptr_array_index(candidates, i);

        if (predicate != OSMIUM_PREDICATE_DISJOINT && !boxes_meet(&box, &item->box)) {
          continue;
        }
        matched = test_predicate(geometry, item->geometry, predicate);
        if (predicate == OSMIUM_PREDICATE_DISJOINT && !matched) {
          break;
        }
      }
      if (predicate == OSMIUM_PREDICATE_DISJOINT) {
        matched = TRUE;
        for (guint i = 0; i < candidates->len; i++) {
          const Candidate *item = g_ptr_array_index(candidates, i);

          if (boxes_meet(&box, &item->box) && !OGR_G_Disjoint(geometry, item->geometry)) {
            matched = FALSE;
            break;
          }
        }
      }
    }
    if (matched) {
      const gint64 fid = OGR_F_GetFID(feature);

      g_array_append_val(fids, fid);
    }
    OGR_F_Destroy(feature);
  }
  candidates_free(candidates);
  source_close(&other);
  source_close(&in);
  return fids;
}

gboolean osmium_analysis_join_by_location(const char *input, const char *join, const char *output, OsmiumPredicate predicate,
                                          const char *prefix, GError **error) {
  VectorSource in = {0}, other = {0}, out = {0};
  GPtrArray *candidates;
  OGRFeatureH feature;
  int n_in, n_join;
  OGRFieldDefnH count;

  if (!source_open(&in, input, error)) {
    return FALSE;
  }
  if (!source_open(&other, join, error)) {
    source_close(&in);
    return FALSE;
  }
  if (!output_create(&out, output, layer_srs(in.layer), OGR_L_GetGeomType(in.layer), error)) {
    source_close(&in);
    source_close(&other);
    return FALSE;
  }
  copy_fields(in.layer, out.layer, NULL);
  copy_fields(other.layer, out.layer, prefix != NULL && prefix[0] != '\0' ? prefix : "j_");
  count = OGR_Fld_Create("join_n", OFTInteger);
  OGR_L_CreateField(out.layer, count, TRUE);
  OGR_Fld_Destroy(count);
  n_in = OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(in.layer));
  n_join = OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(other.layer));

  candidates = load_candidates(other.layer, layer_srs(in.layer));
  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));
    const Candidate *first = NULL;
    int matches = 0;

    copy_values(feature, created, 0);
    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      OGREnvelope box;

      OGR_G_GetEnvelope(geometry, &box);
      for (guint i = 0; i < candidates->len; i++) {
        const Candidate *item = g_ptr_array_index(candidates, i);

        if (!boxes_meet(&box, &item->box) || !test_predicate(geometry, item->geometry, predicate)) {
          continue;
        }
        if (first == NULL) {
          first = item;
        }
        matches++;
      }
      OGR_F_SetGeometry(created, geometry);
    }
    if (first != NULL) {
      copy_values(first->feature, created, n_in);
    }
    OGR_F_SetFieldInteger(created, n_in + n_join, matches);
    OGR_L_CreateFeature(out.layer, created);
    OGR_F_Destroy(created);
    OGR_F_Destroy(feature);
  }
  candidates_free(candidates);
  source_close(&out);
  source_close(&other);
  source_close(&in);
  return TRUE;
}

gboolean osmium_analysis_centroids(const char *input, const char *output, GError **error) {
  VectorSource in = {0}, out = {0};
  OGRFeatureH feature;

  if (!source_open(&in, input, error)) {
    return FALSE;
  }
  if (!output_create(&out, output, layer_srs(in.layer), wkbPoint, error)) {
    source_close(&in);
    return FALSE;
  }
  copy_fields(in.layer, out.layer, NULL);
  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);

    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      OGRGeometryH point = OGR_G_CreateGeometry(wkbPoint);
      OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));

      if (OGR_G_Centroid(geometry, point) != OGRERR_NONE) {
        OGR_G_DestroyGeometry(point);
        OGR_F_Destroy(created);
        OGR_F_Destroy(feature);
        continue;
      }
      copy_values(feature, created, 0);
      OGR_F_SetGeometryDirectly(created, point);
      OGR_L_CreateFeature(out.layer, created);
      OGR_F_Destroy(created);
    }
    OGR_F_Destroy(feature);
  }
  source_close(&out);
  source_close(&in);
  return TRUE;
}

/* --- raster calculator ------------------------------------------------------------------------ */

typedef enum {
  E_NUM,
  E_VAR,
  E_NEG,
  E_ADD,
  E_SUB,
  E_MUL,
  E_DIV,
  E_POW,
  E_LT,
  E_LE,
  E_GT,
  E_GE,
  E_EQ,
  E_NE,
  E_CALL
} ExprKind;

typedef struct Expr Expr;

struct Expr {
  ExprKind kind;
  double value;
  int variable;
  char function[8];
  Expr *args[3];
  int n_args;
};

typedef struct {
  const char *text;
  const char *pos;
  guint n_inputs;
  char *error;
} ExprParser;

static void expr_free(Expr *expr) {
  if (expr == NULL) {
    return;
  }
  for (int i = 0; i < expr->n_args; i++) {
    expr_free(expr->args[i]);
  }
  g_free(expr);
}

static Expr *expr_new(ExprKind kind) {
  Expr *expr = g_new0(Expr, 1);

  expr->kind = kind;
  return expr;
}

static void skip_space(ExprParser *p) {
  while (g_ascii_isspace(*p->pos)) {
    p->pos++;
  }
}

static Expr *parse_comparison(ExprParser *p);

static Expr *binary_expr(ExprKind kind, Expr *a, Expr *b) {
  Expr *expr = expr_new(kind);

  expr->args[0] = a;
  expr->args[1] = b;
  expr->n_args = 2;
  return expr;
}

static Expr *parse_primary(ExprParser *p) {
  Expr *expr = NULL;

  skip_space(p);
  if (p->error != NULL) {
    return NULL;
  }
  if (g_ascii_isdigit(*p->pos) || (*p->pos == '.' && g_ascii_isdigit(p->pos[1]))) {
    char *end;

    expr = expr_new(E_NUM);
    expr->value = g_ascii_strtod(p->pos, &end);
    p->pos = end;
  } else if (*p->pos == '(') {
    p->pos++;
    expr = parse_comparison(p);
    skip_space(p);
    if (*p->pos != ')') {
      p->error = g_strdup("Expected )");
    } else {
      p->pos++;
    }
  } else if (g_ascii_isalpha(*p->pos)) {
    char name[16] = {0};
    guint length = 0;

    while (g_ascii_isalnum(*p->pos) && length < sizeof(name) - 1) {
      name[length++] = *p->pos++;
    }
    skip_space(p);
    if (*p->pos == '(') {
      expr = expr_new(E_CALL);
      g_strlcpy(expr->function, name, sizeof(expr->function));
      for (char *c = expr->function; *c != '\0'; c++) {
        *c = g_ascii_tolower(*c);
      }
      p->pos++;
      while (p->error == NULL) {
        skip_space(p);
        if (*p->pos == ')') {
          p->pos++;
          break;
        }
        if (expr->n_args >= 3) {
          p->error = g_strdup("Too many arguments");
          break;
        }
        expr->args[expr->n_args++] = parse_comparison(p);
        skip_space(p);
        if (*p->pos == ',') {
          p->pos++;
        } else if (*p->pos != ')') {
          p->error = g_strdup("Expected , or )");
        }
      }
      if (p->error == NULL) {
        const char *f = expr->function;
        const int n = expr->n_args;
        const gboolean one = n == 1 && (!strcmp(f, "abs") || !strcmp(f, "sqrt") || !strcmp(f, "exp") || !strcmp(f, "ln") ||
                                        !strcmp(f, "log10") || !strcmp(f, "sin") || !strcmp(f, "cos") || !strcmp(f, "tan") ||
                                        !strcmp(f, "floor") || !strcmp(f, "ceil") || !strcmp(f, "round"));
        const gboolean two = n == 2 && (!strcmp(f, "min") || !strcmp(f, "max") || !strcmp(f, "pow"));
        const gboolean three = n == 3 && !strcmp(f, "if");

        if (!one && !two && !three) {
          p->error = g_strdup_printf("Unknown function or wrong argument count: %s", f);
        }
      }
    } else {
      if (length != 1 || g_ascii_toupper(name[0]) < 'A' || (guint)(g_ascii_toupper(name[0]) - 'A') >= p->n_inputs) {
        p->error = g_strdup_printf("Unknown name “%s”", name);
      } else {
        expr = expr_new(E_VAR);
        expr->variable = g_ascii_toupper(name[0]) - 'A';
      }
    }
  } else {
    p->error = g_strdup(*p->pos == '\0' ? "The expression ends too early" : "Unexpected character");
  }
  return expr;
}

static Expr *parse_unary(ExprParser *p);

static Expr *parse_power(ExprParser *p) {
  Expr *base = parse_primary(p);

  skip_space(p);
  if (p->error == NULL && *p->pos == '^') {
    p->pos++;
    return binary_expr(E_POW, base, parse_unary(p));
  }
  return base;
}

static Expr *parse_unary(ExprParser *p) {
  skip_space(p);
  if (*p->pos == '-') {
    Expr *expr = expr_new(E_NEG);

    p->pos++;
    expr->args[0] = parse_unary(p);
    expr->n_args = 1;
    return expr;
  }
  if (*p->pos == '+') {
    p->pos++;
    return parse_unary(p);
  }
  return parse_power(p);
}

static Expr *parse_term(ExprParser *p) {
  Expr *left = parse_unary(p);

  for (;;) {
    skip_space(p);
    if (p->error != NULL || (*p->pos != '*' && *p->pos != '/')) {
      return left;
    }
    {
      const ExprKind kind = *p->pos == '*' ? E_MUL : E_DIV;

      p->pos++;
      left = binary_expr(kind, left, parse_unary(p));
    }
  }
}

static Expr *parse_sum(ExprParser *p) {
  Expr *left = parse_term(p);

  for (;;) {
    skip_space(p);
    if (p->error != NULL || (*p->pos != '+' && *p->pos != '-')) {
      return left;
    }
    {
      const ExprKind kind = *p->pos == '+' ? E_ADD : E_SUB;

      p->pos++;
      left = binary_expr(kind, left, parse_term(p));
    }
  }
}

static Expr *parse_comparison(ExprParser *p) {
  Expr *left = parse_sum(p);

  skip_space(p);
  if (p->error == NULL) {
    ExprKind kind = E_NUM;
    int length = 0;

    if (p->pos[0] == '<' && p->pos[1] == '=') {
      kind = E_LE, length = 2;
    } else if (p->pos[0] == '>' && p->pos[1] == '=') {
      kind = E_GE, length = 2;
    } else if (p->pos[0] == '=' && p->pos[1] == '=') {
      kind = E_EQ, length = 2;
    } else if (p->pos[0] == '!' && p->pos[1] == '=') {
      kind = E_NE, length = 2;
    } else if (p->pos[0] == '<') {
      kind = E_LT, length = 1;
    } else if (p->pos[0] == '>') {
      kind = E_GT, length = 1;
    }
    if (length > 0) {
      p->pos += length;
      return binary_expr(kind, left, parse_sum(p));
    }
  }
  return left;
}

static Expr *expr_parse(const char *text, guint n_inputs, char **error) {
  ExprParser parser = {.text = text, .pos = text, .n_inputs = n_inputs};
  Expr *expr = parse_comparison(&parser);

  skip_space(&parser);
  if (parser.error == NULL && *parser.pos != '\0') {
    parser.error = g_strdup_printf("Unexpected text near “%s”", parser.pos);
  }
  if (parser.error != NULL) {
    expr_free(expr);
    *error = parser.error;
    return NULL;
  }
  *error = NULL;
  return expr;
}

char *osmium_analysis_check_expression(const char *expression, guint n_inputs) {
  char *error = NULL;
  Expr *expr = expr_parse(expression, n_inputs, &error);

  expr_free(expr);
  return error;
}

static double expr_eval(const Expr *e, const double *values) {
  switch (e->kind) {
  case E_NUM:
    return e->value;
  case E_VAR:
    return values[e->variable];
  case E_NEG:
    return -expr_eval(e->args[0], values);
  case E_ADD:
    return expr_eval(e->args[0], values) + expr_eval(e->args[1], values);
  case E_SUB:
    return expr_eval(e->args[0], values) - expr_eval(e->args[1], values);
  case E_MUL:
    return expr_eval(e->args[0], values) * expr_eval(e->args[1], values);
  case E_DIV: {
    const double d = expr_eval(e->args[1], values);

    return d != 0.0 ? expr_eval(e->args[0], values) / d : NAN;
  }
  case E_POW:
    return pow(expr_eval(e->args[0], values), expr_eval(e->args[1], values));
  case E_LT:
    return expr_eval(e->args[0], values) < expr_eval(e->args[1], values);
  case E_LE:
    return expr_eval(e->args[0], values) <= expr_eval(e->args[1], values);
  case E_GT:
    return expr_eval(e->args[0], values) > expr_eval(e->args[1], values);
  case E_GE:
    return expr_eval(e->args[0], values) >= expr_eval(e->args[1], values);
  case E_EQ:
    return expr_eval(e->args[0], values) == expr_eval(e->args[1], values);
  case E_NE:
    return expr_eval(e->args[0], values) != expr_eval(e->args[1], values);
  case E_CALL: {
    const char *f = e->function;
    const double a = expr_eval(e->args[0], values);

    if (!strcmp(f, "if")) {
      return a != 0.0 ? expr_eval(e->args[1], values) : expr_eval(e->args[2], values);
    }
    if (e->n_args == 2) {
      const double b = expr_eval(e->args[1], values);

      return !strcmp(f, "min") ? fmin(a, b) : (!strcmp(f, "max") ? fmax(a, b) : pow(a, b));
    }
    if (!strcmp(f, "abs")) {
      return fabs(a);
    }
    if (!strcmp(f, "sqrt")) {
      return a >= 0.0 ? sqrt(a) : NAN;
    }
    if (!strcmp(f, "exp")) {
      return exp(a);
    }
    if (!strcmp(f, "ln")) {
      return a > 0.0 ? log(a) : NAN;
    }
    if (!strcmp(f, "log10")) {
      return a > 0.0 ? log10(a) : NAN;
    }
    if (!strcmp(f, "sin")) {
      return sin(a);
    }
    if (!strcmp(f, "cos")) {
      return cos(a);
    }
    if (!strcmp(f, "tan")) {
      return tan(a);
    }
    if (!strcmp(f, "floor")) {
      return floor(a);
    }
    if (!strcmp(f, "ceil")) {
      return ceil(a);
    }
    return round(a);
  }
  default:
    return NAN;
  }
}

#define CALC_NODATA (-9999.0)

gboolean osmium_analysis_raster_calc(const char *const *inputs, const int *bands, guint n_inputs, const char *expression,
                                     const char *output, GError **error) {
  char *problem = NULL;
  Expr *expr;
  GDALDatasetH first = NULL;
  GDALDatasetH sources[8] = {0};
  GDALDatasetH owned[8] = {0};
  int has_nodata[8] = {0};
  double nodata[8] = {0};
  GDALRasterBandH band[8] = {0};
  GDALDatasetH out = NULL;
  GDALRasterBandH out_band;
  double geotransform[6];
  int width, height;
  double *rows[8] = {0};
  float *result = NULL;
  gboolean ok = FALSE;

  if (n_inputs == 0 || n_inputs > 8) {
    SET_ERROR(error, "Choose between 1 and 8 rasters");
    return FALSE;
  }
  expr = expr_parse(expression, n_inputs, &problem);
  if (expr == NULL) {
    SET_ERROR(error, "%s", problem);
    g_free(problem);
    return FALSE;
  }

  osmium_gdal_init();
  first = GDALOpen(inputs[0], GA_ReadOnly);
  if (first == NULL) {
    SET_ERROR(error, "Could not read %s", inputs[0]);
    expr_free(expr);
    return FALSE;
  }
  width = GDALGetRasterXSize(first);
  height = GDALGetRasterYSize(first);
  GDALGetGeoTransform(first, geotransform);
  sources[0] = first;

  for (guint i = 1; i < n_inputs; i++) {
    GDALDatasetH other = GDALOpen(inputs[i], GA_ReadOnly);
    double other_transform[6];
    char **argv = NULL;
    char values[4][G_ASCII_DTOSTR_BUF_SIZE];
    char size[2][16];
    GDALWarpAppOptions *options;
    const char *wkt = GDALGetProjectionRef(first);

    if (other == NULL) {
      SET_ERROR(error, "Could not read %s", inputs[i]);
      goto done;
    }
    GDALGetGeoTransform(other, other_transform);
    if (GDALGetRasterXSize(other) == width && GDALGetRasterYSize(other) == height &&
        memcmp(geotransform, other_transform, sizeof(geotransform)) == 0) {
      sources[i] = other;
      owned[i] = other;
      continue;
    }

    g_ascii_formatd(values[0], sizeof(values[0]), "%.15g", geotransform[0]);
    g_ascii_formatd(values[1], sizeof(values[1]), "%.15g", geotransform[3] + geotransform[5] * height);
    g_ascii_formatd(values[2], sizeof(values[2]), "%.15g", geotransform[0] + geotransform[1] * width);
    g_ascii_formatd(values[3], sizeof(values[3]), "%.15g", geotransform[3]);
    g_snprintf(size[0], sizeof(size[0]), "%d", width);
    g_snprintf(size[1], sizeof(size[1]), "%d", height);
    argv = CSLAddString(argv, "-of");
    argv = CSLAddString(argv, "MEM");
    argv = CSLAddString(argv, "-t_srs");
    argv = CSLAddString(argv, wkt);
    argv = CSLAddString(argv, "-te");
    for (int k = 0; k < 4; k++) {
      argv = CSLAddString(argv, values[k]);
    }
    argv = CSLAddString(argv, "-ts");
    argv = CSLAddString(argv, size[0]);
    argv = CSLAddString(argv, size[1]);
    argv = CSLAddString(argv, "-r");
    argv = CSLAddString(argv, "bilinear");
    options = GDALWarpAppOptionsNew(argv, NULL);
    CSLDestroy(argv);
    sources[i] = options != NULL ? GDALWarp("", NULL, 1, &other, options, NULL) : NULL;
    if (options != NULL) {
      GDALWarpAppOptionsFree(options);
    }
    GDALClose(other);
    if (sources[i] == NULL) {
      SET_ERROR(error, "Could not align %s with the first raster", inputs[i]);
      goto done;
    }
    owned[i] = sources[i];
  }

  for (guint i = 0; i < n_inputs; i++) {
    const int number = bands != NULL && bands[i] > 0 ? bands[i] : 1;

    if (number > GDALGetRasterCount(sources[i])) {
      SET_ERROR(error, "%s has no band %d", inputs[i], number);
      goto done;
    }
    band[i] = GDALGetRasterBand(sources[i], number);
    nodata[i] = GDALGetRasterNoDataValue(band[i], &has_nodata[i]);
    rows[i] = g_new(double, width);
  }

  {
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    char **options = NULL;

    remove_existing(output);
    options = CSLSetNameValue(options, "COMPRESS", "DEFLATE");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BIGTIFF", "IF_SAFER");
    out = GDALCreate(driver, output, width, height, 1, GDT_Float32, options);
    CSLDestroy(options);
  }
  if (out == NULL) {
    SET_ERROR(error, "Could not create %s", output);
    goto done;
  }
  GDALSetGeoTransform(out, geotransform);
  GDALSetProjection(out, GDALGetProjectionRef(first));
  out_band = GDALGetRasterBand(out, 1);
  GDALSetRasterNoDataValue(out_band, CALC_NODATA);
  result = g_new(float, width);

  for (int y = 0; y < height; y++) {
    for (guint i = 0; i < n_inputs; i++) {
      if (GDALRasterIO(band[i], GF_Read, 0, y, width, 1, rows[i], width, 1, GDT_Float64, 0, 0) != CE_None) {
        SET_ERROR(error, "Could not read %s", inputs[i]);
        goto done;
      }
    }
    for (int x = 0; x < width; x++) {
      double values[8];
      gboolean skip = FALSE;

      for (guint i = 0; i < n_inputs; i++) {
        values[i] = rows[i][x];
        if (!isfinite(values[i]) || (has_nodata[i] && values[i] == nodata[i])) {
          skip = TRUE;
          break;
        }
      }
      if (!skip) {
        const double v = expr_eval(expr, values);

        result[x] = isfinite(v) ? (float)v : (float)CALC_NODATA;
      } else {
        result[x] = (float)CALC_NODATA;
      }
    }
    if (GDALRasterIO(out_band, GF_Write, 0, y, width, 1, result, width, 1, GDT_Float32, 0, 0) != CE_None) {
      SET_ERROR(error, "Could not write %s", output);
      goto done;
    }
  }
  ok = TRUE;

done:
  if (out != NULL) {
    GDALClose(out);
  }
  for (guint i = 0; i < 8; i++) {
    g_free(rows[i]);
    if (owned[i] != NULL) {
      GDALClose(owned[i]);
    }
  }
  g_free(result);
  GDALClose(first);
  expr_free(expr);
  return ok;
}

/* --- zonal statistics ------------------------------------------------------------------------- */

gboolean osmium_analysis_zonal_stats(const char *polygons, const char *raster, int band_number, const char *prefix,
                                     const char *output, GError **error) {
  VectorSource in = {0}, out = {0};
  GDALDatasetH dataset;
  GDALRasterBandH band;
  OGRSpatialReferenceH raster_srs = NULL;
  OGRCoordinateTransformationH transform = NULL;
  double gt[6];
  int width, height, has_nodata = 0;
  double nodata;
  OGRFeatureH feature;
  int n_in;
  static const char *const names[] = {"count", "sum", "mean", "min", "max", "std"};
  g_autofree char *pre = g_strdup(prefix != NULL ? prefix : "");

  osmium_gdal_init();
  dataset = GDALOpen(raster, GA_ReadOnly);
  if (dataset == NULL) {
    SET_ERROR(error, "Could not read the raster %s", raster);
    return FALSE;
  }
  if (band_number < 1 || band_number > GDALGetRasterCount(dataset)) {
    SET_ERROR(error, "The raster has no band %d", band_number);
    GDALClose(dataset);
    return FALSE;
  }
  if (GDALGetGeoTransform(dataset, gt) != CE_None || gt[2] != 0.0 || gt[4] != 0.0) {
    SET_ERROR(error, "The raster must be georeferenced and not rotated");
    GDALClose(dataset);
    return FALSE;
  }
  band = GDALGetRasterBand(dataset, band_number);
  nodata = GDALGetRasterNoDataValue(band, &has_nodata);
  width = GDALGetRasterXSize(dataset);
  height = GDALGetRasterYSize(dataset);
  if (GDALGetProjectionRef(dataset) != NULL && GDALGetProjectionRef(dataset)[0] != '\0') {
    raster_srs = OSRNewSpatialReference(GDALGetProjectionRef(dataset));
    OSRSetAxisMappingStrategy(raster_srs, OAMS_TRADITIONAL_GIS_ORDER);
  }

  if (!source_open(&in, polygons, error)) {
    GDALClose(dataset);
    return FALSE;
  }
  if (!output_create(&out, output, layer_srs(in.layer), OGR_L_GetGeomType(in.layer), error)) {
    source_close(&in);
    GDALClose(dataset);
    return FALSE;
  }
  copy_fields(in.layer, out.layer, NULL);
  n_in = OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(in.layer));
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    g_autofree char *name = g_strconcat(pre, names[i], NULL);
    OGRFieldDefnH field = OGR_Fld_Create(name, i == 0 ? OFTInteger : OFTReal);

    OGR_L_CreateField(out.layer, field, TRUE);
    OGR_Fld_Destroy(field);
  }
  transform = transformation(layer_srs(in.layer), raster_srs);

  OGR_L_ResetReading(in.layer);
  while ((feature = OGR_L_GetNextFeature(in.layer)) != NULL) {
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    OGRFeatureH created = OGR_F_Create(OGR_L_GetLayerDefn(out.layer));
    double count = 0, sum = 0, sum_squares = 0, min = INFINITY, max = -INFINITY;

    copy_values(feature, created, 0);
    if (geometry != NULL && !OGR_G_IsEmpty(geometry)) {
      OGRGeometryH work = OGR_G_Clone(geometry);
      OGREnvelope box;
      int x0, x1, y0, y1;

      OGR_F_SetGeometry(created, geometry);
      if (transform != NULL) {
        OGR_G_Transform(work, transform);
      }
      OGR_G_GetEnvelope(work, &box);
      x0 = MAX((int)floor((box.MinX - gt[0]) / gt[1]), 0);
      x1 = MIN((int)ceil((box.MaxX - gt[0]) / gt[1]), width);
      if (gt[5] < 0) {
        y0 = MAX((int)floor((box.MaxY - gt[3]) / gt[5]), 0);
        y1 = MIN((int)ceil((box.MinY - gt[3]) / gt[5]), height);
      } else {
        y0 = MAX((int)floor((box.MinY - gt[3]) / gt[5]), 0);
        y1 = MIN((int)ceil((box.MaxY - gt[3]) / gt[5]), height);
      }

      for (int strip = y0; strip < y1 && x1 > x0; strip += 256) {
        const int rows = MIN(256, y1 - strip);
        const int columns = x1 - x0;
        const size_t cells = (size_t)rows * (size_t)columns;
        GDALDriverH mem = GDALGetDriverByName("MEM");
        GDALDatasetH mask = GDALCreate(mem, "", columns, rows, 1, GDT_Byte, NULL);
        double sub[6] = {gt[0] + x0 * gt[1], gt[1], 0, gt[3] + strip * gt[5], 0, gt[5]};
        int band_list[1] = {1};
        double burn[1] = {1.0};
        OGRGeometryH geometries[1] = {work};
        guchar *mask_data = g_malloc0(cells);
        double *values = g_new(double, cells);

        if (mask == NULL) {
          g_free(mask_data);
          g_free(values);
          break;
        }
        GDALSetGeoTransform(mask, sub);
        GDALRasterizeGeometries(mask, 1, band_list, 1, geometries, NULL, NULL, burn, NULL, NULL, NULL);
        GDALRasterIO(GDALGetRasterBand(mask, 1), GF_Read, 0, 0, columns, rows, mask_data, columns, rows, GDT_Byte, 0, 0);
        GDALClose(mask);
        if (GDALRasterIO(band, GF_Read, x0, strip, columns, rows, values, columns, rows, GDT_Float64, 0, 0) == CE_None) {
          for (size_t k = 0; k < cells; k++) {
            if (!mask_data[k] || !isfinite(values[k]) || (has_nodata && values[k] == nodata)) {
              continue;
            }
            count++;
            sum += values[k];
            sum_squares += values[k] * values[k];
            min = fmin(min, values[k]);
            max = fmax(max, values[k]);
          }
        }
        g_free(mask_data);
        g_free(values);
      }
      OGR_G_DestroyGeometry(work);
    }

    OGR_F_SetFieldInteger(created, n_in, (int)count);
    if (count > 0) {
      const double mean = sum / count;

      OGR_F_SetFieldDouble(created, n_in + 1, sum);
      OGR_F_SetFieldDouble(created, n_in + 2, mean);
      OGR_F_SetFieldDouble(created, n_in + 3, min);
      OGR_F_SetFieldDouble(created, n_in + 4, max);
      OGR_F_SetFieldDouble(created, n_in + 5, sqrt(fmax(sum_squares / count - mean * mean, 0.0)));
    }
    OGR_L_CreateFeature(out.layer, created);
    OGR_F_Destroy(created);
    OGR_F_Destroy(feature);
  }

  if (transform != NULL) {
    OCTDestroyCoordinateTransformation(transform);
  }
  if (raster_srs != NULL) {
    OSRDestroySpatialReference(raster_srs);
  }
  source_close(&out);
  source_close(&in);
  GDALClose(dataset);
  return TRUE;
}

/* --- terrain ---------------------------------------------------------------------------------- */

gboolean osmium_analysis_dem(const char *raster, int band, OsmiumDemMode mode, double z_factor, double azimuth,
                             double altitude, const char *output, GError **error) {
  GDALDatasetH source;
  GDALDatasetH result;
  char **argv = NULL;
  static const char *const modes[] = {"hillshade", "slope", "aspect", "roughness"};
  char text[G_ASCII_DTOSTR_BUF_SIZE];
  GDALDEMProcessingOptions *options;

  osmium_gdal_init();
  source = GDALOpen(raster, GA_ReadOnly);
  if (source == NULL) {
    SET_ERROR(error, "Could not read the raster %s", raster);
    return FALSE;
  }
  remove_existing(output);

  argv = CSLAddString(argv, "-of");
  argv = CSLAddString(argv, "GTiff");
  argv = CSLAddString(argv, "-b");
  g_snprintf(text, sizeof(text), "%d", MAX(band, 1));
  argv = CSLAddString(argv, text);
  argv = CSLAddString(argv, "-compute_edges");
  argv = CSLAddString(argv, "-co");
  argv = CSLAddString(argv, "COMPRESS=DEFLATE");
  {
    const char *wkt = GDALGetProjectionRef(source);
    OGRSpatialReferenceH srs = wkt != NULL && wkt[0] != '\0' ? OSRNewSpatialReference(wkt) : NULL;
    const gboolean geographic = srs != NULL && OSRIsGeographic(srs);
    const double scale = (geographic ? 111120.0 : 1.0) / (mode == OSMIUM_DEM_HILLSHADE || z_factor <= 0.0 ? 1.0 : z_factor);

    if (srs != NULL) {
      OSRDestroySpatialReference(srs);
    }
    if (mode != OSMIUM_DEM_ASPECT && mode != OSMIUM_DEM_ROUGHNESS) {
      argv = CSLAddString(argv, "-s");
      g_ascii_formatd(text, sizeof(text), "%.8g", scale);
      argv = CSLAddString(argv, text);
    }
  }
  if (mode == OSMIUM_DEM_HILLSHADE) {
    argv = CSLAddString(argv, "-z");
    g_ascii_formatd(text, sizeof(text), "%.8g", z_factor);
    argv = CSLAddString(argv, text);
    argv = CSLAddString(argv, "-az");
    g_ascii_formatd(text, sizeof(text), "%.8g", azimuth);
    argv = CSLAddString(argv, text);
    argv = CSLAddString(argv, "-alt");
    g_ascii_formatd(text, sizeof(text), "%.8g", altitude);
    argv = CSLAddString(argv, text);
  }
  options = GDALDEMProcessingOptionsNew(argv, NULL);
  CSLDestroy(argv);
  if (options == NULL) {
    SET_ERROR(error, "Invalid terrain options");
    GDALClose(source);
    return FALSE;
  }
  result = GDALDEMProcessing(output, source, modes[mode], NULL, options, NULL);
  GDALDEMProcessingOptionsFree(options);
  GDALClose(source);
  if (result == NULL) {
    SET_ERROR(error, "Could not compute the terrain model");
    return FALSE;
  }
  GDALClose(result);
  return TRUE;
}

gboolean osmium_analysis_contours(const char *raster, int band_number, double interval, double offset, const char *output,
                                  GError **error) {
  GDALDatasetH source;
  GDALRasterBandH band;
  VectorSource out = {0};
  OGRSpatialReferenceH srs = NULL;
  char **options = NULL;
  char text[G_ASCII_DTOSTR_BUF_SIZE];
  int has_nodata = 0;
  double nodata;
  CPLErr err;

  if (interval <= 0.0) {
    SET_ERROR(error, "The interval must be greater than zero");
    return FALSE;
  }
  osmium_gdal_init();
  source = GDALOpen(raster, GA_ReadOnly);
  if (source == NULL) {
    SET_ERROR(error, "Could not read the raster %s", raster);
    return FALSE;
  }
  if (band_number < 1 || band_number > GDALGetRasterCount(source)) {
    SET_ERROR(error, "The raster has no band %d", band_number);
    GDALClose(source);
    return FALSE;
  }
  band = GDALGetRasterBand(source, band_number);
  if (GDALGetProjectionRef(source) != NULL && GDALGetProjectionRef(source)[0] != '\0') {
    srs = OSRNewSpatialReference(GDALGetProjectionRef(source));
    OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  }
  if (!output_create(&out, output, srs, wkbLineString, error)) {
    if (srs != NULL) {
      OSRDestroySpatialReference(srs);
    }
    GDALClose(source);
    return FALSE;
  }
  {
    OGRFieldDefnH id = OGR_Fld_Create("ID", OFTInteger);
    OGRFieldDefnH elevation = OGR_Fld_Create("ELEV", OFTReal);

    OGR_L_CreateField(out.layer, id, TRUE);
    OGR_L_CreateField(out.layer, elevation, TRUE);
    OGR_Fld_Destroy(id);
    OGR_Fld_Destroy(elevation);
  }
  g_ascii_formatd(text, sizeof(text), "%.10g", interval);
  options = CSLSetNameValue(options, "LEVEL_INTERVAL", text);
  g_ascii_formatd(text, sizeof(text), "%.10g", offset);
  options = CSLSetNameValue(options, "LEVEL_BASE", text);
  options = CSLSetNameValue(options, "ID_FIELD", "0");
  options = CSLSetNameValue(options, "ELEV_FIELD", "1");
  nodata = GDALGetRasterNoDataValue(band, &has_nodata);
  if (has_nodata) {
    g_ascii_formatd(text, sizeof(text), "%.15g", nodata);
    options = CSLSetNameValue(options, "NODATA", text);
  }
  err = GDALContourGenerateEx(band, out.layer, options, NULL, NULL);
  CSLDestroy(options);
  source_close(&out);
  if (srs != NULL) {
    OSRDestroySpatialReference(srs);
  }
  GDALClose(source);
  if (err != CE_None) {
    SET_ERROR(error, "Could not generate the contour lines");
    return FALSE;
  }
  return TRUE;
}
