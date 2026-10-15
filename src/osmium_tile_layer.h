/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_TILE_LAYER (osmium_tile_layer_get_type())

G_DECLARE_FINAL_TYPE(OsmiumTileLayer, osmium_tile_layer, OSMIUM, TILE_LAYER, GtkWidget)

/* TMS/XYZ tile layer. Add it as an overlay before the controls. */
OSMIUM_API GtkWidget *osmium_tile_layer_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_tile_layer_get_map(OsmiumTileLayer *self);
OSMIUM_API void osmium_tile_layer_set_map(OsmiumTileLayer *self, OsmiumMap *map);

OSMIUM_API const char *osmium_tile_layer_get_template_url(OsmiumTileLayer *self);
OSMIUM_API void osmium_tile_layer_set_template_url(OsmiumTileLayer *self, const char *template_url);

OSMIUM_API const char *osmium_tile_layer_get_user_agent(OsmiumTileLayer *self);
OSMIUM_API void osmium_tile_layer_set_user_agent(OsmiumTileLayer *self, const char *user_agent);

OSMIUM_API const char *osmium_tile_layer_get_cache_folder(OsmiumTileLayer *self);
OSMIUM_API void osmium_tile_layer_set_cache_folder(OsmiumTileLayer *self, const char *cache_folder);

G_END_DECLS
