/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

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

OSMIUM_API guint osmium_vector_layer_get_n_features(OsmiumVectorLayer *self);

OSMIUM_API gboolean osmium_vector_layer_get_extent(OsmiumVectorLayer *self,
                                                      double *lat_min,
                                                      double *lon_min,
                                                      double *lat_max,
                                                      double *lon_max);

G_END_DECLS
