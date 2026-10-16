/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

typedef enum {
  OSMIUM_GEOMETRY_POINT,
  OSMIUM_GEOMETRY_LINE,
  OSMIUM_GEOMETRY_POLYGON
} OsmiumGeometryKind;

typedef enum {
  OSMIUM_POINT_CIRCLE,
  OSMIUM_POINT_SQUARE,
  OSMIUM_POINT_DIAMOND,
  OSMIUM_POINT_TRIANGLE,
  OSMIUM_POINT_CROSS
} OsmiumPointShape;

typedef enum {
  OSMIUM_LINE_SOLID,
  OSMIUM_LINE_DASH,
  OSMIUM_LINE_DOT,
  OSMIUM_LINE_DASH_DOT
} OsmiumLineStyle;

typedef struct {
  GdkRGBA stroke;
  GdkRGBA fill;
  double line_width;
  double point_radius;
  OsmiumPointShape shape;
  OsmiumLineStyle line_style;
} OsmiumSymbol;

typedef struct {
  char *value;
  char *label;
  gboolean visible;
  OsmiumSymbol symbol;
} OsmiumStyleCategory;

typedef struct {
  double lower;
  double upper;
  char *label;
  gboolean visible;
  OsmiumSymbol symbol;
} OsmiumStyleClass;

typedef enum {
  OSMIUM_STYLE_SINGLE,
  OSMIUM_STYLE_CATEGORIZED,
  OSMIUM_STYLE_GRADUATED
} OsmiumStyleKind;

typedef struct {
  gboolean enabled;
  char *field;
  char *font_family;
  double size;
  GdkRGBA color;
  GdkRGBA halo_color;
  double halo_width;
} OsmiumStyleLabels;

typedef struct {
  OsmiumStyleKind kind;
  OsmiumSymbol symbol;
  char *field;
  GArray *categories; /* OsmiumStyleCategory; a NULL value is the symbol for all other values */
  GArray *classes;    /* OsmiumStyleClass, ordered by lower bound */
  OsmiumStyleLabels labels;
} OsmiumStyle;

typedef enum {
  OSMIUM_CLASSIFY_EQUAL_INTERVAL,
  OSMIUM_CLASSIFY_QUANTILE,
  OSMIUM_CLASSIFY_NATURAL_BREAKS
} OsmiumClassifyMode;

OSMIUM_API void osmium_symbol_init(OsmiumSymbol *symbol);

OSMIUM_API OsmiumStyle *osmium_style_new(void);
OSMIUM_API OsmiumStyle *osmium_style_copy(const OsmiumStyle *style);
OSMIUM_API void osmium_style_free(OsmiumStyle *style);

OSMIUM_API void osmium_style_set_field(OsmiumStyle *style, const char *field);
OSMIUM_API void osmium_style_set_label_field(OsmiumStyle *style, const char *field);
OSMIUM_API void osmium_style_set_label_font(OsmiumStyle *style, const char *family);
OSMIUM_API void osmium_style_clear_categories(OsmiumStyle *style);
OSMIUM_API void osmium_style_clear_classes(OsmiumStyle *style);
OSMIUM_API void osmium_style_add_category(OsmiumStyle *style, const char *value, const char *label,
                                          const OsmiumSymbol *symbol);
OSMIUM_API void osmium_style_add_class(OsmiumStyle *style, double lower, double upper, const char *label,
                                       const OsmiumSymbol *symbol);

OSMIUM_API const char *const *osmium_color_ramp_names(void);
OSMIUM_API void osmium_color_ramp_sample(const char *ramp, double t, GdkRGBA *color);

/* Fills style->categories with one category per value, colored from the ramp, plus an "other" entry. */
OSMIUM_API void osmium_style_build_categories(OsmiumStyle *style, const char *const *values, guint n_values,
                                              const char *ramp);

/* Class breaks for the values: n_classes + 1 ascending bounds (free with g_free). NULL when there
 * are no values. */
OSMIUM_API double *osmium_classify(const double *values, guint n_values, OsmiumClassifyMode mode, guint n_classes);

/* Fills style->classes from the bounds, colored from the ramp. */
OSMIUM_API void osmium_style_build_classes(OsmiumStyle *style, const double *breaks, guint n_classes,
                                           const char *ramp, gboolean invert_ramp);

/* QGIS (.qml) and OGC SLD documents. Parsing replaces everything in style except what the file lacks. */
/* The geometry kind of the layer selects the symbol flavor (fill, line or marker) written to the file. */
OSMIUM_API char *osmium_style_to_qml_kind(const OsmiumStyle *style, OsmiumGeometryKind kind);
OSMIUM_API char *osmium_style_to_qml(const OsmiumStyle *style);
OSMIUM_API gboolean osmium_style_from_qml(OsmiumStyle *style, const char *xml, GError **error);
OSMIUM_API char *osmium_style_to_sld(const OsmiumStyle *style, const char *layer_name);
OSMIUM_API gboolean osmium_style_from_sld(OsmiumStyle *style, const char *xml, GError **error);

G_END_DECLS
