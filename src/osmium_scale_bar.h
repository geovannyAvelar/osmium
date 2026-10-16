/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>

#include "osmium_geodesy.h"
#include "osmium_map.h"

G_BEGIN_DECLS

#define OSMIUM_TYPE_SCALE_BAR (osmium_scale_bar_get_type())

G_DECLARE_FINAL_TYPE(OsmiumScaleBar, osmium_scale_bar, OSMIUM, SCALE_BAR, GtkWidget)

/* Shows how much ground a stretch of the map covers, measured on the ellipsoid at the bar's own
 * position. When added with osmium_map_add_overlay() (or as <child type="overlay">) the map
 * property is set for you. */
OSMIUM_API GtkWidget *osmium_scale_bar_new(OsmiumMap *map);

OSMIUM_API OsmiumMap *osmium_scale_bar_get_map(OsmiumScaleBar *self);
OSMIUM_API void osmium_scale_bar_set_map(OsmiumScaleBar *self, OsmiumMap *map);

OSMIUM_API OsmiumUnitSystem osmium_scale_bar_get_units(OsmiumScaleBar *self);
OSMIUM_API void osmium_scale_bar_set_units(OsmiumScaleBar *self, OsmiumUnitSystem units);

G_END_DECLS
