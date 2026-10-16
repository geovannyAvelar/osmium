/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "osmium_map_private.h"
#include "osmium_vector_layer.h"
#include "osmium_vector_style.h"
#include "vector/vector_edit.h"

typedef struct {
  GdkTexture *texture;
  guint id;
  guint zoom;
  double x;
  double y;
  int width;
  int height;
} LayerImage;

typedef struct {
  OsmiumEditTool tool;
  guint snap_mode;
  double snap_tolerance;
  gulong tool_handler;
  gulong key_handler;

  gboolean has_cursor;
  double cursor_x;
  double cursor_y;
  gboolean has_snap;
  double snap_nx;
  double snap_ny;

  GArray *draw_points; /* normalized x/y pairs of the feature being digitized */

  int drag; /* 0 none, 1 moving features, 2 moving a vertex */
  double drag_start_nx;
  double drag_start_ny;
  double drag_dx;
  double drag_dy;
  gboolean drag_moved;

  long long vertex_fid;
  size_t vertex_part;
  size_t vertex_index;
  double vertex_nx;
  double vertex_ny;

  gboolean has_active_vertex;
  long long active_fid;
  size_t active_part;
  size_t active_index;
} OsmiumEditorState;

struct _OsmiumVectorLayer {
  GtkWidget parent_instance;

  OsmiumMap *map; /* weak */
  gulong view_handler;
  gulong crs_handler;

  char *path;
  char *user_agent;
  char *source_crs;
  OsmiumStyle *style;
  GHashTable *columns;
  PangoLayout *label_layout;

  guint generation;
  gboolean loading;
  OsmiumVectorData *data;
  GArray *selection;
  GArray *selected_indices;
  guint render_source;
  guint last_request_id;
  LayerImage *image;
  GCancellable *cancellable;

  OsmiumEditSession *session;
  OsmiumEditorState editor;
};


/* shared with osmium_vector_editor.c */
void osmium_vector_layer_refresh(OsmiumVectorLayer *self);
void osmium_vector_layer_rebuild_selection(OsmiumVectorLayer *self);
void osmium_vector_layer_emit_editing_changed(OsmiumVectorLayer *self);

void osmium_editor_init(OsmiumVectorLayer *self);
void osmium_editor_finalize(OsmiumVectorLayer *self);
void osmium_editor_attach(OsmiumVectorLayer *self, OsmiumMap *map);
void osmium_editor_detach(OsmiumVectorLayer *self, OsmiumMap *map);
void osmium_editor_draw(OsmiumVectorLayer *self, GtkSnapshot *snapshot, float width, float height, double center_x, double center_y);
