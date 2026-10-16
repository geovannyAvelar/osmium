/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "osmium_map.h"

#include "osmium_map_private.h"
#include "proj/projection.h"
#include "tms/tms.h"
#include <math.h>

#define OSMIUM_MAP_PAN_STEP 64.0
#define OSMIUM_MAP_ROTATE_STEP 15.0 

struct _OsmiumMap {
  GtkWidget parent_instance;
  PangoLayout *coordinates_text;

  gdouble bearing; 
  GList *overlays;

  gdouble lat;
  gdouble lon;
  guint zoom;

  OsmiumProjection *projection;
  char *crs;

  double drag_last_x;
  double drag_last_y;
  double press_x;
  double press_y;
  gboolean captured;
  gboolean button_down;
  double drag_start_x;
  double drag_start_y;
};

OSMIUM_API GType osmium_map_get_type(void);

static GtkBuildableIface *parent_buildable_iface;

static void osmium_map_buildable_add_child(GtkBuildable *buildable,
                                           GtkBuilder *builder,
                                           GObject *child,
                                           const char *type) {
  if (GTK_IS_WIDGET(child) && (type == NULL || g_strcmp0(type, "overlay") == 0)) {
    osmium_map_add_overlay(OSMIUM_MAP(buildable), GTK_WIDGET(child));
  } else if (parent_buildable_iface->add_child != NULL) {
    parent_buildable_iface->add_child(buildable, builder, child, type);
  }
}

static void osmium_map_buildable_init(GtkBuildableIface *iface) {
  parent_buildable_iface = g_type_interface_peek_parent(iface);
  iface->add_child = osmium_map_buildable_add_child;
}

G_DEFINE_TYPE_WITH_CODE(OsmiumMap,
                        osmium_map,
                        GTK_TYPE_WIDGET,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_BUILDABLE, osmium_map_buildable_init))

enum { PROP_0, PROP_LAT, PROP_LON, PROP_ZOOM, PROP_BEARING, PROP_CRS, PROP_CAPTURED, N_PROPS };

#define OSMIUM_MAP_CLICK_SLOP 5.0

enum { SIGNAL_VIEW_CHANGED, SIGNAL_MAP_CLICKED, SIGNAL_TOOL_EVENT, SIGNAL_TOOL_KEY, N_SIGNALS };

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

static void osmium_map_dispose(GObject *object) {
  OsmiumMap *self = OSMIUM_MAP(object);

  for (GList *l = self->overlays; l != NULL; l = l->next) {
    gtk_widget_unparent(l->data);
  }
  g_clear_pointer(&self->overlays, g_list_free);

  g_clear_object(&self->coordinates_text);
  g_clear_pointer(&self->projection, osmium_projection_unref);
  g_clear_pointer(&self->crs, g_free);

  G_OBJECT_CLASS(osmium_map_parent_class)->dispose(object);
}

static void osmium_map_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumMap *self = OSMIUM_MAP(widget);

  const float width = gtk_widget_get_width(widget);
  const float height = gtk_widget_get_height(widget);

  GdkRGBA background;
  GdkRGBA coordinates_text_color;

  gdk_rgba_parse(&background, "#263238");
  gdk_rgba_parse(&coordinates_text_color, "#ffffff");

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);

  gtk_snapshot_append_color(snapshot, &background, &bounds);

  for (GList *l = self->overlays; l != NULL; l = l->next) {
    gtk_widget_snapshot_child(widget, l->data, snapshot);
  }

  char *text = g_strdup_printf("%.5f, %.5f  |  zoom %u", self->lat, self->lon, self->zoom);
  pango_layout_set_text(self->coordinates_text, text, -1);
  g_free(text);

  int text_width, text_height;
  pango_layout_get_pixel_size(self->coordinates_text, &text_width, &text_height);

  const float pad_x = 10.0f;
  const float pad_y = 4.0f;
  const float box_x = 8.0f;
  const float box_y = gtk_widget_get_height(widget) - text_height - 2 * pad_y - 8.0f;

  GdkRGBA backdrop = {0.0f, 0.0f, 0.0f, 0.6f};
  graphene_rect_t box =
      GRAPHENE_RECT_INIT(box_x, box_y, text_width + 2 * pad_x, text_height + 2 * pad_y);
  GskRoundedRect rounded;
  gsk_rounded_rect_init_from_rect(&rounded, &box, 6.0f);

  gtk_snapshot_push_rounded_clip(snapshot, &rounded);
  gtk_snapshot_append_color(snapshot, &backdrop, &box);
  gtk_snapshot_pop(snapshot);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(box_x + pad_x, box_y + pad_y));
  gtk_snapshot_append_layout(snapshot, self->coordinates_text, &coordinates_text_color);
  gtk_snapshot_restore(snapshot);
}

static void osmium_map_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  OsmiumMap *self = OSMIUM_MAP(widget);

  GTK_WIDGET_CLASS(osmium_map_parent_class)->size_allocate(widget, width, height, baseline);

  GtkAllocation area = {0, 0, width, height};
  for (GList *l = self->overlays; l != NULL; l = l->next) {
    gtk_widget_size_allocate(l->data, &area, -1);
  }
}

static void osmium_map_view_changed(OsmiumMap *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  self->lat = CLAMP(self->lat, -85.0, 85.0);
  self->lon = CLAMP(self->lon, -180.0, 180.0);
  self->zoom = MIN(self->zoom, OSMIUM_MAP_MAX_ZOOM);
  self->bearing = fmod(fmod(self->bearing, 360.0) + 360.0, 360.0);

  gtk_widget_queue_draw(widget);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LAT]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LON]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ZOOM]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BEARING]);

  g_signal_emit(self, signals[SIGNAL_VIEW_CHANGED], 0);
}

static double world_size(guint zoom) {
  return 256.0 * ldexp(1.0, (int)zoom);
}

void osmium_map_lonlat_to_world(OsmiumMap *self, double lat, double lon, guint zoom, double *px, double *py) {
  double nx = 0.5;
  double ny = 0.5;

  g_return_if_fail(OSMIUM_IS_MAP(self));

  if (!osmium_projection_forward(self->projection, lon, lat, &nx, &ny)) {
    nx = ny = 0.5;
  }
  *px = nx * world_size(zoom);
  *py = ny * world_size(zoom);
}

void osmium_map_world_to_lonlat(OsmiumMap *self, double px, double py, guint zoom, double *lat, double *lon) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  if (!osmium_projection_inverse(self->projection, px / world_size(zoom), py / world_size(zoom), lon, lat)) {
    *lat = self->lat;
    *lon = self->lon;
  }
}

void osmium_map_widget_to_normalized(OsmiumMap *self, double x, double y, double *nx, double *ny) {
  double cx, cy;

  g_return_if_fail(OSMIUM_IS_MAP(self));

  const double dx = x - gtk_widget_get_width(GTK_WIDGET(self)) / 2.0;
  const double dy = y - gtk_widget_get_height(GTK_WIDGET(self)) / 2.0;
  const double rad = self->bearing * G_PI / 180.0;
  const double size = world_size(self->zoom);

  osmium_map_lonlat_to_world(self, self->lat, self->lon, self->zoom, &cx, &cy);
  *nx = (cx + dx * cos(rad) - dy * sin(rad)) / size;
  *ny = (cy + dx * sin(rad) + dy * cos(rad)) / size;
}

void osmium_map_normalized_to_widget(OsmiumMap *self, double nx, double ny, double *x, double *y) {
  double cx, cy;

  g_return_if_fail(OSMIUM_IS_MAP(self));

  const double rad = self->bearing * G_PI / 180.0;
  const double size = world_size(self->zoom);

  osmium_map_lonlat_to_world(self, self->lat, self->lon, self->zoom, &cx, &cy);

  const double wx = nx * size - cx;
  const double wy = ny * size - cy;

  *x = gtk_widget_get_width(GTK_WIDGET(self)) / 2.0 + wx * cos(rad) + wy * sin(rad);
  *y = gtk_widget_get_height(GTK_WIDGET(self)) / 2.0 - wx * sin(rad) + wy * cos(rad);
}

void osmium_map_widget_to_latlon(OsmiumMap *self, double x, double y, double *lat, double *lon) {
  double cx, cy;

  g_return_if_fail(OSMIUM_IS_MAP(self));

  const double dx = x - gtk_widget_get_width(GTK_WIDGET(self)) / 2.0;
  const double dy = y - gtk_widget_get_height(GTK_WIDGET(self)) / 2.0;
  const double rad = self->bearing * G_PI / 180.0;
  const double world_dx = dx * cos(rad) - dy * sin(rad);
  const double world_dy = dx * sin(rad) + dy * cos(rad);

  osmium_map_lonlat_to_world(self, self->lat, self->lon, self->zoom, &cx, &cy);
  osmium_map_world_to_lonlat(self, cx + world_dx, cy + world_dy, self->zoom, lat, lon);
}

static gboolean emit_tool_event(OsmiumMap *self, guint type, double x, double y, guint button, guint state, int n_press) {
  gboolean handled = FALSE;

  g_signal_emit(self, signals[SIGNAL_TOOL_EVENT], 0, type, x, y, button, state, n_press, &handled);
  return handled;
}

static gboolean over_child(OsmiumMap *self, double x, double y) {
  GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT);

  return picked != NULL && picked != GTK_WIDGET(self);
}

static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  if (self->captured) {
    if (over_child(self, x, y)) {
      return;
    }
    gtk_widget_grab_focus(GTK_WIDGET(self));
    self->button_down = TRUE;
    emit_tool_event(self, OSMIUM_TOOL_PRESS, x, y, GDK_BUTTON_PRIMARY,
                    gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)), n_press);
    return;
  }

  self->press_x = x;
  self->press_y = y;
  gtk_widget_grab_focus(GTK_WIDGET(self));
}

static void on_click_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);
  GdkModifierType state =
      gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  double lat, lon;

  if (self->captured) {
    return;
  }

  if (n_press != 1 || hypot(x - self->press_x, y - self->press_y) > OSMIUM_MAP_CLICK_SLOP) {
    return;
  }

  osmium_map_widget_to_latlon(self, x, y, &lat, &lon);
  g_signal_emit(self, signals[SIGNAL_MAP_CLICKED], 0, lat, lon, (guint)state);
}

static void osmium_map_pan(OsmiumMap *self, double dx_px, double dy_px) {
  double cx, cy;

  const double rad = self->bearing * G_PI / 180.0;
  const double world_dx = dx_px * cos(rad) - dy_px * sin(rad);
  const double world_dy = dx_px * sin(rad) + dy_px * cos(rad);

  osmium_map_lonlat_to_world(self, self->lat, self->lon, self->zoom, &cx, &cy);
  osmium_map_world_to_lonlat(self, cx + world_dx, cy + world_dy, self->zoom, &self->lat, &self->lon);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);
  const double step = (state & GDK_SHIFT_MASK) ? OSMIUM_MAP_PAN_STEP * 4 : OSMIUM_MAP_PAN_STEP;

  if (self->captured) {
    gboolean handled = FALSE;

    g_signal_emit(self, signals[SIGNAL_TOOL_KEY], 0, keyval, (guint)state, &handled);
    if (handled) {
      return GDK_EVENT_STOP;
    }
  }

  switch (keyval) {
  case GDK_KEY_Left:
    osmium_map_pan(self, -step, 0);
    break;
  case GDK_KEY_Right:
    osmium_map_pan(self, step, 0);
    break;
  case GDK_KEY_Up:
    osmium_map_pan(self, 0, -step);
    break;
  case GDK_KEY_Down:
    osmium_map_pan(self, 0, step);
    break;
  case GDK_KEY_bracketleft:
    self->bearing += OSMIUM_MAP_ROTATE_STEP;
    break;
  case GDK_KEY_bracketright:
    self->bearing -= OSMIUM_MAP_ROTATE_STEP;
    break;
  case GDK_KEY_plus:
  case GDK_KEY_equal:
    if (self->zoom < OSMIUM_MAP_MAX_ZOOM) {
      self->zoom++;
    }
    break;
  case GDK_KEY_minus:
    if (self->zoom > 0) {
      self->zoom--;
    }
    break;
  default:
    return GDK_EVENT_PROPAGATE;
  }

  osmium_map_view_changed(self);
  return GDK_EVENT_STOP;
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  if (dy < 0 && self->zoom < OSMIUM_MAP_MAX_ZOOM) {
    self->zoom++;
  } else if (dy > 0 && self->zoom > 0) {
    self->zoom--;
  } else {
    return GDK_EVENT_PROPAGATE;
  }

  osmium_map_view_changed(self);
  return GDK_EVENT_STOP;
}

static gboolean drag_pans(OsmiumMap *self, GtkGestureDrag *gesture) {
  return !self->captured || gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_PRIMARY;
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  self->drag_last_x = 0;
  self->drag_last_y = 0;
  self->drag_start_x = x;
  self->drag_start_y = y;
  gtk_widget_grab_focus(GTK_WIDGET(self));
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  if (!drag_pans(self, gesture)) {
    if (!self->button_down) {
      return;
    }
    emit_tool_event(self, OSMIUM_TOOL_MOTION, self->drag_start_x + offset_x, self->drag_start_y + offset_y,
                    GDK_BUTTON_PRIMARY,
                    gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)), 0);
    return;
  }

  osmium_map_pan(self, self->drag_last_x - offset_x, self->drag_last_y - offset_y);

  self->drag_last_x = offset_x;
  self->drag_last_y = offset_y;

  osmium_map_view_changed(self);
}

static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  if (!drag_pans(self, gesture)) {
    if (!self->button_down) {
      return;
    }
    self->button_down = FALSE;
    emit_tool_event(self, OSMIUM_TOOL_RELEASE, self->drag_start_x + offset_x, self->drag_start_y + offset_y,
                    GDK_BUTTON_PRIMARY,
                    gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)), 0);
  }
}

static void on_hover(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  OsmiumMap *self = OSMIUM_MAP(user_data);

  if (self->captured && !self->button_down && !over_child(self, x, y)) {
    emit_tool_event(self, OSMIUM_TOOL_MOTION, x, y, 0,
                    gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)), 0);
  }
}

void osmium_map_set_view(OsmiumMap *self, double lat, double lon, guint zoom) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  self->lat = lat;
  self->lon = lon;
  self->zoom = zoom;

  osmium_map_view_changed(self);
}

double osmium_map_get_lat(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), 0.0);

  return self->lat;
}

void osmium_map_set_lat(OsmiumMap *self, double lat) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  osmium_map_set_view(self, lat, self->lon, self->zoom);
}

double osmium_map_get_lon(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), 0.0);

  return self->lon;
}

void osmium_map_set_lon(OsmiumMap *self, double lon) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  osmium_map_set_view(self, self->lat, lon, self->zoom);
}

guint osmium_map_get_zoom(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), 0);

  return self->zoom;
}

void osmium_map_set_zoom(OsmiumMap *self, guint zoom) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  osmium_map_set_view(self, self->lat, self->lon, zoom);
}

double osmium_map_get_bearing(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), 0.0);

  return self->bearing;
}

void osmium_map_set_bearing(OsmiumMap *self, double bearing) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  self->bearing = bearing;

  osmium_map_view_changed(self);
}

gboolean osmium_map_get_captured(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), FALSE);

  return self->captured;
}

void osmium_map_set_captured(OsmiumMap *self, gboolean captured) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  if (self->captured == captured) {
    return;
  }

  self->captured = captured;
  self->button_down = FALSE;
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CAPTURED]);
}

OsmiumProjection *osmium_map_get_projection(OsmiumMap *self) {
  return self->projection;
}

const char *osmium_map_get_crs(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), NULL);

  return self->crs;
}

gboolean osmium_map_set_crs(OsmiumMap *self, const char *crs) {
  char message[128];

  g_return_val_if_fail(OSMIUM_IS_MAP(self), FALSE);
  g_return_val_if_fail(crs != NULL, FALSE);

  if (g_strcmp0(self->crs, crs) == 0) {
    return TRUE;
  }

  OsmiumProjection *projection = osmium_projection_new(crs, message, sizeof(message));
  if (projection == NULL) {
    return FALSE;
  }

  osmium_projection_unref(self->projection);
  self->projection = projection;
  g_free(self->crs);
  self->crs = g_strdup(crs);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CRS]);
  osmium_map_view_changed(self);
  return TRUE;
}

const char *osmium_map_get_crs_name(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), NULL);

  return osmium_projection_get_name(self->projection);
}

gboolean osmium_map_crs_is_web_mercator(OsmiumMap *self) {
  g_return_val_if_fail(OSMIUM_IS_MAP(self), FALSE);

  return osmium_projection_is_web_mercator(self->projection);
}

void osmium_map_normalized_to_units(OsmiumMap *self, double nx, double ny, double *x, double *y) {
  g_return_if_fail(OSMIUM_IS_MAP(self));

  osmium_projection_normalized_to_units(self->projection, nx, ny, x, y);
}

void osmium_map_add_overlay(OsmiumMap *self, GtkWidget *child) {
  g_return_if_fail(OSMIUM_IS_MAP(self));
  g_return_if_fail(GTK_IS_WIDGET(child));
  g_return_if_fail(gtk_widget_get_parent(child) == NULL);

  gtk_widget_set_parent(child, GTK_WIDGET(self));
  self->overlays = g_list_append(self->overlays, child);

  /* controls that have a "map" property bind to this map automatically */
  GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(child), "map");
  if (pspec != NULL && g_type_is_a(OSMIUM_TYPE_MAP, pspec->value_type)) {
    g_object_set(child, "map", self, NULL);
  }
}

void osmium_map_insert_overlay(OsmiumMap *self, GtkWidget *child, int position) {
  g_return_if_fail(OSMIUM_IS_MAP(self));
  g_return_if_fail(GTK_IS_WIDGET(child));
  g_return_if_fail(gtk_widget_get_parent(child) == NULL);

  gtk_widget_set_parent(child, GTK_WIDGET(self));
  self->overlays = g_list_insert(self->overlays, child, position);

  GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(child), "map");
  if (pspec != NULL && g_type_is_a(OSMIUM_TYPE_MAP, pspec->value_type)) {
    g_object_set(child, "map", self, NULL);
  }
}

void osmium_map_move_overlay(OsmiumMap *self, GtkWidget *child, int position) {
  g_return_if_fail(OSMIUM_IS_MAP(self));
  g_return_if_fail(g_list_find(self->overlays, child) != NULL);

  self->overlays = g_list_remove(self->overlays, child);
  self->overlays = g_list_insert(self->overlays, child, position);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void osmium_map_remove_overlay(OsmiumMap *self, GtkWidget *child) {
  g_return_if_fail(OSMIUM_IS_MAP(self));
  g_return_if_fail(g_list_find(self->overlays, child) != NULL);

  self->overlays = g_list_remove(self->overlays, child);
  gtk_widget_unparent(child);
}

static void osmium_map_set_property(GObject *object,
                                  guint prop_id,
                                  const GValue *value,
                                  GParamSpec *pspec) {
  OsmiumMap *self = OSMIUM_MAP(object);

  switch (prop_id) {
  case PROP_LAT:
    osmium_map_set_lat(self, g_value_get_double(value));
    break;
  case PROP_LON:
    osmium_map_set_lon(self, g_value_get_double(value));
    break;
  case PROP_ZOOM:
    osmium_map_set_zoom(self, g_value_get_uint(value));
    break;
  case PROP_BEARING:
    osmium_map_set_bearing(self, g_value_get_double(value));
    break;
  case PROP_CAPTURED:
    osmium_map_set_captured(self, g_value_get_boolean(value));
    break;
  case PROP_CRS:
    if (g_value_get_string(value) != NULL && !osmium_map_set_crs(self, g_value_get_string(value))) {
      g_warning("Unknown coordinate system %s", g_value_get_string(value));
    }
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_map_get_property(GObject *object,
                                  guint prop_id,
                                  GValue *value,
                                  GParamSpec *pspec) {
  OsmiumMap *self = OSMIUM_MAP(object);

  switch (prop_id) {
  case PROP_LAT:
    g_value_set_double(value, self->lat);
    break;
  case PROP_LON:
    g_value_set_double(value, self->lon);
    break;
  case PROP_ZOOM:
    g_value_set_uint(value, self->zoom);
    break;
  case PROP_CRS:
    g_value_set_string(value, self->crs);
    break;
  case PROP_CAPTURED:
    g_value_set_boolean(value, self->captured);
    break;
  case PROP_BEARING:
    g_value_set_double(value, self->bearing);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_map_class_init(OsmiumMapClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = osmium_map_dispose;
  object_class->set_property = osmium_map_set_property;
  object_class->get_property = osmium_map_get_property;

  properties[PROP_LAT] = g_param_spec_double(
      "lat", NULL, "Latitude of the view center", -85.0, 85.0, 0.0,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_LON] = g_param_spec_double(
      "lon", NULL, "Longitude of the view center", -180.0, 180.0, 0.0,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_ZOOM] = g_param_spec_uint(
      "zoom", NULL, "Zoom level", 0, OSMIUM_MAP_MAX_ZOOM, 4,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_BEARING] = g_param_spec_double(
      "bearing", NULL, "Rotation of the map in degrees clockwise from north-up", 0.0, 360.0, 0.0,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_CRS] = g_param_spec_string(
      "crs", NULL, "Coordinate system the map is drawn in (EPSG:3857 by default)", "EPSG:3857",
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_CAPTURED] = g_param_spec_boolean(
      "captured", NULL, "Whether pointer events go to the active editing tool instead of panning", FALSE,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  signals[SIGNAL_VIEW_CHANGED] = g_signal_new("view-changed",
                                              G_TYPE_FROM_CLASS(klass),
                                              G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL,
                                              G_TYPE_NONE, 0);
  signals[SIGNAL_MAP_CLICKED] = g_signal_new("map-clicked", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_DOUBLE,
                                             G_TYPE_DOUBLE, G_TYPE_UINT);
  signals[SIGNAL_TOOL_EVENT] = g_signal_new("tool-event", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                            g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 6,
                                            G_TYPE_UINT, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_UINT, G_TYPE_UINT,
                                            G_TYPE_INT);
  signals[SIGNAL_TOOL_KEY] = g_signal_new("tool-key", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                          g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 2,
                                          G_TYPE_UINT, G_TYPE_UINT);
  widget_class->snapshot = osmium_map_snapshot;
  widget_class->size_allocate = osmium_map_size_allocate;
}

static void osmium_map_init(OsmiumMap *self) {
  self->lat = 0.0;
  self->lon = 0.0;
  self->zoom = 4;
  self->crs = g_strdup("EPSG:3857");
  self->projection = osmium_projection_new(self->crs, NULL, 0);

  self->coordinates_text = gtk_widget_create_pango_layout(GTK_WIDGET(self), NULL);

  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);

  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
  gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
  g_signal_connect(click, "released", G_CALLBACK(on_click_released), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
  gtk_widget_add_controller(GTK_WIDGET(self), scroll);

  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));

  GtkGesture *pan = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan), GDK_BUTTON_MIDDLE);
  g_signal_connect(pan, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(pan, "drag-update", G_CALLBACK(on_drag_update), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(pan));

  GtkEventController *hover = gtk_event_controller_motion_new();
  g_signal_connect(hover, "motion", G_CALLBACK(on_hover), self);
  gtk_widget_add_controller(GTK_WIDGET(self), hover);

  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_size_request(GTK_WIDGET(self), 200, 100);
}

GtkWidget *osmium_map_new(void) {
  return g_object_new(OSMIUM_TYPE_MAP, NULL);
}
