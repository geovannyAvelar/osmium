/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_COMPASS (osmium_compass_get_type())

G_DECLARE_FINAL_TYPE(OsmiumCompass, osmium_compass, OSMIUM, COMPASS, GtkWidget)

/* Shows where north is. Click it to reset the map to north-up, drag around its
 * center to rotate the map. When added with osmium_map_add_overlay() (or as
 * <child type="overlay">) the map property is set for you. */
OSMIUM_API GtkWidget *osmium_compass_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_compass_get_map(OsmiumCompass *self);
OSMIUM_API void osmium_compass_set_map(OsmiumCompass *self, OsmiumMap *map);

G_END_DECLS
