/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_style.h"

#include <math.h>
#include <string.h>

void osmium_symbol_init(OsmiumSymbol *symbol) {
  gdk_rgba_parse(&symbol->stroke, "#1c71d8");
  gdk_rgba_parse(&symbol->fill, "rgba(28, 113, 216, 0.35)");
  symbol->line_width = 1.5;
  symbol->point_radius = 4.0;
  symbol->shape = OSMIUM_POINT_CIRCLE;
  symbol->line_style = OSMIUM_LINE_SOLID;
}

static void category_clear(gpointer data) {
  OsmiumStyleCategory *category = data;

  g_free(category->value);
  g_free(category->label);
}

static void class_clear(gpointer data) {
  g_free(((OsmiumStyleClass *)data)->label);
}

static void new_arrays(OsmiumStyle *style) {
  style->categories = g_array_new(FALSE, TRUE, sizeof(OsmiumStyleCategory));
  g_array_set_clear_func(style->categories, category_clear);
  style->classes = g_array_new(FALSE, TRUE, sizeof(OsmiumStyleClass));
  g_array_set_clear_func(style->classes, class_clear);
}

OsmiumStyle *osmium_style_new(void) {
  OsmiumStyle *style = g_new0(OsmiumStyle, 1);

  style->kind = OSMIUM_STYLE_SINGLE;
  osmium_symbol_init(&style->symbol);
  new_arrays(style);

  style->labels.size = 11.0;
  style->labels.halo_width = 1.5;
  gdk_rgba_parse(&style->labels.color, "#1e1e1e");
  gdk_rgba_parse(&style->labels.halo_color, "rgba(255, 255, 255, 0.9)");
  return style;
}

OsmiumStyle *osmium_style_copy(const OsmiumStyle *style) {
  OsmiumStyle *copy = osmium_style_new();

  copy->kind = style->kind;
  copy->symbol = style->symbol;
  copy->field = g_strdup(style->field);

  for (guint i = 0; i < style->categories->len; i++) {
    const OsmiumStyleCategory *c = &g_array_index(style->categories, OsmiumStyleCategory, i);

    osmium_style_add_category(copy, c->value, c->label, &c->symbol);
    g_array_index(copy->categories, OsmiumStyleCategory, i).visible = c->visible;
  }
  for (guint i = 0; i < style->classes->len; i++) {
    const OsmiumStyleClass *c = &g_array_index(style->classes, OsmiumStyleClass, i);

    osmium_style_add_class(copy, c->lower, c->upper, c->label, &c->symbol);
    g_array_index(copy->classes, OsmiumStyleClass, i).visible = c->visible;
  }

  copy->labels = style->labels;
  copy->labels.field = g_strdup(style->labels.field);
  copy->labels.font_family = g_strdup(style->labels.font_family);
  return copy;
}

void osmium_style_free(OsmiumStyle *style) {
  if (style == NULL) {
    return;
  }
  g_free(style->field);
  g_free(style->labels.field);
  g_free(style->labels.font_family);
  g_array_unref(style->categories);
  g_array_unref(style->classes);
  g_free(style);
}

void osmium_style_set_field(OsmiumStyle *style, const char *field) {
  g_free(style->field);
  style->field = g_strdup(field);
}

void osmium_style_set_label_field(OsmiumStyle *style, const char *field) {
  g_free(style->labels.field);
  style->labels.field = g_strdup(field);
}

void osmium_style_set_label_font(OsmiumStyle *style, const char *family) {
  g_free(style->labels.font_family);
  style->labels.font_family = g_strdup(family);
}

void osmium_style_clear_categories(OsmiumStyle *style) {
  g_array_set_size(style->categories, 0);
}

void osmium_style_clear_classes(OsmiumStyle *style) {
  g_array_set_size(style->classes, 0);
}

void osmium_style_add_category(OsmiumStyle *style, const char *value, const char *label, const OsmiumSymbol *symbol) {
  OsmiumStyleCategory category = {
      .value = g_strdup(value),
      .label = g_strdup(label != NULL ? label : value),
      .visible = TRUE,
      .symbol = *symbol,
  };

  g_array_append_val(style->categories, category);
}

void osmium_style_add_class(OsmiumStyle *style, double lower, double upper, const char *label, const OsmiumSymbol *symbol) {
  OsmiumStyleClass cls = {
      .lower = lower,
      .upper = upper,
      .label = g_strdup(label),
      .visible = TRUE,
      .symbol = *symbol,
  };

  g_array_append_val(style->classes, cls);
}

typedef struct {
  const char *name;
  const char *stops[8];
  guint n;
} Ramp;

static const Ramp ramps[] = {
    {"Blues", {"#f7fbff", "#c6dbef", "#6baed6", "#2171b5", "#08306b"}, 5},
    {"Greens", {"#f7fcf5", "#c7e9c0", "#74c476", "#238b45", "#00441b"}, 5},
    {"Reds", {"#fff5f0", "#fcbba1", "#fb6a4a", "#cb181d", "#67000d"}, 5},
    {"Oranges", {"#fff5eb", "#fdd0a2", "#fd8d3c", "#d94801", "#7f2704"}, 5},
    {"Viridis", {"#440154", "#3b528b", "#21918c", "#5ec962", "#fde725"}, 5},
    {"Magma", {"#000004", "#51127c", "#b73779", "#fc8961", "#fcfdbf"}, 5},
    {"Spectral", {"#d53e4f", "#fc8d59", "#fee08b", "#e6f598", "#99d594", "#3288bd"}, 6},
    {"RdYlGn", {"#d73027", "#fc8d59", "#ffffbf", "#91cf60", "#1a9850"}, 5},
    {"Qualitative", {"#1c71d8", "#e66100", "#26a269", "#c64600", "#9141ac", "#f5c211", "#a51d2d", "#5e5c64"}, 8},
};

const char *const *osmium_color_ramp_names(void) {
  static const char *names[G_N_ELEMENTS(ramps) + 1];

  if (names[0] == NULL) {
    for (guint i = 0; i < G_N_ELEMENTS(ramps); i++) {
      names[i] = ramps[i].name;
    }
  }
  return names;
}

static const Ramp *find_ramp(const char *name) {
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS(ramps); i++) {
    if (g_strcmp0(ramps[i].name, name) == 0) {
      return &ramps[i];
    }
  }
  return &ramps[0];
}

void osmium_color_ramp_sample(const char *ramp, double t, GdkRGBA *color) {
  const Ramp *r = find_ramp(ramp);
  const double position = CLAMP(t, 0.0, 1.0) * (r->n - 1);
  const guint low = MIN((guint)floor(position), r->n - 1);
  const guint high = MIN(low + 1, r->n - 1);
  const double f = position - low;
  GdkRGBA a, b;

  gdk_rgba_parse(&a, r->stops[low]);
  gdk_rgba_parse(&b, r->stops[high]);
  color->red = a.red + (b.red - a.red) * f;
  color->green = a.green + (b.green - a.green) * f;
  color->blue = a.blue + (b.blue - a.blue) * f;
  color->alpha = 1.0;
}

static void symbol_from_color(OsmiumSymbol *symbol, const OsmiumSymbol *base, const GdkRGBA *color) {
  *symbol = *base;
  symbol->fill = *color;
  symbol->fill.alpha = base->fill.alpha > 0.0 ? MAX(base->fill.alpha, 0.6) : 0.6;
  symbol->stroke.red = color->red * 0.6;
  symbol->stroke.green = color->green * 0.6;
  symbol->stroke.blue = color->blue * 0.6;
  symbol->stroke.alpha = 1.0;
}

void osmium_style_build_categories(OsmiumStyle *style, const char *const *values, guint n_values, const char *ramp) {
  const Ramp *r = find_ramp(ramp);
  const gboolean qualitative = g_strcmp0(r->name, "Qualitative") == 0;

  osmium_style_clear_categories(style);

  for (guint i = 0; i < n_values; i++) {
    GdkRGBA color;
    OsmiumSymbol symbol;

    if (qualitative) {
      gdk_rgba_parse(&color, r->stops[i % r->n]);
    } else {
      osmium_color_ramp_sample(ramp, n_values > 1 ? (double)i / (n_values - 1) : 0.5, &color);
    }
    symbol_from_color(&symbol, &style->symbol, &color);
    osmium_style_add_category(style, values[i], values[i][0] != '\0' ? values[i] : "(empty)", &symbol);
  }

  {
    OsmiumSymbol other = style->symbol;

    gdk_rgba_parse(&other.fill, "rgba(150, 150, 150, 0.5)");
    gdk_rgba_parse(&other.stroke, "#606060");
    osmium_style_add_category(style, NULL, "all other values", &other);
  }
}

static int compare_double(gconstpointer a, gconstpointer b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;

  return (x > y) - (x < y);
}

static double *jenks_breaks(const double *sorted, guint n, guint classes) {
  const guint cap = 4000;
  guint m = n;
  g_autofree double *sample = NULL;

  if (n > cap) {
    sample = g_new(double, cap);
    for (guint i = 0; i < cap; i++) {
      sample[i] = sorted[(gsize)i * (n - 1) / (cap - 1)];
    }
    sorted = sample;
    m = cap;
  }

  g_autofree double *lower = g_new0(double, (gsize)(m + 1) * (classes + 1));
  g_autofree double *variance = g_new0(double, (gsize)(m + 1) * (classes + 1));

#define LOWER(i, j) lower[(gsize)(i) * (classes + 1) + (j)]
#define VARIANCE(i, j) variance[(gsize)(i) * (classes + 1) + (j)]

  for (guint j = 1; j <= classes; j++) {
    LOWER(1, j) = 1;
    VARIANCE(1, j) = 0;
    for (guint i = 2; i <= m; i++) {
      VARIANCE(i, j) = G_MAXDOUBLE;
    }
  }

  for (guint l = 2; l <= m; l++) {
    double sum = 0, sum_squares = 0, w = 0;

    for (guint k = 1; k <= l; k++) {
      const guint index = l - k + 1;
      const double value = sorted[index - 1];

      w++;
      sum += value;
      sum_squares += value * value;
      const double v = sum_squares - (sum * sum) / w;
      const guint previous = index - 1;

      if (previous != 0) {
        for (guint j = 2; j <= classes; j++) {
          if (VARIANCE(l, j) >= v + VARIANCE(previous, j - 1)) {
            LOWER(l, j) = index;
            VARIANCE(l, j) = v + VARIANCE(previous, j - 1);
          }
        }
      }
    }
    LOWER(l, 1) = 1;
    VARIANCE(l, 1) = sum_squares - (sum * sum) / w;
  }

  double *breaks = g_new0(double, classes + 1);
  guint k = m;

  breaks[classes] = sorted[m - 1];
  breaks[0] = sorted[0];
  for (guint j = classes; j >= 2; j--) {
    const guint id = (guint)LOWER(k, j) - 2;

    breaks[j - 1] = sorted[id];
    k = (guint)LOWER(k, j) - 1;
  }

#undef LOWER
#undef VARIANCE
  return breaks;
}

double *osmium_classify(const double *values, guint n_values, OsmiumClassifyMode mode, guint n_classes) {
  if (n_values == 0 || n_classes == 0) {
    return NULL;
  }

  g_autofree double *sorted = g_memdup2(values, sizeof(double) * n_values);

  qsort(sorted, n_values, sizeof(double), (int (*)(const void *, const void *))compare_double);

  const double min = sorted[0];
  const double max = sorted[n_values - 1];
  double *breaks;

  if (min == max) {
    breaks = g_new(double, n_classes + 1);
    for (guint i = 0; i <= n_classes; i++) {
      breaks[i] = min;
    }
    return breaks;
  }

  if (mode == OSMIUM_CLASSIFY_NATURAL_BREAKS && n_values > n_classes) {
    return jenks_breaks(sorted, n_values, n_classes);
  }

  breaks = g_new(double, n_classes + 1);
  for (guint i = 0; i <= n_classes; i++) {
    if (mode == OSMIUM_CLASSIFY_QUANTILE) {
      const double position = (double)i / n_classes * (n_values - 1);
      const guint low = (guint)floor(position);
      const guint high = MIN(low + 1, n_values - 1);

      breaks[i] = sorted[low] + (sorted[high] - sorted[low]) * (position - low);
    } else {
      breaks[i] = min + (max - min) * i / n_classes;
    }
  }
  breaks[0] = min;
  breaks[n_classes] = max;
  return breaks;
}

void osmium_style_build_classes(OsmiumStyle *style, const double *breaks, guint n_classes, const char *ramp,
                                gboolean invert_ramp) {
  osmium_style_clear_classes(style);

  for (guint i = 0; i < n_classes; i++) {
    GdkRGBA color;
    OsmiumSymbol symbol;
    double t = n_classes > 1 ? (double)i / (n_classes - 1) : 0.5;
    g_autofree char *label = g_strdup_printf("%.6g – %.6g", breaks[i], breaks[i + 1]);

    osmium_color_ramp_sample(ramp, invert_ramp ? 1.0 - t : t, &color);
    symbol_from_color(&symbol, &style->symbol, &color);
    osmium_style_add_class(style, breaks[i], breaks[i + 1], label, &symbol);
  }
}
