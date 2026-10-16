/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gtk/gtk.h>
#include "osmium_map.h"

G_BEGIN_DECLS

typedef struct _OsmiumExpression OsmiumExpression;

/* A small QGIS-like expression language over the fields of a table row:
 *   "field" or bare names, 'text', numbers, TRUE/FALSE/NULL, ( ), + - * / % ||, = != <> < <= > >=,
 *   AND OR NOT, LIKE / ILIKE ('%' and '_'), IN (...), IS [NOT] NULL, BETWEEN a AND b and the
 *   functions lower upper trim length abs round coalesce substr. Empty cells are NULL; text that
 *   reads as a number compares as a number. */
OSMIUM_API OsmiumExpression *osmium_expression_parse(const char *text,
                                                     const char *const *field_names,
                                                     guint n_fields,
                                                     GError **error);
OSMIUM_API void osmium_expression_free(OsmiumExpression *expression);

/* TRUE when the expression is true for the row (cells: one string per field). */
OSMIUM_API gboolean osmium_expression_matches(OsmiumExpression *expression, const char *const *cells);

G_END_DECLS
