/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_compass.h"

#include <math.h>

#define COMPASS_SIZE 48
#define DRAG_THRESHOLD 3.0

struct _OsmiumCompass {
  GtkWidget parent_instance;

  OsmiumMap *map;

  double drag_start_x;
  double drag_start_y;
  double drag_start_angle;
  double drag_start_bearing;
  gboolean dragged;
};

OSMIUM_API GType osmium_compass_get_type(void);

G_DEFINE_TYPE(OsmiumCompass, osmium_compass, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_MAP, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void on_map_bearing_changed(GObject *map, GParamSpec *pspec, gpointer user_data) {
  gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void osmium_compass_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumCompass *self = OSMIUM_COMPASS(widget);

  const double width = gtk_widget_get_width(widget);
  const double height = gtk_widget_get_height(widget);
  const double radius = MIN(width, height) / 2.0;
  const double bearing = self->map != NULL ? osmium_map_get_bearing(self->map) : 0.0;

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);

  cairo_translate(cr, width / 2.0, height / 2.0);

  cairo_arc(cr, 0, 0, radius, 0, 2 * G_PI);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
  cairo_fill(cr);

  cairo_rotate(cr, -bearing * G_PI / 180.0);

  cairo_move_to(cr, 0, -radius * 0.75);
  cairo_line_to(cr, radius * 0.22, 0);
  cairo_line_to(cr, -radius * 0.22, 0);
  cairo_close_path(cr);
  cairo_set_source_rgb(cr, 0.90, 0.22, 0.21);
  cairo_fill(cr);

  cairo_move_to(cr, 0, radius * 0.75);
  cairo_line_to(cr, radius * 0.22, 0);
  cairo_line_to(cr, -radius * 0.22, 0);
  cairo_close_path(cr);
  cairo_set_source_rgb(cr, 0.93, 0.94, 0.95);
  cairo_fill(cr);

  cairo_destroy(cr);
}

static double pointer_angle(OsmiumCompass *self, double x, double y) {
  GtkWidget *widget = GTK_WIDGET(self);

  return atan2(y - gtk_widget_get_height(widget) / 2.0, x - gtk_widget_get_width(widget) / 2.0) *
         180.0 / G_PI;
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data) {
  OsmiumCompass *self = OSMIUM_COMPASS(user_data);

  self->drag_start_x = x;
  self->drag_start_y = y;
  self->drag_start_angle = pointer_angle(self, x, y);
  self->drag_start_bearing = self->map != NULL ? osmium_map_get_bearing(self->map) : 0.0;
  self->dragged = FALSE;
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  OsmiumCompass *self = OSMIUM_COMPASS(user_data);

  if (self->map == NULL) {
    return;
  }

  if (!self->dragged && hypot(offset_x, offset_y) < DRAG_THRESHOLD) {
    return;
  }
  self->dragged = TRUE;

  double angle = pointer_angle(self, self->drag_start_x + offset_x, self->drag_start_y + offset_y);

  osmium_map_set_bearing(self->map, self->drag_start_bearing - (angle - self->drag_start_angle));
}

static void
on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  OsmiumCompass *self = OSMIUM_COMPASS(user_data);

  if (!self->dragged && self->map != NULL) {
    osmium_map_set_bearing(self->map, 0.0);
  }
}

OsmiumMap *osmium_compass_get_map(OsmiumCompass *self) {
  g_return_val_if_fail(OSMIUM_IS_COMPASS(self), NULL);

  return self->map;
}

void osmium_compass_set_map(OsmiumCompass *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_COMPASS(self));
  g_return_if_fail(map == NULL || OSMIUM_IS_MAP(map));

  if (self->map == map) {
    return;
  }

  if (self->map != NULL) {
    g_signal_handlers_disconnect_by_data(self->map, self);
    g_clear_weak_pointer(&self->map);
  }

  if (map != NULL) {
    g_set_weak_pointer(&self->map, map);
    g_signal_connect(map, "notify::bearing", G_CALLBACK(on_map_bearing_changed), self);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

static void osmium_compass_dispose(GObject *object) {
  osmium_compass_set_map(OSMIUM_COMPASS(object), NULL);

  G_OBJECT_CLASS(osmium_compass_parent_class)->dispose(object);
}

static void osmium_compass_set_property(GObject *object,
                                        guint prop_id,
                                        const GValue *value,
                                        GParamSpec *pspec) {
  OsmiumCompass *self = OSMIUM_COMPASS(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_compass_set_map(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
osmium_compass_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  OsmiumCompass *self = OSMIUM_COMPASS(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_compass_class_init(OsmiumCompassClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = osmium_compass_dispose;
  object_class->set_property = osmium_compass_set_property;
  object_class->get_property = osmium_compass_get_property;

  properties[PROP_MAP] =
      g_param_spec_object("map",
                          NULL,
                          "The map this compass shows and rotates",
                          OSMIUM_TYPE_MAP,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  widget_class->snapshot = osmium_compass_snapshot;
  gtk_widget_class_set_css_name(widget_class, "osmium-compass");
}

static void osmium_compass_init(OsmiumCompass *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  gtk_widget_set_size_request(widget, COMPASS_SIZE, COMPASS_SIZE);
  gtk_widget_set_halign(widget, GTK_ALIGN_END);
  gtk_widget_set_valign(widget, GTK_ALIGN_START);
  gtk_widget_set_margin_top(widget, 12);
  gtk_widget_set_margin_bottom(widget, 12);
  gtk_widget_set_margin_start(widget, 12);
  gtk_widget_set_margin_end(widget, 12);
  gtk_widget_set_tooltip_text(widget, "Click to point north, drag to rotate");

  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
}

GtkWidget *osmium_compass_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_COMPASS, "map", map, NULL);
}
