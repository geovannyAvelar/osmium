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

typedef struct {
  char *name;
  char *code;
  gboolean assumed;
  gboolean ballpark;
  double accuracy;
} OsmiumLayerCrsInfo;

OSMIUM_API void osmium_layer_crs_info_clear(OsmiumLayerCrsInfo *info);

typedef enum {
  OSMIUM_EDIT_TOOL_NONE,
  OSMIUM_EDIT_TOOL_MOVE,
  OSMIUM_EDIT_TOOL_VERTEX,
  OSMIUM_EDIT_TOOL_ADD
} OsmiumEditTool;

typedef enum {
  OSMIUM_SNAP_NONE = 0,
  OSMIUM_SNAP_VERTEX = 1 << 0,
  OSMIUM_SNAP_SEGMENT = 1 << 1
} OsmiumSnapMode;

typedef enum {
  OSMIUM_GEOMETRY_POINT,
  OSMIUM_GEOMETRY_LINE,
  OSMIUM_GEOMETRY_POLYGON
} OsmiumGeometryKind;

typedef enum {
  OSMIUM_LAYER_ISSUE_INVALID,
  OSMIUM_LAYER_ISSUE_SELF_INTERSECTION,
  OSMIUM_LAYER_ISSUE_OVERLAP,
  OSMIUM_LAYER_ISSUE_DUPLICATE,
  OSMIUM_LAYER_ISSUE_DEGENERATE
} OsmiumLayerIssueKind;

typedef struct {
  OsmiumLayerIssueKind kind;
  gint64 fid;
  gboolean has_other;
  gint64 other_fid;
  double lon;
  double lat;
  char message[128];
} OsmiumLayerIssue;

OSMIUM_API void osmium_feature_field_free(OsmiumFeatureField *field);

#define OSMIUM_TYPE_VECTOR_LAYER (osmium_vector_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumVectorLayer, osmium_vector_layer, OSMIUM, VECTOR_LAYER, GtkWidget)

OSMIUM_API GtkWidget *osmium_vector_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_vector_layer_get_map(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_map(OsmiumVectorLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_vector_layer_get_path(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_path(OsmiumVectorLayer *self, const char *path);

/* Overrides the coordinate system stored in the file; NULL or "" uses the file's. */
OSMIUM_API const char *osmium_vector_layer_get_source_crs(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_source_crs(OsmiumVectorLayer *self, const char *source_crs);

/* Coordinate system the data was read in, and how reliable the datum shift to WGS 84 is.
 * name and code are NULL-able and owned by the caller (see osmium_layer_crs_info_clear). */
OSMIUM_API gboolean osmium_vector_layer_get_crs_info(OsmiumVectorLayer *self, OsmiumLayerCrsInfo *info);

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

/* --- Editing ---
 * An edit session keeps changes in memory (undoable) until osmium_vector_layer_save_edits() writes
 * them into the source file. Only local, writable files can be edited. */
OSMIUM_API gboolean osmium_vector_layer_start_editing(OsmiumVectorLayer *self, GError **error);
OSMIUM_API gboolean osmium_vector_layer_stop_editing(OsmiumVectorLayer *self, gboolean save, GError **error);
OSMIUM_API gboolean osmium_vector_layer_is_editing(OsmiumVectorLayer *self);
OSMIUM_API gboolean osmium_vector_layer_has_unsaved_edits(OsmiumVectorLayer *self);
OSMIUM_API gboolean osmium_vector_layer_save_edits(OsmiumVectorLayer *self, GError **error);
OSMIUM_API OsmiumGeometryKind osmium_vector_layer_get_geometry_kind(OsmiumVectorLayer *self);

OSMIUM_API OsmiumEditTool osmium_vector_layer_get_edit_tool(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_edit_tool(OsmiumVectorLayer *self, OsmiumEditTool tool);
OSMIUM_API guint osmium_vector_layer_get_snap_mode(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_snap_mode(OsmiumVectorLayer *self, guint snap_mode);
OSMIUM_API double osmium_vector_layer_get_snap_tolerance(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_set_snap_tolerance(OsmiumVectorLayer *self, double pixels);

OSMIUM_API gboolean osmium_vector_layer_can_undo(OsmiumVectorLayer *self);
OSMIUM_API gboolean osmium_vector_layer_can_redo(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_undo(OsmiumVectorLayer *self);
OSMIUM_API void osmium_vector_layer_redo(OsmiumVectorLayer *self);

/* Deletes the selected features; returns how many. */
OSMIUM_API guint osmium_vector_layer_delete_selected(OsmiumVectorLayer *self);

/* Buffered (undoable) attribute edits; old_values are what the caller displayed. */
OSMIUM_API gboolean osmium_vector_layer_edit_attributes(OsmiumVectorLayer *self,
                                                        const gint64 *fids,
                                                        const char *const *fields,
                                                        const char *const *old_values,
                                                        const char *const *new_values,
                                                        guint n_edits);

/* Topology checks on the features in memory, unsaved edits included: invalid or self-intersecting
 * geometries, overlapping polygons, duplicate geometries and repeated vertices. The result is a
 * GArray of OsmiumLayerIssue. */
OSMIUM_API void osmium_vector_layer_check_topology_async(OsmiumVectorLayer *self,
                                                         GAsyncReadyCallback callback,
                                                         gpointer user_data);
OSMIUM_API GArray *osmium_vector_layer_check_topology_finish(OsmiumVectorLayer *self,
                                                             GAsyncResult *result,
                                                             GError **error);

typedef struct _OsmiumFeatureClip OsmiumFeatureClip;

OSMIUM_API void osmium_feature_clip_free(OsmiumFeatureClip *clip);
OSMIUM_API guint osmium_feature_clip_get_n_features(OsmiumFeatureClip *clip);

/* Copies the selected features (geometry and attributes), NULL when nothing is selected. */
OSMIUM_API OsmiumFeatureClip *osmium_vector_layer_copy_selected(OsmiumVectorLayer *self);

/* Pastes into the edit session, centered on lat/lon. Only clips of the same geometry type are
 * accepted. Attributes are matched by field name. Returns the number of new features. */
OSMIUM_API guint osmium_vector_layer_paste(OsmiumVectorLayer *self, OsmiumFeatureClip *clip, double lat, double lon);
OSMIUM_API gboolean osmium_vector_layer_can_paste(OsmiumVectorLayer *self, OsmiumFeatureClip *clip);

OSMIUM_API guint osmium_vector_layer_get_n_features(OsmiumVectorLayer *self);

OSMIUM_API gboolean osmium_vector_layer_get_extent(OsmiumVectorLayer *self,
                                                      double *lat_min,
                                                      double *lon_min,
                                                      double *lat_max,
                                                      double *lon_max);

G_END_DECLS
