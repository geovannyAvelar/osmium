/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_scale_bar.h"

#include <math.h>
#include <pango/pangocairo.h>

#define BAR_MAX_WIDTH 120
#define BAR_HEIGHT 30
#define BAR_PADDING 6
#define SAMPLE_WIDTH 100.0
#define METRES_PER_FOOT 0.3048
#define METRES_PER_MILE 1609.344

struct _OsmiumScaleBar {
  GtkWidget parent_instance;

  OsmiumMap *map;
  OsmiumUnitSystem units;
};

OSMIUM_API GType osmium_scale_bar_get_type(void);

G_DEFINE_TYPE(OsmiumScaleBar, osmium_scale_bar, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_MAP, PROP_UNITS, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void on_map_changed(GObject *map, gpointer user_data) {
  gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static double nice_length(double limit) {
  const double magnitude = pow(10.0, floor(log10(limit)));
  const double fraction = limit / magnitude;

  if (fraction >= 5.0) {
    return 5.0 * magnitude;
  }
  return fraction >= 2.0 ? 2.0 * magnitude : magnitude;
}

static double metres_per_pixel(OsmiumScaleBar *self) {
  graphene_point_t origin;
  double lat1, lon1, lat2, lon2;

  if (!gtk_widget_compute_point(GTK_WIDGET(self), GTK_WIDGET(self->map),
                                &GRAPHENE_POINT_INIT(0, gtk_widget_get_height(GTK_WIDGET(self)) / 2.0), &origin)) {
    return 0.0;
  }

  osmium_map_widget_to_latlon(self->map, origin.x, origin.y, &lat1, &lon1);
  osmium_map_widget_to_latlon(self->map, origin.x + SAMPLE_WIDTH, origin.y, &lat2, &lon2);

  return osmium_geodesic_distance(lat1, lon1, lat2, lon2) / SAMPLE_WIDTH;
}

/* Rounds the longest bar that fits to 1, 2 or 5 times a power of ten in the unit shown. */
static double bar_metres(OsmiumUnitSystem units, double limit_metres, char **label) {
  if (units == OSMIUM_UNITS_IMPERIAL) {
    if (limit_metres >= METRES_PER_MILE) {
      const double miles = nice_length(limit_metres / METRES_PER_MILE);

      *label = g_strdup_printf("%g mi", miles);
      return miles * METRES_PER_MILE;
    } else {
      const double feet = nice_length(limit_metres / METRES_PER_FOOT);

      *label = g_strdup_printf("%g ft", feet);
      return feet * METRES_PER_FOOT;
    }
  }

  const double metres = nice_length(limit_metres);

  *label = metres >= 1000.0 ? g_strdup_printf("%g km", metres / 1000.0) : g_strdup_printf("%g m", metres);
  return metres;
}

static void osmium_scale_bar_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  OsmiumScaleBar *self = OSMIUM_SCALE_BAR(widget);
  g_autofree char *label = NULL;

  if (self->map == NULL) {
    return;
  }

  const double mpp = metres_per_pixel(self);

  if (!(mpp > 0.0) || !isfinite(mpp)) {
    return;
  }

  const double metres = bar_metres(self->units, mpp * BAR_MAX_WIDTH, &label);
  const double length = metres / mpp;
  const double width = gtk_widget_get_width(widget);
  const double height = gtk_widget_get_height(widget);
  const double baseline = height - 4.0;
  const double left = BAR_PADDING;
  const double right = left + length;

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
  PangoLayout *layout = gtk_widget_create_pango_layout(widget, label);

  cairo_move_to(cr, left, baseline - 6.0);
  cairo_line_to(cr, left, baseline);
  cairo_line_to(cr, right, baseline);
  cairo_line_to(cr, right, baseline - 6.0);

  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
  cairo_set_line_width(cr, 4.0);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);

  cairo_move_to(cr, left, 1.0);
  pango_cairo_layout_path(cr, layout);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
  cairo_set_line_width(cr, 3.0);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_fill(cr);

  g_object_unref(layout);
  cairo_destroy(cr);
}

OsmiumMap *osmium_scale_bar_get_map(OsmiumScaleBar *self) {
  g_return_val_if_fail(OSMIUM_IS_SCALE_BAR(self), NULL);

  return self->map;
}

void osmium_scale_bar_set_map(OsmiumScaleBar *self, OsmiumMap *map) {
  g_return_if_fail(OSMIUM_IS_SCALE_BAR(self));
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
    g_signal_connect(map, "view-changed", G_CALLBACK(on_map_changed), self);
    g_signal_connect(map, "notify::crs", G_CALLBACK(on_map_changed), self);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAP]);
}

OsmiumUnitSystem osmium_scale_bar_get_units(OsmiumScaleBar *self) {
  g_return_val_if_fail(OSMIUM_IS_SCALE_BAR(self), OSMIUM_UNITS_METRIC);

  return self->units;
}

void osmium_scale_bar_set_units(OsmiumScaleBar *self, OsmiumUnitSystem units) {
  g_return_if_fail(OSMIUM_IS_SCALE_BAR(self));

  if (self->units == units) {
    return;
  }

  self->units = units;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_UNITS]);
}

static void osmium_scale_bar_dispose(GObject *object) {
  osmium_scale_bar_set_map(OSMIUM_SCALE_BAR(object), NULL);

  G_OBJECT_CLASS(osmium_scale_bar_parent_class)->dispose(object);
}

static void osmium_scale_bar_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  OsmiumScaleBar *self = OSMIUM_SCALE_BAR(object);

  switch (prop_id) {
  case PROP_MAP:
    osmium_scale_bar_set_map(self, g_value_get_object(value));
    break;
  case PROP_UNITS:
    osmium_scale_bar_set_units(self, (OsmiumUnitSystem)g_value_get_int(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_scale_bar_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  OsmiumScaleBar *self = OSMIUM_SCALE_BAR(object);

  switch (prop_id) {
  case PROP_MAP:
    g_value_set_object(value, self->map);
    break;
  case PROP_UNITS:
    g_value_set_int(value, (int)self->units);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void osmium_scale_bar_class_init(OsmiumScaleBarClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = osmium_scale_bar_dispose;
  object_class->set_property = osmium_scale_bar_set_property;
  object_class->get_property = osmium_scale_bar_get_property;

  properties[PROP_MAP] = g_param_spec_object("map", NULL, "The map this scale bar measures", OSMIUM_TYPE_MAP,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_UNITS] =
      g_param_spec_int("units", NULL, "Unit system of the scale (0 metric, 1 imperial)", OSMIUM_UNITS_METRIC,
                       OSMIUM_UNITS_IMPERIAL, OSMIUM_UNITS_METRIC,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  widget_class->snapshot = osmium_scale_bar_snapshot;
  gtk_widget_class_set_css_name(widget_class, "osmium-scale-bar");
}

static void osmium_scale_bar_init(OsmiumScaleBar *self) {
  GtkWidget *widget = GTK_WIDGET(self);

  gtk_widget_set_size_request(widget, BAR_MAX_WIDTH + 2 * BAR_PADDING, BAR_HEIGHT);
  gtk_widget_set_halign(widget, GTK_ALIGN_START);
  gtk_widget_set_valign(widget, GTK_ALIGN_END);
  gtk_widget_set_margin_start(widget, 8);
  gtk_widget_set_margin_bottom(widget, 44);
  gtk_widget_set_can_target(widget, FALSE);
}

GtkWidget *osmium_scale_bar_new(OsmiumMap *map) {
  return g_object_new(OSMIUM_TYPE_SCALE_BAR, "map", map, NULL);
}
