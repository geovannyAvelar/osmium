/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_SHAPEFILE_LAYER (osmium_shapefile_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumShapefileLayer, osmium_shapefile_layer, OSMIUM, SHAPEFILE_LAYER, GtkWidget)

OSMIUM_API GtkWidget *osmium_shapefile_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_shapefile_layer_get_map(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_map(OsmiumShapefileLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_shapefile_layer_get_path(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_path(OsmiumShapefileLayer *self, const char *path);

OSMIUM_API const GdkRGBA *osmium_shapefile_layer_get_stroke_color(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_stroke_color(OsmiumShapefileLayer *self, const GdkRGBA *color);

OSMIUM_API const GdkRGBA *osmium_shapefile_layer_get_fill_color(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_fill_color(OsmiumShapefileLayer *self, const GdkRGBA *color);

OSMIUM_API double osmium_shapefile_layer_get_line_width(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_line_width(OsmiumShapefileLayer *self, double width);

OSMIUM_API double osmium_shapefile_layer_get_point_radius(OsmiumShapefileLayer *self);
OSMIUM_API void osmium_shapefile_layer_set_point_radius(OsmiumShapefileLayer *self, double radius);

OSMIUM_API guint osmium_shapefile_layer_get_n_features(OsmiumShapefileLayer *self);

OSMIUM_API gboolean osmium_shapefile_layer_get_extent(OsmiumShapefileLayer *self,
                                                      double *lat_min,
                                                      double *lon_min,
                                                      double *lat_max,
                                                      double *lon_max);

G_END_DECLS
