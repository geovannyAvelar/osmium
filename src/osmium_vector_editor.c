/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_layer_private.h"

#include <math.h>
#include <string.h>

#define EDIT_HANDLE_PICK_PX 9.0
#define EDIT_FEATURE_PICK_PX 6.0
#define EDIT_MOVE_SLOP_PX 3.0
#define EDIT_MAX_HANDLES 4000
#define EDIT_HIT_LIMIT 16

static double world_size(OsmiumVectorLayer *self) {
  return 256.0 * ldexp(1.0, (int)osmium_map_get_zoom(self->map));
}

static double px_to_norm(OsmiumVectorLayer *self, double px) {
  return px / world_size(self);
}

static OsmiumVectorKind data_kind(OsmiumVectorLayer *self) {
  return self->data->geometry_kind;
}

/* --- geometry helpers ----------------------------------------------------------------------- */

static void geometry_free_view(OsmiumGeometry *g) {
  osmium_geometry_clear(g);
}

static gboolean convert_to_lonlat(OsmiumVectorLayer *self, double *xy, size_t n) {
  return osmium_projection_inverse_many(osmium_map_get_projection(self->map), xy, n) == 0;
}

static gboolean is_ring(const OsmiumVectorFeature *f) {
  return f->kind == OSMIUM_VECTOR_POLYGON;
}

/* The vertex at the first and last position of a ring is the same one. */
static size_t canonical_index(const OsmiumVectorFeature *f, size_t part, size_t absolute) {
  const size_t start = f->part_start[part];
  const size_t last = f->part_start[part + 1] - 1;

  return is_ring(f) && absolute == last ? start : absolute;
}

static gboolean replace_vertex(OsmiumVectorLayer *self, const OsmiumVectorFeature *f, size_t part, size_t absolute,
                               double nx, double ny, OsmiumGeometry *out) {
  double position[2] = {nx, ny};
  const size_t start = f->part_start[part];
  const size_t last = f->part_start[part + 1] - 1;

  if (!osmium_vector_data_copy_geometry(self->data, (size_t)osmium_vector_data_find_fid(self->data, f->fid), out) ||
      !convert_to_lonlat(self, position, 1)) {
    return FALSE;
  }

  out->ll[absolute * 2] = position[0];
  out->ll[absolute * 2 + 1] = position[1];
  if (is_ring(f) && (absolute == start || absolute == last)) {
    out->ll[start * 2] = out->ll[last * 2] = position[0];
    out->ll[start * 2 + 1] = out->ll[last * 2 + 1] = position[1];
  }
  return TRUE;
}

static gboolean insert_vertex_geometry(OsmiumVectorLayer *self, const OsmiumVectorFeature *f, size_t part,
                                       size_t after_absolute, double nx, double ny, OsmiumGeometry *out) {
  OsmiumGeometry source;
  double position[2] = {nx, ny};

  if (!osmium_vector_data_copy_geometry(self->data, (size_t)osmium_vector_data_find_fid(self->data, f->fid), &source) ||
      !convert_to_lonlat(self, position, 1)) {
    return FALSE;
  }

  memset(out, 0, sizeof(*out));
  out->kind = source.kind;
  out->n_parts = source.n_parts;
  out->n_points = source.n_points + 1;
  out->ll = malloc(out->n_points * 2 * sizeof(double));
  out->part_start = malloc((out->n_parts + 1) * sizeof(size_t));
  out->part_role = malloc(out->n_parts ? out->n_parts : 1);

  if (!out->ll || !out->part_start || !out->part_role) {
    osmium_geometry_clear(out);
    osmium_geometry_clear(&source);
    return FALSE;
  }

  const size_t at = after_absolute + 1;

  memcpy(out->ll, source.ll, at * 2 * sizeof(double));
  out->ll[at * 2] = position[0];
  out->ll[at * 2 + 1] = position[1];
  memcpy(out->ll + (at + 1) * 2, source.ll + at * 2, (source.n_points - at) * 2 * sizeof(double));
  memcpy(out->part_role, source.part_role, source.n_parts);
  for (size_t p = 0; p <= source.n_parts; p++) {
    out->part_start[p] = source.part_start[p] + (p > part ? 1 : 0);
  }

  osmium_geometry_clear(&source);
  return TRUE;
}

static gboolean delete_vertex_geometry(OsmiumVectorLayer *self, const OsmiumVectorFeature *f, size_t part,
                                       size_t absolute, OsmiumGeometry *out) {
  OsmiumGeometry source;
  const size_t start = f->part_start[part];
  const size_t end = f->part_start[part + 1];
  const size_t unique = is_ring(f) ? end - start - 1 : end - start;
  const size_t minimum = f->kind == OSMIUM_VECTOR_LINE ? 2 : (f->kind == OSMIUM_VECTOR_POLYGON ? 3 : 1);

  if (unique <= minimum || f->kind == OSMIUM_VECTOR_POINT) {
    return FALSE;
  }

  if (!osmium_vector_data_copy_geometry(self->data, (size_t)osmium_vector_data_find_fid(self->data, f->fid), &source)) {
    return FALSE;
  }

  memset(out, 0, sizeof(*out));
  out->kind = source.kind;
  out->n_parts = source.n_parts;
  out->ll = malloc(source.n_points * 2 * sizeof(double));
  out->part_start = malloc((out->n_parts + 1) * sizeof(size_t));
  out->part_role = malloc(out->n_parts ? out->n_parts : 1);

  if (!out->ll || !out->part_start || !out->part_role) {
    osmium_geometry_clear(out);
    osmium_geometry_clear(&source);
    return FALSE;
  }

  memcpy(out->part_role, source.part_role, source.n_parts);

  const size_t last = end - 1;
  const gboolean drops_first = is_ring(f) && (absolute == start || absolute == last);
  size_t written = 0;

  for (size_t p = 0; p < source.n_parts; p++) {
    out->part_start[p] = written;

    for (size_t i = source.part_start[p]; i < source.part_start[p + 1]; i++) {
      if (p == part && ((!drops_first && i == absolute) || (drops_first && i == start))) {
        continue;
      }
      memcpy(out->ll + written * 2, source.ll + i * 2, 2 * sizeof(double));
      written++;
    }

    if (p == part && drops_first) {
      /* the ring must close on its new first vertex */
      memcpy(out->ll + (written - 1) * 2, out->ll + out->part_start[p] * 2, 2 * sizeof(double));
    }
  }
  out->part_start[source.n_parts] = written;
  out->n_points = written;

  osmium_geometry_clear(&source);
  return TRUE;
}

/* --- picking and snapping ------------------------------------------------------------------- */

static gboolean read_lock(OsmiumVectorLayer *self) {
  return self->data != NULL && osmium_vector_data_begin_read(self->data, osmium_map_get_projection(self->map));
}

static gint64 pick_feature(OsmiumVectorLayer *self, double nx, double ny) {
  long long fids[EDIT_HIT_LIMIT];
  size_t count;

  if (!read_lock(self)) {
    return G_MININT64;
  }
  count = osmium_vector_data_hit_test(self->data, nx, ny, px_to_norm(self, EDIT_FEATURE_PICK_PX), fids, EDIT_HIT_LIMIT);
  osmium_vector_data_end_read(self->data);
  return count > 0 ? fids[0] : G_MININT64;
}

static gboolean selection_has(OsmiumVectorLayer *self, gint64 fid) {
  for (guint i = 0; i < self->selection->len; i++) {
    if (g_array_index(self->selection, gint64, i) == fid) {
      return TRUE;
    }
  }
  return FALSE;
}

static void set_selection_single(OsmiumVectorLayer *self, gint64 fid) {
  osmium_vector_layer_set_selection(self, &fid, 1);
}

static double segment_nearest(double px, double py, double ax, double ay, double bx, double by, double *cx, double *cy) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double length2 = dx * dx + dy * dy;
  double t = length2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / length2 : 0.0;

  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  *cx = ax + t * dx;
  *cy = ay + t * dy;
  return (px - *cx) * (px - *cx) + (py - *cy) * (py - *cy);
}

static gboolean snap_find(OsmiumVectorLayer *self, double nx, double ny, double *sx, double *sy) {
  OsmiumEditorState *e = &self->editor;
  const double tol = px_to_norm(self, e->snap_tolerance);
  double best = tol * tol;
  gboolean found = FALSE;

  if (e->snap_mode == OSMIUM_SNAP_NONE || !read_lock(self)) {
    return FALSE;
  }

  for (size_t i = 0; i < self->data->n_features; i++) {
    const OsmiumVectorFeature *f = &self->data->features[i];

    if (f->deleted || nx < f->min_x - tol || nx > f->max_x + tol || ny < f->min_y - tol || ny > f->max_y + tol) {
      continue;
    }
    if (e->drag == 1 && selection_has(self, f->fid)) {
      continue;
    }

    for (size_t p = 0; p < f->n_parts; p++) {
      const size_t start = f->part_start[p];
      const size_t end = f->part_start[p + 1];

      for (size_t v = start; v < end; v++) {
        const gboolean dragged = e->drag == 2 && f->fid == e->vertex_fid && p == e->vertex_part &&
                                 canonical_index(f, p, v) == canonical_index(f, p, e->vertex_index);

        if (dragged) {
          continue;
        }

        if (e->snap_mode & OSMIUM_SNAP_VERTEX) {
          const double d2 = (nx - f->xy[v * 2]) * (nx - f->xy[v * 2]) + (ny - f->xy[v * 2 + 1]) * (ny - f->xy[v * 2 + 1]);

          if (d2 <= best) {
            best = d2;
            *sx = f->xy[v * 2];
            *sy = f->xy[v * 2 + 1];
            found = TRUE;
          }
        }

        if ((e->snap_mode & OSMIUM_SNAP_SEGMENT) && v + 1 < end) {
          const gboolean touches_dragged =
              e->drag == 2 && f->fid == e->vertex_fid && p == e->vertex_part &&
              (canonical_index(f, p, v) == canonical_index(f, p, e->vertex_index) ||
               canonical_index(f, p, v + 1) == canonical_index(f, p, e->vertex_index));
          double cx, cy;

          if (touches_dragged) {
            continue;
          }

          const double d2 = segment_nearest(nx, ny, f->xy[v * 2], f->xy[v * 2 + 1], f->xy[(v + 1) * 2],
                                            f->xy[(v + 1) * 2 + 1], &cx, &cy);

          /* a vertex found on the way wins over the middle of a segment */
          if (d2 < best) {
            best = d2;
            *sx = cx;
            *sy = cy;
            found = TRUE;
          }
        }
      }
    }
  }

  osmium_vector_data_end_read(self->data);
  return found;
}

static void update_cursor(OsmiumVectorLayer *self, double x, double y) {
  OsmiumEditorState *e = &self->editor;
  double nx, ny;

  e->has_cursor = TRUE;
  e->cursor_x = x;
  e->cursor_y = y;
  osmium_map_widget_to_normalized(self->map, x, y, &nx, &ny);
  e->has_snap = snap_find(self, nx, ny, &e->snap_nx, &e->snap_ny);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* pointer position in normalized coordinates, snapped when a snap target is close */
static void pointer_position(OsmiumVectorLayer *self, double x, double y, double *nx, double *ny) {
  osmium_map_widget_to_normalized(self->map, x, y, nx, ny);

  if (snap_find(self, *nx, *ny, &self->editor.snap_nx, &self->editor.snap_ny)) {
    *nx = self->editor.snap_nx;
    *ny = self->editor.snap_ny;
    self->editor.has_snap = TRUE;
  } else {
    self->editor.has_snap = FALSE;
  }
}

/* --- committing edits ----------------------------------------------------------------------- */

static void committed(OsmiumVectorLayer *self) {
  osmium_vector_layer_refresh(self);
}

static void finish_digitizing(OsmiumVectorLayer *self) {
  OsmiumEditorState *e = &self->editor;
  const OsmiumVectorKind kind = data_kind(self);
  const size_t n = e->draw_points->len / 2;
  const size_t minimum = kind == OSMIUM_VECTOR_POINT ? 1 : (kind == OSMIUM_VECTOR_LINE ? 2 : 3);

  if (n < minimum) {
    return;
  }

  const size_t total = kind == OSMIUM_VECTOR_POLYGON ? n + 1 : n;
  double *ll = g_new(double, total * 2);
  size_t part_start[2] = {0, total};
  unsigned char role[1] = {0};

  memcpy(ll, e->draw_points->data, n * 2 * sizeof(double));
  if (kind == OSMIUM_VECTOR_POLYGON) {
    ll[n * 2] = ll[0];
    ll[n * 2 + 1] = ll[1];
  }

  if (convert_to_lonlat(self, ll, total)) {
    OsmiumGeometry geometry = {.kind = kind, .ll = ll, .n_points = total, .part_start = part_start,
                               .part_role = role, .n_parts = 1};
    const long long fid = osmium_edit_add_feature(self->session, &geometry);

    if (fid != 0) {
      gint64 selected = fid;

      g_array_set_size(e->draw_points, 0);
      osmium_vector_layer_set_selection(self, &selected, 1);
      committed(self);
      osmium_vector_layer_emit_feature_added(self, selected);
    }
  }

  g_free(ll);
}

static void commit_move(OsmiumVectorLayer *self) {
  OsmiumEditorState *e = &self->editor;
  GArray *fids = g_array_new(FALSE, FALSE, sizeof(gint64));

  for (guint i = 0; i < self->selection->len; i++) {
    g_array_append_val(fids, g_array_index(self->selection, gint64, i));
  }

  osmium_edit_session_begin_group(self->session);

  for (guint i = 0; i < fids->len; i++) {
    const long long fid = g_array_index(fids, gint64, i);
    const ptrdiff_t index = osmium_vector_data_find_fid(self->data, fid);
    OsmiumGeometry geometry;

    if (index < 0 || self->data->features[index].deleted ||
        !osmium_vector_data_copy_geometry(self->data, (size_t)index, &geometry)) {
      continue;
    }

    const OsmiumVectorFeature *f = &self->data->features[index];
    double *xy = g_new(double, geometry.n_points * 2);

    for (size_t v = 0; v < geometry.n_points; v++) {
      xy[v * 2] = f->xy[v * 2] + e->drag_dx;
      xy[v * 2 + 1] = f->xy[v * 2 + 1] + e->drag_dy;
    }

    if (convert_to_lonlat(self, xy, geometry.n_points)) {
      memcpy(geometry.ll, xy, geometry.n_points * 2 * sizeof(double));
      osmium_edit_set_geometry(self->session, fid, &geometry);
    }

    g_free(xy);
    geometry_free_view(&geometry);
  }

  osmium_edit_session_end_group(self->session);
  g_array_unref(fids);
  committed(self);
}

static void commit_vertex_move(OsmiumVectorLayer *self) {
  OsmiumEditorState *e = &self->editor;
  const ptrdiff_t index = osmium_vector_data_find_fid(self->data, e->vertex_fid);
  OsmiumGeometry geometry;

  if (index < 0) {
    return;
  }

  if (replace_vertex(self, &self->data->features[index], e->vertex_part, e->vertex_index, e->vertex_nx, e->vertex_ny, &geometry)) {
    osmium_edit_set_geometry(self->session, e->vertex_fid, &geometry);
    geometry_free_view(&geometry);
    committed(self);
  }
}

static gboolean find_vertex_handle(OsmiumVectorLayer *self, double x, double y, gint64 *fid, size_t *part, size_t *vertex) {
  gboolean found = FALSE;
  double best = EDIT_HANDLE_PICK_PX * EDIT_HANDLE_PICK_PX;

  if (!read_lock(self)) {
    return FALSE;
  }

  for (guint s = 0; s < self->selected_indices->len; s++) {
    const OsmiumVectorFeature *f = &self->data->features[g_array_index(self->selected_indices, size_t, s)];

    if (f->deleted) {
      continue;
    }
    for (size_t p = 0; p < f->n_parts; p++) {
      for (size_t v = f->part_start[p]; v < f->part_start[p + 1]; v++) {
        double wx, wy;

        osmium_map_normalized_to_widget(self->map, f->xy[v * 2], f->xy[v * 2 + 1], &wx, &wy);

        const double d2 = (wx - x) * (wx - x) + (wy - y) * (wy - y);

        if (d2 <= best) {
          best = d2;
          *fid = f->fid;
          *part = p;
          *vertex = canonical_index(f, p, v);
          found = TRUE;
        }
      }
    }
  }

  osmium_vector_data_end_read(self->data);
  return found;
}

static gboolean insert_at_segment(OsmiumVectorLayer *self, double x, double y) {
  double nx, ny;
  gboolean done = FALSE;

  osmium_map_widget_to_normalized(self->map, x, y, &nx, &ny);

  if (!read_lock(self)) {
    return FALSE;
  }

  const double tol = px_to_norm(self, EDIT_FEATURE_PICK_PX);
  double best = tol * tol;
  gint64 best_fid = 0;
  size_t best_part = 0, best_vertex = 0;
  double bx = 0, by = 0;

  for (guint s = 0; s < self->selected_indices->len; s++) {
    const OsmiumVectorFeature *f = &self->data->features[g_array_index(self->selected_indices, size_t, s)];

    if (f->deleted || f->kind == OSMIUM_VECTOR_POINT) {
      continue;
    }
    for (size_t p = 0; p < f->n_parts; p++) {
      for (size_t v = f->part_start[p]; v + 1 < f->part_start[p + 1]; v++) {
        double cx, cy;
        const double d2 = segment_nearest(nx, ny, f->xy[v * 2], f->xy[v * 2 + 1], f->xy[(v + 1) * 2],
                                          f->xy[(v + 1) * 2 + 1], &cx, &cy);

        if (d2 < best) {
          best = d2;
          best_fid = f->fid;
          best_part = p;
          best_vertex = v;
          bx = cx;
          by = cy;
          done = TRUE;
        }
      }
    }
  }

  osmium_vector_data_end_read(self->data);

  if (done) {
    const ptrdiff_t index = osmium_vector_data_find_fid(self->data, best_fid);
    OsmiumGeometry geometry;

    if (index >= 0 && insert_vertex_geometry(self, &self->data->features[index], best_part, best_vertex, bx, by, &geometry)) {
      osmium_edit_set_geometry(self->session, best_fid, &geometry);
      geometry_free_view(&geometry);
      committed(self);
    }
  }
  return done;
}

static gboolean delete_active_vertex(OsmiumVectorLayer *self) {
  OsmiumEditorState *e = &self->editor;

  if (!e->has_active_vertex) {
    return FALSE;
  }

  const ptrdiff_t index = osmium_vector_data_find_fid(self->data, e->active_fid);
  OsmiumGeometry geometry;

  if (index < 0 || !delete_vertex_geometry(self, &self->data->features[index], e->active_part, e->active_index, &geometry)) {
    return FALSE;
  }

  osmium_edit_set_geometry(self->session, e->active_fid, &geometry);
  geometry_free_view(&geometry);
  e->has_active_vertex = FALSE;
  committed(self);
  return TRUE;
}

/* --- pointer and key handling --------------------------------------------------------------- */

static gboolean on_press(OsmiumVectorLayer *self, double x, double y, guint state, int n_press) {
  OsmiumEditorState *e = &self->editor;
  const gboolean extend = (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0;
  double nx, ny;

  if (e->tool == OSMIUM_EDIT_TOOL_ADD) {
    if (n_press >= 2) {
      finish_digitizing(self);
      return TRUE;
    }

    pointer_position(self, x, y, &nx, &ny);
    g_array_append_vals(e->draw_points, (double[]){nx, ny}, 2);
    if (data_kind(self) == OSMIUM_VECTOR_POINT) {
      finish_digitizing(self);
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return TRUE;
  }

  osmium_map_widget_to_normalized(self->map, x, y, &nx, &ny);

  if (e->tool == OSMIUM_EDIT_TOOL_VERTEX) {
    gint64 fid;
    size_t part, vertex;

    if (find_vertex_handle(self, x, y, &fid, &part, &vertex)) {
      e->drag = 2;
      e->vertex_fid = fid;
      e->vertex_part = part;
      e->vertex_index = vertex;
      e->vertex_nx = nx;
      e->vertex_ny = ny;
      e->drag_moved = FALSE;
      e->has_active_vertex = TRUE;
      e->active_fid = fid;
      e->active_part = part;
      e->active_index = vertex;
      gtk_widget_queue_draw(GTK_WIDGET(self));
      return TRUE;
    }

    e->has_active_vertex = FALSE;
    if (n_press >= 2 && insert_at_segment(self, x, y)) {
      return TRUE;
    }
  }

  const gint64 hit = pick_feature(self, nx, ny);

  if (hit == G_MININT64) {
    if (!extend) {
      osmium_vector_layer_set_selection(self, NULL, 0);
    }
    return TRUE;
  }

  if (extend) {
    GArray *next = g_array_new(FALSE, FALSE, sizeof(gint64));

    for (guint i = 0; i < self->selection->len; i++) {
      if (g_array_index(self->selection, gint64, i) != hit) {
        g_array_append_val(next, g_array_index(self->selection, gint64, i));
      }
    }
    if (!selection_has(self, hit)) {
      g_array_append_val(next, hit);
    }
    osmium_vector_layer_set_selection(self, (gint64 *)next->data, next->len);
    g_array_unref(next);
  } else if (!selection_has(self, hit)) {
    set_selection_single(self, hit);
  }

  if (e->tool == OSMIUM_EDIT_TOOL_MOVE && selection_has(self, hit)) {
    e->drag = 1;
    e->drag_start_nx = nx;
    e->drag_start_ny = ny;
    e->drag_dx = e->drag_dy = 0.0;
    e->drag_moved = FALSE;
  }
  return TRUE;
}

static gboolean on_motion(OsmiumVectorLayer *self, double x, double y, guint state) {
  OsmiumEditorState *e = &self->editor;

  if (e->drag == 0) {
    update_cursor(self, x, y);
    return TRUE;
  }

  double nx, ny;

  pointer_position(self, x, y, &nx, &ny);

  if (e->drag == 1) {
    e->drag_dx = nx - e->drag_start_nx;
    e->drag_dy = ny - e->drag_start_ny;
    e->drag_moved = e->drag_moved || hypot(e->drag_dx, e->drag_dy) * world_size(self) > EDIT_MOVE_SLOP_PX;
  } else if (e->drag == 2) {
    e->vertex_nx = nx;
    e->vertex_ny = ny;
    e->drag_moved = TRUE;
  }

  e->has_cursor = TRUE;
  e->cursor_x = x;
  e->cursor_y = y;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  return TRUE;
}

static gboolean on_release(OsmiumVectorLayer *self) {
  OsmiumEditorState *e = &self->editor;
  const int drag = e->drag;

  e->drag = 0;
  if (drag == 1 && e->drag_moved) {
    commit_move(self);
  } else if (drag == 2 && e->drag_moved) {
    commit_vertex_move(self);
  }

  e->drag_moved = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  return drag != 0 || e->tool != OSMIUM_EDIT_TOOL_NONE;
}

static gboolean on_tool_event(OsmiumMap *map, guint type, double x, double y, guint button, guint state, int n_press,
                              gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(user_data);

  if (self->session == NULL || self->data == NULL || self->editor.tool == OSMIUM_EDIT_TOOL_NONE) {
    return FALSE;
  }

  switch ((OsmiumToolEventType)type) {
  case OSMIUM_TOOL_PRESS:
    return on_press(self, x, y, state, n_press);
  case OSMIUM_TOOL_MOTION:
    return on_motion(self, x, y, state);
  case OSMIUM_TOOL_RELEASE:
    return on_release(self);
  default:
    return FALSE;
  }
}

static gboolean on_tool_key(OsmiumMap *map, guint keyval, guint state, gpointer user_data) {
  OsmiumVectorLayer *self = OSMIUM_VECTOR_LAYER(user_data);
  OsmiumEditorState *e = &self->editor;

  if (self->session == NULL) {
    return FALSE;
  }

  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
    if (state & GDK_SHIFT_MASK) {
      osmium_vector_layer_redo(self);
    } else {
      osmium_vector_layer_undo(self);
    }
    return TRUE;
  }
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_y || keyval == GDK_KEY_Y)) {
    osmium_vector_layer_redo(self);
    return TRUE;
  }

  switch (keyval) {
  case GDK_KEY_Escape:
    if (e->draw_points->len > 0 || e->drag != 0) {
      g_array_set_size(e->draw_points, 0);
      e->drag = 0;
      gtk_widget_queue_draw(GTK_WIDGET(self));
      return TRUE;
    }
    return FALSE;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
    if (e->draw_points->len > 0) {
      finish_digitizing(self);
      return TRUE;
    }
    return FALSE;
  case GDK_KEY_BackSpace:
    if (e->draw_points->len >= 2) {
      g_array_set_size(e->draw_points, e->draw_points->len - 2);
      gtk_widget_queue_draw(GTK_WIDGET(self));
      return TRUE;
    }
    return FALSE;
  case GDK_KEY_Delete:
  case GDK_KEY_KP_Delete:
    if (e->tool == OSMIUM_EDIT_TOOL_VERTEX && delete_active_vertex(self)) {
      return TRUE;
    }
    return osmium_vector_layer_delete_selected(self) > 0;
  default:
    return FALSE;
  }
}

void osmium_editor_attach(OsmiumVectorLayer *self, OsmiumMap *map) {
  OsmiumEditorState *e = &self->editor;

  e->tool_handler = g_signal_connect(map, "tool-event", G_CALLBACK(on_tool_event), self);
  e->key_handler = g_signal_connect(map, "tool-key", G_CALLBACK(on_tool_key), self);
  osmium_map_set_captured(map, self->session != NULL && e->tool != OSMIUM_EDIT_TOOL_NONE);
}

void osmium_editor_detach(OsmiumVectorLayer *self, OsmiumMap *map) {
  OsmiumEditorState *e = &self->editor;

  if (e->tool_handler != 0) {
    g_signal_handler_disconnect(map, e->tool_handler);
    g_signal_handler_disconnect(map, e->key_handler);
    e->tool_handler = e->key_handler = 0;
  }
  osmium_map_set_captured(map, FALSE);
}

void osmium_editor_init(OsmiumVectorLayer *self) {
  self->editor.snap_mode = OSMIUM_SNAP_VERTEX | OSMIUM_SNAP_SEGMENT;
  self->editor.snap_tolerance = 10.0;
  self->editor.draw_points = g_array_new(FALSE, FALSE, sizeof(double));
}

void osmium_editor_finalize(OsmiumVectorLayer *self) {
  g_clear_pointer(&self->editor.draw_points, g_array_unref);
}

/* --- drawing -------------------------------------------------------------------------------- */

static void add_polyline(GskPathBuilder *builder, const double *xy, size_t n, double scale, double cx, double cy,
                         double dx, double dy, gboolean close) {
  for (size_t i = 0; i < n; i++) {
    const float x = (float)((xy[i * 2] + dx) * scale - cx);
    const float y = (float)((xy[i * 2 + 1] + dy) * scale - cy);

    if (i == 0) {
      gsk_path_builder_move_to(builder, x, y);
    } else {
      gsk_path_builder_line_to(builder, x, y);
    }
  }
  if (close && n > 2) {
    gsk_path_builder_close(builder);
  }
}

static void add_feature_path(GskPathBuilder *builder, const OsmiumVectorFeature *f, double scale, double cx, double cy,
                             double dx, double dy, gboolean substitute, size_t vp, size_t vi, double vx, double vy) {
  for (size_t p = 0; p < f->n_parts; p++) {
    const size_t start = f->part_start[p];
    const size_t end = f->part_start[p + 1];

    if (f->kind == OSMIUM_VECTOR_POINT) {
      gsk_path_builder_add_circle(builder,
                                  &GRAPHENE_POINT_INIT((float)((f->xy[start * 2] + dx) * scale - cx),
                                                       (float)((f->xy[start * 2 + 1] + dy) * scale - cy)),
                                  6.0f);
      continue;
    }

    for (size_t v = start; v < end; v++) {
      double x = f->xy[v * 2] + dx;
      double y = f->xy[v * 2 + 1] + dy;

      if (substitute && p == vp && canonical_index(f, p, v) == vi) {
        x = vx;
        y = vy;
      }
      if (v == start) {
        gsk_path_builder_move_to(builder, (float)(x * scale - cx), (float)(y * scale - cy));
      } else {
        gsk_path_builder_line_to(builder, (float)(x * scale - cx), (float)(y * scale - cy));
      }
    }
    if (f->kind == OSMIUM_VECTOR_POLYGON) {
      gsk_path_builder_close(builder);
    }
  }
}

static void add_handle(GskPathBuilder *builder, double nx, double ny, double scale, double cx, double cy, float half) {
  graphene_rect_t rect = GRAPHENE_RECT_INIT((float)(nx * scale - cx) - half, (float)(ny * scale - cy) - half, half * 2, half * 2);

  gsk_path_builder_add_rect(builder, &rect);
}

void osmium_editor_draw(OsmiumVectorLayer *self, GtkSnapshot *snapshot, float width, float height, double center_x,
                        double center_y) {
  OsmiumEditorState *e = &self->editor;
  static const GdkRGBA preview = {0.88f, 0.11f, 0.14f, 1.0f};
  static const GdkRGBA preview_fill = {0.88f, 0.11f, 0.14f, 0.2f};
  static const GdkRGBA handle_fill = {1.0f, 1.0f, 1.0f, 1.0f};
  static const GdkRGBA handle_line = {0.11f, 0.44f, 0.85f, 1.0f};
  static const GdkRGBA active_fill = {0.88f, 0.11f, 0.14f, 1.0f};
  static const GdkRGBA snap_color = {0.9f, 0.1f, 0.8f, 1.0f};

  if (self->session == NULL || self->data == NULL || e->tool == OSMIUM_EDIT_TOOL_NONE) {
    return;
  }
  if (!osmium_vector_data_try_begin_read(self->data, osmium_map_get_projection(self->map))) {
    return;
  }

  const double scale = world_size(self);
  GskPathBuilder *shapes = gsk_path_builder_new();
  GskPathBuilder *handles = gsk_path_builder_new();
  GskPathBuilder *active = gsk_path_builder_new();
  GskPathBuilder *markers = gsk_path_builder_new();
  gboolean has_shapes = FALSE;
  size_t handle_budget = EDIT_MAX_HANDLES;

  for (guint s = 0; s < self->selected_indices->len; s++) {
    const OsmiumVectorFeature *f = &self->data->features[g_array_index(self->selected_indices, size_t, s)];

    if (f->deleted) {
      continue;
    }

    if (e->drag == 1) {
      add_feature_path(shapes, f, scale, center_x, center_y, e->drag_dx, e->drag_dy, FALSE, 0, 0, 0, 0);
      has_shapes = TRUE;
    } else if (e->drag == 2 && f->fid == e->vertex_fid) {
      add_feature_path(shapes, f, scale, center_x, center_y, 0, 0, TRUE, e->vertex_part, e->vertex_index, e->vertex_nx,
                       e->vertex_ny);
      has_shapes = TRUE;
    }

    if (e->tool == OSMIUM_EDIT_TOOL_VERTEX) {
      for (size_t p = 0; p < f->n_parts; p++) {
        for (size_t v = f->part_start[p]; v < f->part_start[p + 1] && handle_budget > 0; v++) {
          if (is_ring(f) && v == f->part_start[p + 1] - 1) {
            continue;
          }
          const gboolean is_active = e->has_active_vertex && f->fid == e->active_fid && p == e->active_part &&
                                     canonical_index(f, p, v) == e->active_index;

          add_handle(is_active ? active : handles, f->xy[v * 2], f->xy[v * 2 + 1], scale, center_x, center_y,
                     is_active ? 5.0f : 4.0f);
          handle_budget--;
        }
      }
    }
  }

  const size_t n_draw = e->draw_points->len / 2;

  if (e->tool == OSMIUM_EDIT_TOOL_ADD && n_draw > 0) {
    const double *pts = (const double *)e->draw_points->data;
    GskPathBuilder *line = shapes;

    add_polyline(line, pts, n_draw, scale, center_x, center_y, 0, 0, FALSE);
    if (e->has_cursor) {
      double nx, ny;

      if (e->has_snap) {
        nx = e->snap_nx;
        ny = e->snap_ny;
      } else {
        osmium_map_widget_to_normalized(self->map, e->cursor_x, e->cursor_y, &nx, &ny);
      }
      gsk_path_builder_line_to(line, (float)(nx * scale - center_x), (float)(ny * scale - center_y));
      if (data_kind(self) == OSMIUM_VECTOR_POLYGON && n_draw > 1) {
        gsk_path_builder_line_to(line, (float)(pts[0] * scale - center_x), (float)(pts[1] * scale - center_y));
      }
    }
    has_shapes = TRUE;
    for (size_t i = 0; i < n_draw; i++) {
      add_handle(handles, pts[i * 2], pts[i * 2 + 1], scale, center_x, center_y, 4.0f);
    }
  }

  if (e->has_snap && (e->tool == OSMIUM_EDIT_TOOL_ADD || e->drag != 0)) {
    add_handle(markers, e->snap_nx, e->snap_ny, scale, center_x, center_y, 7.0f);
  } else if (e->has_snap && e->has_cursor) {
    add_handle(markers, e->snap_nx, e->snap_ny, scale, center_x, center_y, 7.0f);
  }

  GskPath *shape_path = gsk_path_builder_free_to_path(shapes);
  GskPath *handle_path = gsk_path_builder_free_to_path(handles);
  GskPath *active_path = gsk_path_builder_free_to_path(active);
  GskPath *marker_path = gsk_path_builder_free_to_path(markers);
  GskStroke *thick = gsk_stroke_new(2.0f);
  GskStroke *thin = gsk_stroke_new(1.0f);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0f, height / 2.0f));
  gtk_snapshot_rotate(snapshot, -(float)osmium_map_get_bearing(self->map));

  if (has_shapes && !gsk_path_is_empty(shape_path)) {
    if (e->drag != 0 || data_kind(self) == OSMIUM_VECTOR_POLYGON) {
      gtk_snapshot_append_fill(snapshot, shape_path, GSK_FILL_RULE_EVEN_ODD, &preview_fill);
    }
    gtk_snapshot_append_stroke(snapshot, shape_path, thick, &preview);
  }
  if (!gsk_path_is_empty(handle_path)) {
    gtk_snapshot_append_fill(snapshot, handle_path, GSK_FILL_RULE_WINDING, &handle_fill);
    gtk_snapshot_append_stroke(snapshot, handle_path, thin, &handle_line);
  }
  if (!gsk_path_is_empty(active_path)) {
    gtk_snapshot_append_fill(snapshot, active_path, GSK_FILL_RULE_WINDING, &active_fill);
  }
  if (!gsk_path_is_empty(marker_path)) {
    gtk_snapshot_append_stroke(snapshot, marker_path, thick, &snap_color);
  }

  gtk_snapshot_restore(snapshot);

  gsk_stroke_free(thick);
  gsk_stroke_free(thin);
  gsk_path_unref(shape_path);
  gsk_path_unref(handle_path);
  gsk_path_unref(active_path);
  gsk_path_unref(marker_path);
  osmium_vector_data_end_read(self->data);
}

/* --- public API ----------------------------------------------------------------------------- */

gboolean osmium_vector_layer_is_editing(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->session != NULL;
}

gboolean osmium_vector_layer_has_unsaved_edits(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->session != NULL && osmium_edit_session_is_dirty(self->session);
}

OsmiumGeometryKind osmium_vector_layer_get_geometry_kind(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), OSMIUM_GEOMETRY_POLYGON);

  if (self->data == NULL) {
    return OSMIUM_GEOMETRY_POLYGON;
  }
  return self->data->geometry_kind == OSMIUM_VECTOR_POINT  ? OSMIUM_GEOMETRY_POINT
         : self->data->geometry_kind == OSMIUM_VECTOR_LINE ? OSMIUM_GEOMETRY_LINE
                                                           : OSMIUM_GEOMETRY_POLYGON;
}

gboolean osmium_vector_layer_start_editing(OsmiumVectorLayer *self, GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  if (self->session != NULL) {
    return TRUE;
  }
  if (self->data == NULL || !osmium_vector_layer_is_editable(self)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
                        "This layer can't be edited (remote, not loaded yet or not writable)");
    return FALSE;
  }

  self->session = osmium_edit_session_new(self->data);
  g_array_set_size(self->editor.draw_points, 0);
  self->editor.drag = 0;
  self->editor.has_active_vertex = FALSE;
  osmium_vector_layer_emit_editing_changed(self);
  return TRUE;
}

gboolean osmium_vector_layer_save_edits(OsmiumVectorLayer *self, GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  char message[256] = "";

  if (self->session == NULL) {
    return TRUE;
  }
  if (!osmium_edit_session_is_dirty(self->session)) {
    return TRUE;
  }

  if (osmium_edit_save(self->session, message, sizeof(message)) != OSMIUM_OK) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, *message != '\0' ? message : "Could not write the file");
    return FALSE;
  }

  for (guint i = self->selection->len; i > 0; i--) {
    if (g_array_index(self->selection, gint64, i - 1) < 0) {
      g_array_remove_index(self->selection, i - 1);
    }
  }
  osmium_vector_layer_reload(self);
  osmium_vector_layer_emit_editing_changed(self);
  return TRUE;
}

gboolean osmium_vector_layer_stop_editing(OsmiumVectorLayer *self, gboolean save, GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  if (self->session == NULL) {
    return TRUE;
  }

  if (save && !osmium_vector_layer_save_edits(self, error)) {
    return FALSE;
  }

  if (self->session != NULL) {
    osmium_edit_discard(self->session);
    g_clear_pointer(&self->session, osmium_edit_session_free);
  }

  self->editor.tool = OSMIUM_EDIT_TOOL_NONE;
  self->editor.drag = 0;
  g_array_set_size(self->editor.draw_points, 0);
  if (self->map != NULL) {
    osmium_map_set_captured(self->map, FALSE);
  }
  osmium_vector_layer_refresh(self);
  return TRUE;
}

OsmiumEditTool osmium_vector_layer_get_edit_tool(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), OSMIUM_EDIT_TOOL_NONE);

  return self->editor.tool;
}

void osmium_vector_layer_set_edit_tool(OsmiumVectorLayer *self, OsmiumEditTool tool) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->session == NULL) {
    tool = OSMIUM_EDIT_TOOL_NONE;
  }

  self->editor.tool = tool;
  self->editor.drag = 0;
  self->editor.has_active_vertex = FALSE;
  self->editor.has_snap = FALSE;
  g_array_set_size(self->editor.draw_points, 0);
  if (self->map != NULL) {
    osmium_map_set_captured(self->map, tool != OSMIUM_EDIT_TOOL_NONE);
  }
  gtk_widget_queue_draw(GTK_WIDGET(self));
  osmium_vector_layer_emit_editing_changed(self);
}

guint osmium_vector_layer_get_snap_mode(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0);

  return self->editor.snap_mode;
}

void osmium_vector_layer_set_snap_mode(OsmiumVectorLayer *self, guint snap_mode) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  self->editor.snap_mode = snap_mode;
  self->editor.has_snap = FALSE;
  osmium_vector_layer_emit_editing_changed(self);
}

double osmium_vector_layer_get_snap_tolerance(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0.0);

  return self->editor.snap_tolerance;
}

void osmium_vector_layer_set_snap_tolerance(OsmiumVectorLayer *self, double pixels) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  self->editor.snap_tolerance = CLAMP(pixels, 1.0, 50.0);
}

gboolean osmium_vector_layer_can_undo(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->session != NULL && osmium_edit_session_can_undo(self->session);
}

gboolean osmium_vector_layer_can_redo(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return self->session != NULL && osmium_edit_session_can_redo(self->session);
}

void osmium_vector_layer_undo(OsmiumVectorLayer *self) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->session != NULL && osmium_edit_undo(self->session)) {
    self->editor.has_active_vertex = FALSE;
    osmium_vector_layer_refresh(self);
  }
}

void osmium_vector_layer_redo(OsmiumVectorLayer *self) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  if (self->session != NULL && osmium_edit_redo(self->session)) {
    self->editor.has_active_vertex = FALSE;
    osmium_vector_layer_refresh(self);
  }
}

guint osmium_vector_layer_delete_selected(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0);

  guint count = 0;

  if (self->session == NULL || self->selection->len == 0) {
    return 0;
  }

  GArray *fids = g_array_new(FALSE, FALSE, sizeof(gint64));

  g_array_append_vals(fids, self->selection->data, self->selection->len);
  osmium_edit_session_begin_group(self->session);
  for (guint i = 0; i < fids->len; i++) {
    count += osmium_edit_delete_feature(self->session, g_array_index(fids, gint64, i)) ? 1 : 0;
  }
  osmium_edit_session_end_group(self->session);
  g_array_unref(fids);

  g_array_set_size(self->selection, 0);
  self->editor.has_active_vertex = FALSE;
  osmium_vector_layer_refresh(self);
  return count;
}

gboolean osmium_vector_layer_edit_attributes(OsmiumVectorLayer *self,
                                             const gint64 *fids,
                                             const char *const *fields,
                                             const char *const *old_values,
                                             const char *const *new_values,
                                             guint n_edits) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  if (self->session == NULL) {
    return FALSE;
  }

  osmium_edit_session_begin_group(self->session);
  for (guint i = 0; i < n_edits; i++) {
    osmium_edit_set_attribute(self->session, fids[i], fields[i], old_values[i], new_values[i]);
  }
  osmium_edit_session_end_group(self->session);
  osmium_vector_layer_emit_editing_changed(self);
  return TRUE;
}

typedef struct {
  OsmiumVectorData *data;
  OsmiumProjection *projection;
} TopologyJob;

static void topology_job_free(gpointer data) {
  TopologyJob *job = data;

  osmium_vector_data_free(job->data);
  osmium_projection_unref(job->projection);
  g_free(job);
}

static void topology_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  TopologyJob *job = task_data;
  OsmiumTopologyIssue *issues = NULL;
  size_t n_issues = 0;

  if (!osmium_vector_data_begin_read(job->data, job->projection)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "could not read the features");
    return;
  }

  const osmium_error err = osmium_vector_data_check_topology(job->data, &issues, &n_issues);

  osmium_vector_data_end_read(job->data);

  if (err != OSMIUM_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "the topology check failed");
    return;
  }

  GArray *result = g_array_new(FALSE, FALSE, sizeof(OsmiumLayerIssue));

  for (size_t i = 0; i < n_issues; i++) {
    OsmiumLayerIssue issue = {0};

    issue.kind = (OsmiumLayerIssueKind)issues[i].kind;
    issue.fid = issues[i].fid;
    issue.has_other = issues[i].has_other;
    issue.other_fid = issues[i].other_fid;
    issue.lon = issues[i].lon;
    issue.lat = issues[i].lat;
    g_strlcpy(issue.message, issues[i].message, sizeof(issue.message));
    g_array_append_val(result, issue);
  }
  free(issues);

  g_task_return_pointer(task, result, (GDestroyNotify)g_array_unref);
}

void osmium_vector_layer_check_topology_async(OsmiumVectorLayer *self, GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(OSMIUM_IS_VECTOR_LAYER(self));

  GTask *task = g_task_new(self, NULL, callback, user_data);

  if (self->data == NULL || self->map == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "the layer is not loaded");
    g_object_unref(task);
    return;
  }

  TopologyJob *job = g_new0(TopologyJob, 1);

  job->data = osmium_vector_data_ref(self->data);
  job->projection = osmium_projection_ref(osmium_map_get_projection(self->map));
  g_task_set_task_data(task, job, topology_job_free);
  g_task_run_in_thread(task, topology_worker);
  g_object_unref(task);
}

GArray *osmium_vector_layer_check_topology_finish(OsmiumVectorLayer *self, GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

struct _OsmiumFeatureClip {
  OsmiumVectorKind kind;
  GPtrArray *geometries;
  GPtrArray *attributes;
};

static void clip_geometry_free(gpointer data) {
  OsmiumGeometry *geometry = data;

  osmium_geometry_clear(geometry);
  g_free(geometry);
}

void osmium_feature_clip_free(OsmiumFeatureClip *clip) {
  if (clip == NULL) {
    return;
  }
  g_ptr_array_unref(clip->geometries);
  g_ptr_array_unref(clip->attributes);
  g_free(clip);
}

guint osmium_feature_clip_get_n_features(OsmiumFeatureClip *clip) {
  return clip != NULL ? clip->geometries->len : 0;
}

OsmiumFeatureClip *osmium_vector_layer_copy_selected(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  if (self->data == NULL || self->selection->len == 0) {
    return NULL;
  }

  OsmiumFeatureClip *clip = g_new0(OsmiumFeatureClip, 1);

  clip->kind = self->data->geometry_kind;
  clip->geometries = g_ptr_array_new_with_free_func(clip_geometry_free);
  clip->attributes = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);

  for (guint i = 0; i < self->selection->len; i++) {
    const gint64 fid = g_array_index(self->selection, gint64, i);
    const ptrdiff_t index = osmium_vector_data_find_fid(self->data, fid);
    OsmiumGeometry *geometry = g_new0(OsmiumGeometry, 1);

    if (index < 0 || self->data->features[index].deleted ||
        !osmium_vector_data_copy_geometry(self->data, (size_t)index, geometry)) {
      g_free(geometry);
      continue;
    }

    GPtrArray *fields = osmium_vector_layer_get_feature_attributes(self, fid);

    g_ptr_array_add(clip->geometries, geometry);
    g_ptr_array_add(clip->attributes,
                    fields != NULL ? fields : g_ptr_array_new_with_free_func((GDestroyNotify)osmium_feature_field_free));
  }

  if (clip->geometries->len == 0) {
    osmium_feature_clip_free(clip);
    return NULL;
  }
  return clip;
}

gboolean osmium_vector_layer_can_paste(OsmiumVectorLayer *self, OsmiumFeatureClip *clip) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  return clip != NULL && self->session != NULL && self->data != NULL && clip->kind == self->data->geometry_kind;
}

guint osmium_vector_layer_paste(OsmiumVectorLayer *self, OsmiumFeatureClip *clip, double lat, double lon) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), 0);

  if (!osmium_vector_layer_can_paste(self, clip)) {
    return 0;
  }

  double min_lon = G_MAXDOUBLE, max_lon = -G_MAXDOUBLE, min_lat = G_MAXDOUBLE, max_lat = -G_MAXDOUBLE;

  for (guint i = 0; i < clip->geometries->len; i++) {
    const OsmiumGeometry *g = g_ptr_array_index(clip->geometries, i);

    for (size_t v = 0; v < g->n_points; v++) {
      min_lon = MIN(min_lon, g->ll[v * 2]);
      max_lon = MAX(max_lon, g->ll[v * 2]);
      min_lat = MIN(min_lat, g->ll[v * 2 + 1]);
      max_lat = MAX(max_lat, g->ll[v * 2 + 1]);
    }
  }

  const double dlon = lon - (min_lon + max_lon) / 2.0;
  double dlat = lat - (min_lat + max_lat) / 2.0;

  if (max_lat + dlat > 85.0) {
    dlat = 85.0 - max_lat;
  } else if (min_lat + dlat < -85.0) {
    dlat = -85.0 - min_lat;
  }

  g_autoptr(GPtrArray) targets = osmium_vector_layer_get_fields(self);
  g_autoptr(GArray) created = g_array_new(FALSE, FALSE, sizeof(gint64));

  osmium_edit_session_begin_group(self->session);
  for (guint i = 0; i < clip->geometries->len; i++) {
    OsmiumGeometry shifted;
    const OsmiumGeometry *g = g_ptr_array_index(clip->geometries, i);

    if (!osmium_geometry_copy(g, &shifted)) {
      continue;
    }
    for (size_t v = 0; v < shifted.n_points; v++) {
      shifted.ll[v * 2] += dlon;
      shifted.ll[v * 2 + 1] += dlat;
    }

    const long long fid = osmium_edit_add_feature(self->session, &shifted);

    osmium_geometry_clear(&shifted);
    if (fid == 0) {
      continue;
    }

    const GPtrArray *fields = g_ptr_array_index(clip->attributes, i);

    for (guint f = 0; targets != NULL && f < fields->len; f++) {
      const OsmiumFeatureField *field = g_ptr_array_index(fields, f);

      for (guint t = 0; t < targets->len; t++) {
        const OsmiumFeatureField *target = g_ptr_array_index(targets, t);

        if (g_strcmp0(target->name, field->name) == 0 && field->value != NULL) {
          osmium_edit_set_attribute(self->session, fid, field->name, NULL, field->value);
          break;
        }
      }
    }
    g_array_append_val(created, (gint64){fid});
  }
  osmium_edit_session_end_group(self->session);

  osmium_vector_layer_set_selection(self, (gint64 *)created->data, created->len);
  committed(self);
  return created->len;
}
