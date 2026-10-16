/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

typedef enum {
  OSMIUM_FEATURE_FIELD_STRING,
  OSMIUM_FEATURE_FIELD_INTEGER,
  OSMIUM_FEATURE_FIELD_REAL,
  OSMIUM_FEATURE_FIELD_DATE,
  OSMIUM_FEATURE_FIELD_OTHER
} OsmiumFeatureFieldType;

typedef struct {
  char *name;
  char *value;
  OsmiumFeatureFieldType type;
  int width;
} OsmiumFeatureField;

OSMIUM_API void osmium_feature_field_free(OsmiumFeatureField *field);

#define OSMIUM_TYPE_VECTOR_LAYER (osmium_vector_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumVectorLayer, osmium_vector_layer, OSMIUM, VECTOR_LAYER, GtkWidget)

OSMIUM_API GtkWidget *osmium_vector_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_vector_layer_get_map(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_map(OsmiumVectorLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_vector_layer_get_path(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_path(OsmiumVectorLayer *self, const char *path);

OSMIUM_API void osmium_vector_layer_reload(OsmiumVectorLayer *self);
OSMIUM_API gboolean osmium_vector_layer_get_loading(OsmiumVectorLayer *self);

OSMIUM_API const char *osmium_vector_layer_get_user_agent(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_user_agent(OsmiumVectorLayer *self, const char *user_agent);

OSMIUM_API const GdkRGBA *osmium_vector_layer_get_stroke_color(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_stroke_color(OsmiumVectorLayer *self, const GdkRGBA *color);

OSMIUM_API const GdkRGBA *osmium_vector_layer_get_fill_color(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_fill_color(OsmiumVectorLayer *self, const GdkRGBA *color);

OSMIUM_API double osmium_vector_layer_get_line_width(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_line_width(OsmiumVectorLayer *self, double width);

OSMIUM_API double osmium_vector_layer_get_point_radius(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_point_radius(OsmiumVectorLayer *self, double radius);

/* Features under a map position, topmost first. Elements are gint64 feature ids. */
OSMIUM_API GArray *osmium_vector_layer_identify(OsmiumVectorLayer *self,
                                                double lat,
                                                double lon,
                                                double tolerance_px);

/* Attribute table of one feature as OsmiumFeatureField, NULL when it can't be read. */
OSMIUM_API GPtrArray *osmium_vector_layer_get_feature_attributes(OsmiumVectorLayer *self, gint64 fid);

/* Editing writes straight into the source file. Only local, writable files are editable. */
OSMIUM_API gboolean osmium_vector_layer_is_editable(OsmiumVectorLayer *self);

/* Field names and types, with empty values. */
OSMIUM_API GPtrArray *osmium_vector_layer_get_fields(OsmiumVectorLayer *self);

OSMIUM_API gboolean osmium_vector_layer_set_attributes(OsmiumVectorLayer *self,
                                                       const gint64 *fids,
                                                       const char *const *fields,
                                                       const char *const *values,
                                                       guint n_edits,
                                                       GError **error);

OSMIUM_API gboolean osmium_vector_layer_add_field(OsmiumVectorLayer *self,
                                                  const char *name,
                                                  OsmiumFeatureFieldType type,
                                                  GError **error);

OSMIUM_API gboolean osmium_vector_layer_rename_field(OsmiumVectorLayer *self,
                                                     const char *old_name,
                                                     const char *new_name,
                                                     GError **error);

OSMIUM_API gboolean osmium_vector_layer_delete_field(OsmiumVectorLayer *self,
                                                     const char *name,
                                                     GError **error);

OSMIUM_API void osmium_vector_layer_set_selection(OsmiumVectorLayer *self, const gint64 *fids, guint n_fids);
OSMIUM_API GArray *osmium_vector_layer_get_selection(OsmiumVectorLayer *self);

OSMIUM_API guint osmium_vector_layer_get_n_features(OsmiumVectorLayer *self);

OSMIUM_API gboolean osmium_vector_layer_get_extent(OsmiumVectorLayer *self,
                                                      double *lat_min,
                                                      double *lon_min,
                                                      double *lat_max,
                                                      double *lon_max);

G_END_DECLS
