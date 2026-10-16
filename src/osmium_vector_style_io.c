/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_style.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlwriter.h>
#include <math.h>
#include <string.h>

#define PX_PER_MM (96.0 / 25.4)
#define PX_PER_POINT (96.0 / 72.0)

/* --- shared helpers -------------------------------------------------------------------------- */

static const xmlChar *X(const char *text) {
  return (const xmlChar *)text;
}

static const char *S(const xmlChar *text) {
  return (const char *)text;
}

static char *format_double(double value) {
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd(buffer, sizeof(buffer), "%.6g", value);
  return g_strdup(buffer);
}

static void write_attr_double(xmlTextWriterPtr w, const char *name, double value) {
  g_autofree char *text = format_double(value);

  xmlTextWriterWriteAttribute(w, X(name), X(text));
}

static char *rgba_to_qgis(const GdkRGBA *c) {
  return g_strdup_printf("%d,%d,%d,%d", (int)lround(c->red * 255), (int)lround(c->green * 255),
                         (int)lround(c->blue * 255), (int)lround(c->alpha * 255));
}

static gboolean rgba_from_qgis(const char *text, GdkRGBA *c) {
  int r, g, b, a = 255;

  if (text == NULL) {
    return FALSE;
  }
  if (sscanf(text, "%d,%d,%d,%d", &r, &g, &b, &a) < 3) {
    return FALSE;
  }
  c->red = CLAMP(r, 0, 255) / 255.0;
  c->green = CLAMP(g, 0, 255) / 255.0;
  c->blue = CLAMP(b, 0, 255) / 255.0;
  c->alpha = CLAMP(a, 0, 255) / 255.0;
  return TRUE;
}

static char *rgb_to_hex(const GdkRGBA *c) {
  return g_strdup_printf("#%02x%02x%02x", (int)lround(c->red * 255), (int)lround(c->green * 255),
                         (int)lround(c->blue * 255));
}

static gboolean rgba_from_hex(const char *text, double opacity, GdkRGBA *c) {
  if (text == NULL || !gdk_rgba_parse(c, text)) {
    return FALSE;
  }
  c->alpha = opacity;
  return TRUE;
}

static double parse_double(const char *text, double fallback) {
  char *end = NULL;
  double value;

  if (text == NULL || text[0] == '\0') {
    return fallback;
  }
  value = g_ascii_strtod(text, &end);
  return end != text ? value : fallback;
}

static xmlNodePtr first_child(xmlNodePtr node, const char *name) {
  for (xmlNodePtr n = node != NULL ? node->children : NULL; n != NULL; n = n->next) {
    if (n->type == XML_ELEMENT_NODE && g_strcmp0(S(n->name), name) == 0) {
      return n;
    }
  }
  return NULL;
}

static xmlNodePtr find_descendant(xmlNodePtr node, const char *name) {
  for (xmlNodePtr n = node != NULL ? node->children : NULL; n != NULL; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      continue;
    }
    if (g_strcmp0(S(n->name), name) == 0) {
      return n;
    }
    xmlNodePtr deeper = find_descendant(n, name);

    if (deeper != NULL) {
      return deeper;
    }
  }
  return NULL;
}

static char *attr(xmlNodePtr node, const char *name) {
  xmlChar *value = node != NULL ? xmlGetProp(node, X(name)) : NULL;
  char *result = value != NULL ? g_strdup(S(value)) : NULL;

  xmlFree(value);
  return result;
}

static char *node_text(xmlNodePtr node) {
  xmlChar *content = node != NULL ? xmlNodeGetContent(node) : NULL;
  char *result = content != NULL ? g_strstrip(g_strdup(S(content))) : NULL;

  xmlFree(content);
  return result;
}

static const char *shape_names[] = {"circle", "square", "diamond", "triangle", "cross"};
static const char *line_style_names[] = {"solid", "dash", "dot", "dash dot"};

static OsmiumPointShape shape_from_name(const char *name) {
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS(shape_names); i++) {
    if (g_ascii_strcasecmp(name, shape_names[i]) == 0) {
      return (OsmiumPointShape)i;
    }
  }
  if (name != NULL && (g_ascii_strcasecmp(name, "cross2") == 0 || g_ascii_strcasecmp(name, "x") == 0 ||
                       g_ascii_strcasecmp(name, "star") == 0)) {
    return OSMIUM_POINT_CROSS;
  }
  return OSMIUM_POINT_CIRCLE;
}

static OsmiumLineStyle line_style_from_name(const char *name) {
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS(line_style_names); i++) {
    if (g_ascii_strcasecmp(name, line_style_names[i]) == 0) {
      return (OsmiumLineStyle)i;
    }
  }
  return OSMIUM_LINE_SOLID;
}

/* --- QML export ------------------------------------------------------------------------------ */

static void qml_option(xmlTextWriterPtr w, const char *name, const char *value) {
  xmlTextWriterStartElement(w, X("Option"));
  xmlTextWriterWriteAttribute(w, X("name"), X(name));
  xmlTextWriterWriteAttribute(w, X("value"), X(value));
  xmlTextWriterWriteAttribute(w, X("type"), X("QString"));
  xmlTextWriterEndElement(w);
}

static void qml_option_double(xmlTextWriterPtr w, const char *name, double value) {
  g_autofree char *text = format_double(value);

  qml_option(w, name, text);
}

static void qml_symbol(xmlTextWriterPtr w, const char *name, const OsmiumSymbol *s, OsmiumGeometryKind kind) {
  const char *type = kind == OSMIUM_GEOMETRY_POINT ? "marker" : (kind == OSMIUM_GEOMETRY_LINE ? "line" : "fill");
  g_autofree char *fill = rgba_to_qgis(&s->fill);
  g_autofree char *stroke = rgba_to_qgis(&s->stroke);

  xmlTextWriterStartElement(w, X("symbol"));
  xmlTextWriterWriteAttribute(w, X("type"), X(type));
  xmlTextWriterWriteAttribute(w, X("name"), X(name));
  xmlTextWriterWriteAttribute(w, X("alpha"), X("1"));
  xmlTextWriterWriteAttribute(w, X("clip_to_extent"), X("1"));
  xmlTextWriterWriteAttribute(w, X("force_rhr"), X("0"));

  xmlTextWriterStartElement(w, X("layer"));
  xmlTextWriterWriteAttribute(w, X("class"),
                              X(kind == OSMIUM_GEOMETRY_POINT ? "SimpleMarker"
                                                              : (kind == OSMIUM_GEOMETRY_LINE ? "SimpleLine" : "SimpleFill")));
  xmlTextWriterWriteAttribute(w, X("locked"), X("0"));
  xmlTextWriterWriteAttribute(w, X("enabled"), X("1"));
  xmlTextWriterWriteAttribute(w, X("pass"), X("0"));

  xmlTextWriterStartElement(w, X("Option"));
  xmlTextWriterWriteAttribute(w, X("type"), X("Map"));

  if (kind == OSMIUM_GEOMETRY_LINE) {
    qml_option(w, "line_color", stroke);
    qml_option_double(w, "line_width", s->line_width / PX_PER_MM);
    qml_option(w, "line_width_unit", "MM");
    qml_option(w, "line_style", line_style_names[s->line_style]);
    qml_option(w, "capstyle", "round");
    qml_option(w, "joinstyle", "round");
  } else if (kind == OSMIUM_GEOMETRY_POINT) {
    qml_option(w, "color", fill);
    qml_option(w, "outline_color", stroke);
    qml_option_double(w, "outline_width", s->line_width / PX_PER_MM);
    qml_option(w, "outline_width_unit", "MM");
    qml_option(w, "outline_style", "solid");
    qml_option(w, "name", shape_names[s->shape]);
    qml_option_double(w, "size", s->point_radius * 2.0 / PX_PER_MM);
    qml_option(w, "size_unit", "MM");
  } else {
    qml_option(w, "color", fill);
    qml_option(w, "outline_color", stroke);
    qml_option_double(w, "outline_width", s->line_width / PX_PER_MM);
    qml_option(w, "outline_width_unit", "MM");
    qml_option(w, "outline_style", line_style_names[s->line_style]);
    qml_option(w, "style", "solid");
  }

  xmlTextWriterEndElement(w); /* Option Map */
  xmlTextWriterEndElement(w); /* layer */
  xmlTextWriterEndElement(w); /* symbol */
}

/* A style does not know its geometry type, so the file carries all three symbol flavors and the
 * rules point at the one matching the layer. */
static void qml_symbols(xmlTextWriterPtr w, const OsmiumStyle *style, OsmiumGeometryKind kind) {
  xmlTextWriterStartElement(w, X("symbols"));

  if (style->kind == OSMIUM_STYLE_SINGLE) {
    qml_symbol(w, "0", &style->symbol, kind);
  } else if (style->kind == OSMIUM_STYLE_CATEGORIZED) {
    for (guint i = 0; i < style->categories->len; i++) {
      g_autofree char *name = g_strdup_printf("%u", i);

      qml_symbol(w, name, &g_array_index(style->categories, OsmiumStyleCategory, i).symbol, kind);
    }
  } else {
    for (guint i = 0; i < style->classes->len; i++) {
      g_autofree char *name = g_strdup_printf("%u", i);

      qml_symbol(w, name, &g_array_index(style->classes, OsmiumStyleClass, i).symbol, kind);
    }
  }

  xmlTextWriterEndElement(w);
}

char *osmium_style_to_qml_kind(const OsmiumStyle *style, OsmiumGeometryKind kind) {
  xmlBufferPtr buffer = xmlBufferCreate();
  xmlTextWriterPtr w = xmlNewTextWriterMemory(buffer, 0);
  char *result;

  xmlTextWriterSetIndent(w, 1);
  xmlTextWriterStartDocument(w, NULL, "UTF-8", NULL);
  xmlTextWriterWriteDTD(w, X("qgis"), X("http://mrcc.com/qgis.dtd"), X("SYSTEM"), NULL);
  xmlTextWriterStartElement(w, X("qgis"));
  xmlTextWriterWriteAttribute(w, X("version"), X("3.28.0-Firenze"));
  xmlTextWriterWriteAttribute(w, X("styleCategories"), X("Symbology|Labeling"));

  xmlTextWriterStartElement(w, X("renderer-v2"));
  xmlTextWriterWriteAttribute(w, X("forceraster"), X("0"));
  xmlTextWriterWriteAttribute(w, X("symbollevels"), X("0"));
  xmlTextWriterWriteAttribute(w, X("enableorderby"), X("0"));

  if (style->kind == OSMIUM_STYLE_CATEGORIZED && style->field != NULL) {
    xmlTextWriterWriteAttribute(w, X("type"), X("categorizedSymbol"));
    xmlTextWriterWriteAttribute(w, X("attr"), X(style->field));
    xmlTextWriterStartElement(w, X("categories"));
    for (guint i = 0; i < style->categories->len; i++) {
      const OsmiumStyleCategory *c = &g_array_index(style->categories, OsmiumStyleCategory, i);
      g_autofree char *index = g_strdup_printf("%u", i);

      xmlTextWriterStartElement(w, X("category"));
      xmlTextWriterWriteAttribute(w, X("render"), X(c->visible ? "true" : "false"));
      xmlTextWriterWriteAttribute(w, X("value"), X(c->value != NULL ? c->value : ""));
      xmlTextWriterWriteAttribute(w, X("label"), X(c->label != NULL ? c->label : ""));
      xmlTextWriterWriteAttribute(w, X("symbol"), X(index));
      xmlTextWriterEndElement(w);
    }
    xmlTextWriterEndElement(w);
    qml_symbols(w, style, kind);
  } else if (style->kind == OSMIUM_STYLE_GRADUATED && style->field != NULL) {
    xmlTextWriterWriteAttribute(w, X("type"), X("graduatedSymbol"));
    xmlTextWriterWriteAttribute(w, X("attr"), X(style->field));
    xmlTextWriterWriteAttribute(w, X("graduatedMethod"), X("GraduatedColor"));
    xmlTextWriterStartElement(w, X("ranges"));
    for (guint i = 0; i < style->classes->len; i++) {
      const OsmiumStyleClass *c = &g_array_index(style->classes, OsmiumStyleClass, i);
      g_autofree char *index = g_strdup_printf("%u", i);

      xmlTextWriterStartElement(w, X("range"));
      xmlTextWriterWriteAttribute(w, X("render"), X(c->visible ? "true" : "false"));
      write_attr_double(w, "lower", c->lower);
      write_attr_double(w, "upper", c->upper);
      xmlTextWriterWriteAttribute(w, X("label"), X(c->label != NULL ? c->label : ""));
      xmlTextWriterWriteAttribute(w, X("symbol"), X(index));
      xmlTextWriterEndElement(w);
    }
    xmlTextWriterEndElement(w);
    qml_symbols(w, style, kind);
  } else {
    xmlTextWriterWriteAttribute(w, X("type"), X("singleSymbol"));
    qml_symbols(w, style, kind);
  }
  xmlTextWriterEndElement(w); /* renderer-v2 */

  if (style->labels.enabled && style->labels.field != NULL) {
    const OsmiumStyleLabels *l = &style->labels;
    g_autofree char *color = rgba_to_qgis(&l->color);
    g_autofree char *halo = rgba_to_qgis(&l->halo_color);
    g_autofree char *size = format_double(l->size / PX_PER_POINT);
    g_autofree char *halo_size = format_double(l->halo_width / PX_PER_MM);

    xmlTextWriterStartElement(w, X("labeling"));
    xmlTextWriterWriteAttribute(w, X("type"), X("simple"));
    xmlTextWriterStartElement(w, X("settings"));
    xmlTextWriterWriteAttribute(w, X("calloutType"), X("simple"));
    xmlTextWriterStartElement(w, X("text-style"));
    xmlTextWriterWriteAttribute(w, X("fieldName"), X(l->field));
    xmlTextWriterWriteAttribute(w, X("isExpression"), X("0"));
    xmlTextWriterWriteAttribute(w, X("fontFamily"), X(l->font_family != NULL ? l->font_family : "Sans"));
    xmlTextWriterWriteAttribute(w, X("fontSize"), X(size));
    xmlTextWriterWriteAttribute(w, X("fontSizeUnit"), X("Point"));
    xmlTextWriterWriteAttribute(w, X("textColor"), X(color));
    xmlTextWriterWriteAttribute(w, X("textOpacity"), X("1"));
    xmlTextWriterStartElement(w, X("text-buffer"));
    xmlTextWriterWriteAttribute(w, X("bufferDraw"), X(l->halo_width > 0 ? "1" : "0"));
    xmlTextWriterWriteAttribute(w, X("bufferSize"), X(halo_size));
    xmlTextWriterWriteAttribute(w, X("bufferSizeUnits"), X("MM"));
    xmlTextWriterWriteAttribute(w, X("bufferColor"), X(halo));
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w); /* text-style */
    xmlTextWriterEndElement(w); /* settings */
    xmlTextWriterEndElement(w); /* labeling */
  }

  xmlTextWriterEndElement(w); /* qgis */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  result = g_strdup(S(xmlBufferContent(buffer)));
  xmlBufferFree(buffer);
  return result;
}

char *osmium_style_to_qml(const OsmiumStyle *style) {
  return osmium_style_to_qml_kind(style, OSMIUM_GEOMETRY_POLYGON);
}

/* --- QML import ------------------------------------------------------------------------------ */

static GHashTable *read_properties(xmlNodePtr layer) {
  GHashTable *props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  for (xmlNodePtr n = layer->children; n != NULL; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      continue;
    }
    if (g_strcmp0(S(n->name), "prop") == 0) {
      char *k = attr(n, "k");
      char *v = attr(n, "v");

      if (k != NULL) {
        g_hash_table_replace(props, k, v != NULL ? v : g_strdup(""));
      } else {
        g_free(v);
      }
    } else if (g_strcmp0(S(n->name), "Option") == 0) {
      for (xmlNodePtr o = n->children; o != NULL; o = o->next) {
        char *name = o->type == XML_ELEMENT_NODE ? attr(o, "name") : NULL;
        char *value = name != NULL ? attr(o, "value") : NULL;

        if (name != NULL) {
          g_hash_table_replace(props, name, value != NULL ? value : g_strdup(""));
        }
      }
    }
  }
  return props;
}

static double mm_value(GHashTable *props, const char *name, const char *unit_name, double fallback_px) {
  const char *text = g_hash_table_lookup(props, name);
  const char *unit = g_hash_table_lookup(props, unit_name);
  const double value = parse_double(text, -1.0);

  if (value < 0.0) {
    return fallback_px;
  }
  if (g_strcmp0(unit, "Pixel") == 0 || g_strcmp0(unit, "RenderMetersInMapUnits") == 0) {
    return value;
  }
  if (g_strcmp0(unit, "Point") == 0) {
    return value * PX_PER_POINT;
  }
  return value * PX_PER_MM;
}

static gboolean qml_read_symbol(xmlNodePtr symbol, OsmiumSymbol *out) {
  xmlNodePtr layer = first_child(symbol, "layer");

  if (layer == NULL) {
    return FALSE;
  }

  g_autofree char *klass = attr(layer, "class");
  g_autoptr(GHashTable) props = read_properties(layer);
  const char *type = NULL;
  gboolean ok = TRUE;
  g_autofree char *symbol_type = attr(symbol, "type");

  type = symbol_type;
  osmium_symbol_init(out);

  if (g_strcmp0(klass, "SimpleFill") == 0 || g_strcmp0(type, "fill") == 0) {
    rgba_from_qgis(g_hash_table_lookup(props, "color"), &out->fill);
    rgba_from_qgis(g_hash_table_lookup(props, "outline_color"), &out->stroke);
    if (g_strcmp0(g_hash_table_lookup(props, "style"), "no") == 0) {
      out->fill.alpha = 0.0;
    }
    if (g_strcmp0(g_hash_table_lookup(props, "outline_style"), "no") == 0) {
      out->line_width = 0.0;
    } else {
      out->line_width = mm_value(props, "outline_width", "outline_width_unit", 1.5);
    }
    out->line_style = line_style_from_name(g_hash_table_lookup(props, "outline_style"));
  } else if (g_strcmp0(klass, "SimpleLine") == 0 || g_strcmp0(type, "line") == 0) {
    rgba_from_qgis(g_hash_table_lookup(props, "line_color"), &out->stroke);
    out->line_width = mm_value(props, "line_width", "line_width_unit", 1.5);
    out->line_style = line_style_from_name(g_hash_table_lookup(props, "line_style"));
  } else if (g_strcmp0(klass, "SimpleMarker") == 0 || g_strcmp0(type, "marker") == 0) {
    rgba_from_qgis(g_hash_table_lookup(props, "color"), &out->fill);
    rgba_from_qgis(g_hash_table_lookup(props, "outline_color"), &out->stroke);
    out->line_width = mm_value(props, "outline_width", "outline_width_unit", 1.0);
    out->point_radius = MAX(mm_value(props, "size", "size_unit", 8.0) / 2.0, 1.0);
    out->shape = shape_from_name(g_hash_table_lookup(props, "name"));
  } else {
    ok = FALSE;
  }
  return ok;
}

static gboolean parse_bool(const char *text, gboolean fallback) {
  if (text == NULL) {
    return fallback;
  }
  return g_ascii_strcasecmp(text, "false") != 0 && g_strcmp0(text, "0") != 0;
}

static gboolean read_qml_labels(OsmiumStyle *style, xmlNodePtr root) {
  xmlNodePtr labeling = first_child(root, "labeling");
  xmlNodePtr text = find_descendant(labeling, "text-style");
  g_autofree char *field = attr(text, "fieldName");

  if (field == NULL || field[0] == '\0') {
    return FALSE;
  }

  g_autofree char *family = attr(text, "fontFamily");
  g_autofree char *size = attr(text, "fontSize");
  g_autofree char *unit = attr(text, "fontSizeUnit");
  g_autofree char *color = attr(text, "textColor");
  xmlNodePtr buffer = first_child(text, "text-buffer");
  g_autofree char *draw = attr(buffer, "bufferDraw");
  g_autofree char *buffer_size = attr(buffer, "bufferSize");
  g_autofree char *buffer_color = attr(buffer, "bufferColor");

  style->labels.enabled = TRUE;
  osmium_style_set_label_field(style, field);
  osmium_style_set_label_font(style, family);
  if (size != NULL) {
    const double value = parse_double(size, 8.0);

    style->labels.size = g_strcmp0(unit, "Pixel") == 0 ? value : value * PX_PER_POINT;
  }
  rgba_from_qgis(color, &style->labels.color);
  if (buffer != NULL && parse_bool(draw, FALSE)) {
    style->labels.halo_width = parse_double(buffer_size, 1.0) * PX_PER_MM;
    rgba_from_qgis(buffer_color, &style->labels.halo_color);
  } else {
    style->labels.halo_width = 0.0;
  }
  return TRUE;
}

gboolean osmium_style_from_qml(OsmiumStyle *style, const char *xml, GError **error) {
  xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  xmlNodePtr root;
  xmlNodePtr renderer;
  g_autofree char *type = NULL;
  OsmiumStyle *parsed;
  GHashTable *symbols;

  if (doc == NULL || (root = xmlDocGetRootElement(doc)) == NULL || g_strcmp0(S(root->name), "qgis") != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This is not a QGIS style file");
    xmlFreeDoc(doc);
    return FALSE;
  }

  parsed = osmium_style_new();
  parsed->labels = style->labels;
  parsed->labels.field = g_strdup(style->labels.field);
  parsed->labels.font_family = g_strdup(style->labels.font_family);
  parsed->symbol = style->symbol;

  renderer = first_child(root, "renderer-v2");
  type = attr(renderer, "type");
  symbols = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  if (renderer != NULL) {
    xmlNodePtr list = first_child(renderer, "symbols");

    for (xmlNodePtr n = list != NULL ? list->children : NULL; n != NULL; n = n->next) {
      OsmiumSymbol symbol;
      char *name = n->type == XML_ELEMENT_NODE && g_strcmp0(S(n->name), "symbol") == 0 ? attr(n, "name") : NULL;

      if (name != NULL && qml_read_symbol(n, &symbol)) {
        g_hash_table_insert(symbols, name, g_memdup2(&symbol, sizeof(symbol)));
      } else {
        g_free(name);
      }
    }
  }

  if (renderer == NULL || g_hash_table_size(symbols) == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The file has no supported symbols");
    g_hash_table_destroy(symbols);
    osmium_style_free(parsed);
    xmlFreeDoc(doc);
    return FALSE;
  }

  if (g_strcmp0(type, "categorizedSymbol") == 0) {
    g_autofree char *field = attr(renderer, "attr");
    xmlNodePtr list = first_child(renderer, "categories");

    parsed->kind = OSMIUM_STYLE_CATEGORIZED;
    osmium_style_set_field(parsed, field);
    for (xmlNodePtr n = list != NULL ? list->children : NULL; n != NULL; n = n->next) {
      g_autofree char *value = n->type == XML_ELEMENT_NODE ? attr(n, "value") : NULL;
      g_autofree char *label = n->type == XML_ELEMENT_NODE ? attr(n, "label") : NULL;
      g_autofree char *render = n->type == XML_ELEMENT_NODE ? attr(n, "render") : NULL;
      g_autofree char *symbol_name = n->type == XML_ELEMENT_NODE ? attr(n, "symbol") : NULL;
      const OsmiumSymbol *symbol = symbol_name != NULL ? g_hash_table_lookup(symbols, symbol_name) : NULL;

      if (symbol == NULL) {
        continue;
      }
      osmium_style_add_category(parsed, value != NULL && value[0] != '\0' ? value : NULL, label, symbol);
      g_array_index(parsed->categories, OsmiumStyleCategory, parsed->categories->len - 1).visible = parse_bool(render, TRUE);
    }
  } else if (g_strcmp0(type, "graduatedSymbol") == 0) {
    g_autofree char *field = attr(renderer, "attr");
    xmlNodePtr list = first_child(renderer, "ranges");

    parsed->kind = OSMIUM_STYLE_GRADUATED;
    osmium_style_set_field(parsed, field);
    for (xmlNodePtr n = list != NULL ? list->children : NULL; n != NULL; n = n->next) {
      g_autofree char *lower = n->type == XML_ELEMENT_NODE ? attr(n, "lower") : NULL;
      g_autofree char *upper = n->type == XML_ELEMENT_NODE ? attr(n, "upper") : NULL;
      g_autofree char *label = n->type == XML_ELEMENT_NODE ? attr(n, "label") : NULL;
      g_autofree char *render = n->type == XML_ELEMENT_NODE ? attr(n, "render") : NULL;
      g_autofree char *symbol_name = n->type == XML_ELEMENT_NODE ? attr(n, "symbol") : NULL;
      const OsmiumSymbol *symbol = symbol_name != NULL ? g_hash_table_lookup(symbols, symbol_name) : NULL;

      if (symbol == NULL) {
        continue;
      }
      osmium_style_add_class(parsed, parse_double(lower, 0.0), parse_double(upper, 0.0), label, symbol);
      g_array_index(parsed->classes, OsmiumStyleClass, parsed->classes->len - 1).visible = parse_bool(render, TRUE);
    }
  } else {
    GHashTableIter iter;
    gpointer value;

    parsed->kind = OSMIUM_STYLE_SINGLE;
    g_hash_table_iter_init(&iter, symbols);
    if (g_hash_table_iter_next(&iter, NULL, &value)) {
      parsed->symbol = *(OsmiumSymbol *)value;
    }
    {
      xmlNodePtr list = first_child(renderer, "symbols");
      xmlNodePtr first = first_child(list, "symbol");
      g_autofree char *name = attr(first, "name");
      const OsmiumSymbol *symbol = name != NULL ? g_hash_table_lookup(symbols, name) : NULL;

      if (symbol != NULL) {
        parsed->symbol = *symbol;
      }
    }
  }

  if (parsed->kind == OSMIUM_STYLE_CATEGORIZED && parsed->categories->len == 0) {
    parsed->kind = OSMIUM_STYLE_SINGLE;
  }
  if (parsed->kind == OSMIUM_STYLE_GRADUATED && parsed->classes->len == 0) {
    parsed->kind = OSMIUM_STYLE_SINGLE;
  }

  if (!read_qml_labels(parsed, root)) {
    parsed->labels.enabled = FALSE;
  }

  {
    OsmiumStyle swap = *style;

    *style = *parsed;
    *parsed = swap;
  }

  g_hash_table_destroy(symbols);
  osmium_style_free(parsed);
  xmlFreeDoc(doc);
  return TRUE;
}

/* --- SLD export ------------------------------------------------------------------------------ */

#define NS_SLD "http://www.opengis.net/sld"
#define NS_OGC "http://www.opengis.net/ogc"

static void sld_css(xmlTextWriterPtr w, const char *name, const char *value) {
  xmlTextWriterStartElement(w, X("CssParameter"));
  xmlTextWriterWriteAttribute(w, X("name"), X(name));
  xmlTextWriterWriteString(w, X(value));
  xmlTextWriterEndElement(w);
}

static void sld_css_double(xmlTextWriterPtr w, const char *name, double value) {
  g_autofree char *text = format_double(value);

  sld_css(w, name, text);
}

static void sld_fill(xmlTextWriterPtr w, const GdkRGBA *color) {
  g_autofree char *hex = rgb_to_hex(color);

  xmlTextWriterStartElement(w, X("Fill"));
  sld_css(w, "fill", hex);
  sld_css_double(w, "fill-opacity", color->alpha);
  xmlTextWriterEndElement(w);
}

static void sld_stroke(xmlTextWriterPtr w, const OsmiumSymbol *s) {
  g_autofree char *hex = rgb_to_hex(&s->stroke);
  const double u = MAX(s->line_width, 1.0);

  xmlTextWriterStartElement(w, X("Stroke"));
  sld_css(w, "stroke", hex);
  sld_css_double(w, "stroke-opacity", s->stroke.alpha);
  sld_css_double(w, "stroke-width", s->line_width);
  sld_css(w, "stroke-linejoin", "round");
  switch (s->line_style) {
  case OSMIUM_LINE_DASH: {
    g_autofree char *text = g_strdup_printf("%g %g", 5 * u, 3 * u);

    sld_css(w, "stroke-dasharray", text);
    break;
  }
  case OSMIUM_LINE_DOT: {
    g_autofree char *text = g_strdup_printf("%g %g", u, 2 * u);

    sld_css(w, "stroke-dasharray", text);
    break;
  }
  case OSMIUM_LINE_DASH_DOT: {
    g_autofree char *text = g_strdup_printf("%g %g %g %g", 5 * u, 2 * u, u, 2 * u);

    sld_css(w, "stroke-dasharray", text);
    break;
  }
  case OSMIUM_LINE_SOLID:
  default:
    break;
  }
  xmlTextWriterEndElement(w);
}

static void sld_symbolizers(xmlTextWriterPtr w, const OsmiumSymbol *s) {
  xmlTextWriterStartElement(w, X("PolygonSymbolizer"));
  sld_fill(w, &s->fill);
  sld_stroke(w, s);
  xmlTextWriterEndElement(w);

  xmlTextWriterStartElement(w, X("LineSymbolizer"));
  sld_stroke(w, s);
  xmlTextWriterEndElement(w);

  xmlTextWriterStartElement(w, X("PointSymbolizer"));
  xmlTextWriterStartElement(w, X("Graphic"));
  xmlTextWriterStartElement(w, X("Mark"));
  xmlTextWriterWriteElement(w, X("WellKnownName"), X(shape_names[s->shape] != NULL ? (s->shape == OSMIUM_POINT_DIAMOND ? "square" : (s->shape == OSMIUM_POINT_CROSS ? "cross" : shape_names[s->shape])) : "circle"));
  sld_fill(w, &s->fill);
  sld_stroke(w, s);
  xmlTextWriterEndElement(w);
  {
    g_autofree char *size = format_double(s->point_radius * 2.0);

    xmlTextWriterWriteElement(w, X("Size"), X(size));
  }
  xmlTextWriterEndElement(w);
  xmlTextWriterEndElement(w);
}

static void sld_property(xmlTextWriterPtr w, const char *op, const char *field, const char *literal) {
  xmlTextWriterStartElementNS(w, X("ogc"), X(op), NULL);
  xmlTextWriterWriteElementNS(w, X("ogc"), X("PropertyName"), NULL, X(field));
  xmlTextWriterWriteElementNS(w, X("ogc"), X("Literal"), NULL, X(literal));
  xmlTextWriterEndElement(w);
}

char *osmium_style_to_sld(const OsmiumStyle *style, const char *layer_name) {
  xmlBufferPtr buffer = xmlBufferCreate();
  xmlTextWriterPtr w = xmlNewTextWriterMemory(buffer, 0);
  const char *name = layer_name != NULL && layer_name[0] != '\0' ? layer_name : "layer";
  char *result;

  xmlTextWriterSetIndent(w, 1);
  xmlTextWriterStartDocument(w, NULL, "UTF-8", NULL);
  xmlTextWriterStartElement(w, X("StyledLayerDescriptor"));
  xmlTextWriterWriteAttribute(w, X("version"), X("1.0.0"));
  xmlTextWriterWriteAttribute(w, X("xmlns"), X(NS_SLD));
  xmlTextWriterWriteAttribute(w, X("xmlns:ogc"), X(NS_OGC));
  xmlTextWriterWriteAttribute(w, X("xmlns:xlink"), X("http://www.w3.org/1999/xlink"));
  xmlTextWriterStartElement(w, X("NamedLayer"));
  xmlTextWriterWriteElement(w, X("Name"), X(name));
  xmlTextWriterStartElement(w, X("UserStyle"));
  xmlTextWriterWriteElement(w, X("Name"), X(name));
  xmlTextWriterStartElement(w, X("FeatureTypeStyle"));

  if (style->kind == OSMIUM_STYLE_CATEGORIZED && style->field != NULL) {
    for (guint i = 0; i < style->categories->len; i++) {
      const OsmiumStyleCategory *c = &g_array_index(style->categories, OsmiumStyleCategory, i);

      if (!c->visible) {
        continue;
      }
      xmlTextWriterStartElement(w, X("Rule"));
      xmlTextWriterWriteElement(w, X("Name"), X(c->label != NULL ? c->label : ""));
      xmlTextWriterWriteElement(w, X("Title"), X(c->label != NULL ? c->label : ""));
      if (c->value != NULL) {
        xmlTextWriterStartElementNS(w, X("ogc"), X("Filter"), NULL);
        sld_property(w, "PropertyIsEqualTo", style->field, c->value);
        xmlTextWriterEndElement(w);
      } else {
        xmlTextWriterStartElementNS(w, X("ogc"), X("ElseFilter"), NULL);
        xmlTextWriterEndElement(w);
      }
      sld_symbolizers(w, &c->symbol);
      xmlTextWriterEndElement(w);
    }
  } else if (style->kind == OSMIUM_STYLE_GRADUATED && style->field != NULL) {
    for (guint i = 0; i < style->classes->len; i++) {
      const OsmiumStyleClass *c = &g_array_index(style->classes, OsmiumStyleClass, i);
      g_autofree char *lower = format_double(c->lower);
      g_autofree char *upper = format_double(c->upper);

      if (!c->visible) {
        continue;
      }
      xmlTextWriterStartElement(w, X("Rule"));
      xmlTextWriterWriteElement(w, X("Name"), X(c->label != NULL ? c->label : ""));
      xmlTextWriterWriteElement(w, X("Title"), X(c->label != NULL ? c->label : ""));
      xmlTextWriterStartElementNS(w, X("ogc"), X("Filter"), NULL);
      xmlTextWriterStartElementNS(w, X("ogc"), X("And"), NULL);
      sld_property(w, "PropertyIsGreaterThanOrEqualTo", style->field, lower);
      sld_property(w, i + 1 == style->classes->len ? "PropertyIsLessThanOrEqualTo" : "PropertyIsLessThan", style->field, upper);
      xmlTextWriterEndElement(w);
      xmlTextWriterEndElement(w);
      sld_symbolizers(w, &c->symbol);
      xmlTextWriterEndElement(w);
    }
  } else {
    xmlTextWriterStartElement(w, X("Rule"));
    xmlTextWriterWriteElement(w, X("Name"), X("default"));
    sld_symbolizers(w, &style->symbol);
    xmlTextWriterEndElement(w);
  }

  if (style->labels.enabled && style->labels.field != NULL) {
    const OsmiumStyleLabels *l = &style->labels;
    g_autofree char *hex = rgb_to_hex(&l->color);
    g_autofree char *halo = rgb_to_hex(&l->halo_color);
    g_autofree char *size = format_double(l->size);

    xmlTextWriterStartElement(w, X("Rule"));
    xmlTextWriterWriteElement(w, X("Name"), X("labels"));
    xmlTextWriterStartElement(w, X("TextSymbolizer"));
    xmlTextWriterStartElement(w, X("Label"));
    xmlTextWriterWriteElementNS(w, X("ogc"), X("PropertyName"), NULL, X(l->field));
    xmlTextWriterEndElement(w);
    xmlTextWriterStartElement(w, X("Font"));
    sld_css(w, "font-family", l->font_family != NULL ? l->font_family : "Sans");
    sld_css(w, "font-size", size);
    xmlTextWriterEndElement(w);
    if (l->halo_width > 0.0) {
      g_autofree char *radius = format_double(l->halo_width);

      xmlTextWriterStartElement(w, X("Halo"));
      xmlTextWriterWriteElement(w, X("Radius"), X(radius));
      xmlTextWriterStartElement(w, X("Fill"));
      sld_css(w, "fill", halo);
      sld_css_double(w, "fill-opacity", l->halo_color.alpha);
      xmlTextWriterEndElement(w);
      xmlTextWriterEndElement(w);
    }
    xmlTextWriterStartElement(w, X("Fill"));
    sld_css(w, "fill", hex);
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w);
  }

  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  result = g_strdup(S(xmlBufferContent(buffer)));
  xmlBufferFree(buffer);
  return result;
}

/* --- SLD import ------------------------------------------------------------------------------ */

static char *css_param(xmlNodePtr parent, const char *name) {
  for (xmlNodePtr n = parent != NULL ? parent->children : NULL; n != NULL; n = n->next) {
    if (n->type != XML_ELEMENT_NODE ||
        (g_strcmp0(S(n->name), "CssParameter") != 0 && g_strcmp0(S(n->name), "SvgParameter") != 0)) {
      continue;
    }
    g_autofree char *key = attr(n, "name");

    if (g_strcmp0(key, name) == 0) {
      return node_text(n);
    }
  }
  return NULL;
}

static void sld_read_fill(xmlNodePtr fill, GdkRGBA *color) {
  g_autofree char *value = css_param(fill, "fill");
  g_autofree char *opacity = css_param(fill, "fill-opacity");

  if (value != NULL) {
    rgba_from_hex(value, parse_double(opacity, 1.0), color);
  } else if (fill != NULL) {
    color->alpha = parse_double(opacity, 1.0);
  }
}

static void sld_read_stroke(xmlNodePtr stroke, OsmiumSymbol *s) {
  g_autofree char *value = css_param(stroke, "stroke");
  g_autofree char *opacity = css_param(stroke, "stroke-opacity");
  g_autofree char *width = css_param(stroke, "stroke-width");
  g_autofree char *dash = css_param(stroke, "stroke-dasharray");

  if (stroke == NULL) {
    return;
  }
  if (value != NULL) {
    rgba_from_hex(value, parse_double(opacity, 1.0), &s->stroke);
  }
  s->line_width = parse_double(width, 1.0);
  if (dash != NULL) {
    g_auto(GStrv) parts = g_strsplit_set(dash, " ,", -1);
    guint n = g_strv_length(parts);

    s->line_style = n >= 4 ? OSMIUM_LINE_DASH_DOT : (n >= 2 && parse_double(parts[0], 0) <= parse_double(parts[1], 0) * 0.6 ? OSMIUM_LINE_DOT : OSMIUM_LINE_DASH);
  }
}

static void sld_read_rule_symbol(xmlNodePtr rule, OsmiumSymbol *s) {
  xmlNodePtr polygon = first_child(rule, "PolygonSymbolizer");
  xmlNodePtr line = first_child(rule, "LineSymbolizer");
  xmlNodePtr point = first_child(rule, "PointSymbolizer");

  osmium_symbol_init(s);
  if (polygon != NULL) {
    sld_read_fill(first_child(polygon, "Fill"), &s->fill);
    sld_read_stroke(first_child(polygon, "Stroke"), s);
  } else if (line != NULL) {
    sld_read_stroke(first_child(line, "Stroke"), s);
  }
  if (point != NULL) {
    xmlNodePtr mark = find_descendant(point, "Mark");
    xmlNodePtr size = find_descendant(point, "Size");
    g_autofree char *shape = node_text(first_child(mark, "WellKnownName"));
    g_autofree char *size_text = node_text(size);

    sld_read_fill(first_child(mark, "Fill"), &s->fill);
    sld_read_stroke(first_child(mark, "Stroke"), s);
    s->shape = shape_from_name(shape);
    s->point_radius = MAX(parse_double(size_text, 8.0) / 2.0, 1.0);
  } else if (polygon == NULL && line == NULL) {
    return;
  }
}

typedef struct {
  char *field;
  char *equal;
  gboolean has_lower;
  gboolean has_upper;
  double lower;
  double upper;
  gboolean is_else;
} RuleFilter;

static void filter_clear(RuleFilter *f) {
  g_free(f->field);
  g_free(f->equal);
}

static void read_comparison(xmlNodePtr node, RuleFilter *filter) {
  const char *name = S(node->name);
  g_autofree char *field = node_text(first_child(node, "PropertyName"));
  g_autofree char *literal = node_text(first_child(node, "Literal"));

  if (field == NULL) {
    return;
  }
  if (g_strcmp0(name, "PropertyIsEqualTo") == 0) {
    g_free(filter->field);
    g_free(filter->equal);
    filter->field = g_strdup(field);
    filter->equal = g_strdup(literal);
  } else if (g_strcmp0(name, "PropertyIsGreaterThanOrEqualTo") == 0 || g_strcmp0(name, "PropertyIsGreaterThan") == 0) {
    g_free(filter->field);
    filter->field = g_strdup(field);
    filter->has_lower = TRUE;
    filter->lower = parse_double(literal, 0.0);
  } else if (g_strcmp0(name, "PropertyIsLessThan") == 0 || g_strcmp0(name, "PropertyIsLessThanOrEqualTo") == 0) {
    g_free(filter->field);
    filter->field = g_strdup(field);
    filter->has_upper = TRUE;
    filter->upper = parse_double(literal, 0.0);
  } else if (g_strcmp0(name, "PropertyIsBetween") == 0) {
    g_autofree char *lower = node_text(find_descendant(first_child(node, "LowerBoundary"), "Literal"));
    g_autofree char *upper = node_text(find_descendant(first_child(node, "UpperBoundary"), "Literal"));

    g_free(filter->field);
    filter->field = g_strdup(field);
    filter->has_lower = filter->has_upper = TRUE;
    filter->lower = parse_double(lower, 0.0);
    filter->upper = parse_double(upper, 0.0);
  }
}

static void read_filter(xmlNodePtr filter_node, RuleFilter *filter) {
  for (xmlNodePtr n = filter_node != NULL ? filter_node->children : NULL; n != NULL; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      continue;
    }
    if (g_strcmp0(S(n->name), "And") == 0) {
      read_filter(n, filter);
    } else {
      read_comparison(n, filter);
    }
  }
}

gboolean osmium_style_from_sld(OsmiumStyle *style, const char *xml, GError **error) {
  xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  xmlNodePtr root;
  xmlNodePtr fts;
  OsmiumStyle *parsed;
  guint n_rules = 0;
  gboolean categorized = FALSE;
  gboolean graduated = FALSE;
  g_autofree char *field = NULL;

  if (doc == NULL || (root = xmlDocGetRootElement(doc)) == NULL || g_strcmp0(S(root->name), "StyledLayerDescriptor") != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This is not an SLD style file");
    xmlFreeDoc(doc);
    return FALSE;
  }

  fts = find_descendant(root, "FeatureTypeStyle");
  parsed = osmium_style_new();
  parsed->symbol = style->symbol;

  for (xmlNodePtr rule = fts != NULL ? fts->children : NULL; rule != NULL; rule = rule->next) {
    if (rule->type != XML_ELEMENT_NODE || g_strcmp0(S(rule->name), "Rule") != 0) {
      continue;
    }

    xmlNodePtr text = first_child(rule, "TextSymbolizer");

    if (text != NULL) {
      g_autofree char *label_field = node_text(find_descendant(first_child(text, "Label"), "PropertyName"));
      xmlNodePtr font = first_child(text, "Font");
      g_autofree char *family = css_param(font, "font-family");
      g_autofree char *size = css_param(font, "font-size");
      xmlNodePtr halo = first_child(text, "Halo");

      if (label_field != NULL) {
        parsed->labels.enabled = TRUE;
        osmium_style_set_label_field(parsed, label_field);
        osmium_style_set_label_font(parsed, family);
        parsed->labels.size = parse_double(size, 11.0);
        sld_read_fill(first_child(text, "Fill"), &parsed->labels.color);
        if (halo != NULL) {
          g_autofree char *radius = node_text(first_child(halo, "Radius"));

          parsed->labels.halo_width = parse_double(radius, 1.5);
          sld_read_fill(first_child(halo, "Fill"), &parsed->labels.halo_color);
        } else {
          parsed->labels.halo_width = 0.0;
        }
      }
      if (first_child(rule, "PolygonSymbolizer") == NULL && first_child(rule, "LineSymbolizer") == NULL &&
          first_child(rule, "PointSymbolizer") == NULL) {
        continue;
      }
    }

    OsmiumSymbol symbol;
    RuleFilter filter = {0};
    g_autofree char *title = node_text(first_child(rule, "Title"));
    g_autofree char *name = node_text(first_child(rule, "Name"));
    const char *label = title != NULL && title[0] != '\0' ? title : name;

    sld_read_rule_symbol(rule, &symbol);
    read_filter(first_child(rule, "Filter"), &filter);
    filter.is_else = first_child(rule, "ElseFilter") != NULL;

    if (n_rules == 0 && filter.field == NULL && !filter.is_else) {
      parsed->symbol = symbol;
    }

    if (filter.field != NULL && filter.equal != NULL && !graduated) {
      categorized = TRUE;
      if (field == NULL) {
        field = g_strdup(filter.field);
      }
      osmium_style_add_category(parsed, filter.equal, label, &symbol);
    } else if (filter.field != NULL && filter.has_lower && filter.has_upper && !categorized) {
      graduated = TRUE;
      if (field == NULL) {
        field = g_strdup(filter.field);
      }
      osmium_style_add_class(parsed, filter.lower, filter.upper, label, &symbol);
    } else if (filter.is_else && !graduated) {
      osmium_style_add_category(parsed, NULL, label != NULL ? label : "all other values", &symbol);
    }
    n_rules++;
    filter_clear(&filter);
  }

  if (n_rules == 0 && !parsed->labels.enabled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The file has no supported rules");
    osmium_style_free(parsed);
    xmlFreeDoc(doc);
    return FALSE;
  }

  if (graduated && parsed->classes->len > 0) {
    parsed->kind = OSMIUM_STYLE_GRADUATED;
    osmium_style_set_field(parsed, field);
  } else if (categorized && parsed->categories->len > 0) {
    parsed->kind = OSMIUM_STYLE_CATEGORIZED;
    osmium_style_set_field(parsed, field);
  } else {
    osmium_style_clear_categories(parsed);
  }

  {
    OsmiumStyle swap = *style;

    *style = *parsed;
    *parsed = swap;
  }
  osmium_style_free(parsed);
  xmlFreeDoc(doc);
  return TRUE;
}
