/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_zoom_control.h"

struct _OsmiumZoomControl {
  GtkWidget parent_instance;

  GtkWidget *zoom_in;
  GtkWidget *zoom_out;
  OsmiumMap *map; 
};

OSMIUM_API GType osmium_zoom_control_get_type(void);

G_DEFINE_TYPE(OsmiumZoomControl, osmium_zoom_control, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_MAP, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void update_buttons(OsmiumZoomControl *self) {
  guint zoom = self->map != NULL ? osmium_map_get_zoom(self->map) : 0;

  gtk_widget_set_sensitive(self->zoom_in, self->map != NULL && zoom < OSMIUM_MAP_MAX_ZOOM);
  gtk_widget_set_sensitive(self->zoom_out, self->map != NULL && zoom > 0);
}

static void on_map_zoom_changed(GObject *map, GParamSpec *pspec, gpointer user_data) {
  update_buttons(OSMIUM_ZOOM_CONTROL(user_data));
}

static void on_zoom_in(GtkButton *button, gpointer user_data) {
  OsmiumZoomControl *self = OSMIUM_ZOOM_CONTROL(user_data);

  if (self->map != NULL && osmium_map_get_zoom(self->map) < OSMIUM_MAP_MAX_ZOOM) {
    osmium_map_set_zoom(self->map, osmium_map_get_zoom(self->map) + 1);
  }
}

static void on_zoom_out(GtkButton *button, gpointer user_data) {
  OsmiumZoomControl *self = OSMIUM_ZOOM_CONTROL(user_data);

  if (self->map != NULL && osmium_map_get_zoom(self->map) > 0) {
    osmium_map_set_zoom(self->map, osmium_map_get_zoom(self->map) - 1);
  }
}

OsmiumMap *osmium_zoom_control_get_map(OsmiumZoomControl *self) {
  g_return_val_if_fail(OSMIUM_IS_ZOOM_CONTROL(self), NULL);

  return self->map;
}

void osmium_zoom_control_set_map(OsmiumZoomControl *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_ZOOM_CONTROL(self));
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
    g_signal_connect(map, "notify::zoom", G_CALLBACK(on_map_zoom_changed), self);
  }

  update_buttons(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

static void osmium_zoom_control_dispose(GObject *object) {
  OsmiumZoomControl *self = OSMIUM_ZOOM_CONTROL(object);

  osmium_zoom_control_set_map(self, NULL);

  g_clear_pointer(&self->zoom_in, gtk_widget_unparent);
  g_clear_pointer(&self->zoom_out, gtk_widget_unparent);

  G_OBJECT_CLASS(osmium_zoom_control_parent_class)->dispose(object);
}

static void osmium_zoom_control_set_property(GObject *object,
                                             guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec) {
  OsmiumZoomControl *self = OSMIUM_ZOOM_CONTROL(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_zoom_control_set_map(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_zoom_control_get_property(GObject *object,
                                             guint prop_id,
                                             GValue *value,
                                             GParamSpec *pspec) {
  OsmiumZoomControl *self = OSMIUM_ZOOM_CONTROL(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_zoom_control_class_init(OsmiumZoomControlClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = osmium_zoom_control_dispose;
  object_class->set_property = osmium_zoom_control_set_property;
  object_class->get_property = osmium_zoom_control_get_property;

  properties[PROP_MAP] = g_param_spec_object(
      "map", NULL, "The map this control zooms", OSMIUM_TYPE_MAP,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "osmium-zoom-control");
}

static void osmium_zoom_control_init(OsmiumZoomControl *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  gtk_orientable_set_orientation(GTK_ORIENTABLE(gtk_widget_get_layout_manager(widget)),
                                 GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(widget, "linked");

  gtk_widget_set_halign(widget, GTK_ALIGN_END);
  gtk_widget_set_valign(widget, GTK_ALIGN_END);
  gtk_widget_set_margin_top(widget, 12);
  gtk_widget_set_margin_bottom(widget, 12);
  gtk_widget_set_margin_start(widget, 12);
  gtk_widget_set_margin_end(widget, 12);

  self->zoom_in = gtk_button_new_from_icon_name("zoom-in-symbolic");
  gtk_widget_set_tooltip_text(self->zoom_in, "Zoom in");
  gtk_widget_add_css_class(self->zoom_in, "osd");
  gtk_widget_set_focusable(self->zoom_in, FALSE);
  g_signal_connect(self->zoom_in, "clicked", G_CALLBACK(on_zoom_in), self);
  gtk_widget_set_parent(self->zoom_in, widget);

  self->zoom_out = gtk_button_new_from_icon_name("zoom-out-symbolic");
  gtk_widget_set_tooltip_text(self->zoom_out, "Zoom out");
  gtk_widget_add_css_class(self->zoom_out, "osd");
  gtk_widget_set_focusable(self->zoom_out, FALSE);
  g_signal_connect(self->zoom_out, "clicked", G_CALLBACK(on_zoom_out), self);
  gtk_widget_set_parent(self->zoom_out, widget);

  update_buttons(self);
}

GtkWidget *osmium_zoom_control_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_ZOOM_CONTROL, "map", map, NULL);
}
